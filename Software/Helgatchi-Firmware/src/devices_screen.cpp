#include "devices_screen.h"
#include "scan_service.h"
#include "scan_types.h"
#include "vendor_lookup.h"
#include "event_ids.h"
#include "UI/screens.h"
#include "UI/styles.h"
#include "UI/eez-flow.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

DevicesScreen g_devices_screen;

// ---------------------------------------------------------------------------
// Device list — dynamic cards rendered into objects.devices_container.
//
// Architecture mirrors AlertsScreen: EEZ owns the visuals (Device user widget
// design + "Device Card" style + container flex layout); C owns the data
// flow. We build one LVGL card per unique device, replicating
// create_user_widget_device() in src/UI/screens.c (icon / name / MFG /
// RSSI+age / MAC labels) using the EEZ-exported style so visuals match the
// design exactly.
//
// Refresh cadence: EV_SCAN_COMPLETE only. Diff is by (domain, MAC) — matched
// cards update in place, new devices get a card, evicted devices are deleted,
// then the list is reordered RSSI-strongest-first. See the header for the
// rationale (focus/scroll preservation).
// ---------------------------------------------------------------------------

struct DeviceCard {
    lv_obj_t* card         = nullptr;
    lv_obj_t* icon_label   = nullptr;
    lv_obj_t* name_label   = nullptr;
    lv_obj_t* mfg_label    = nullptr;
    lv_obj_t* rssi_label   = nullptr;   // combined "-43dBm 32s ago"
    lv_obj_t* mac_label    = nullptr;
    uint8_t   domain       = 0;
    uint8_t   mac[6]       = {0};
    int8_t    rssi         = 0;
    uint32_t  last_seen_ms = 0;         // cached for the live age tick
};

static DeviceCard  _cards[ScanService::SEEN_CAPACITY];
static uint16_t    _card_count = 0;
static lv_timer_t* _age_timer  = nullptr;

// Incremental build state. The seen-map can hold up to SEEN_CAPACITY devices;
// building all their cards (~6 LVGL objects each) in one pass would exhaust the
// LVGL pool mid-build → lv_malloc assert → LV_ASSERT_HANDLER `while(1)` halt
// (a silent freeze). So a pass creates at most NEW_CARDS_PER_PASS new cards and,
// if more remain, re-arms _build_timer to continue — spreading the work across
// loop iterations so the pool is never slammed and the loop never blocks. The
// build only runs while the devices screen is actually showing (see _kickBuild);
// off-screen scans just set _dirty and defer the work to the next screen load.
static bool        _dirty       = false;   // seen-map changed; list needs a (re)build
static lv_timer_t* _build_timer = nullptr; // drives incremental passes; null when idle
static constexpr uint16_t NEW_CARDS_PER_PASS = 8;
static constexpr uint32_t BUILD_PASS_MS      = 20;   // gap between incremental passes

// Devices unseen for this long fall off the list; their cards are deleted.
// (BLE MAC randomization means new "devices" appear constantly — without this,
// cards accumulate every scan and LV_MEM climbs until the pool is exhausted.)
static constexpr uint32_t DEVICE_LIST_AGE_MS = 5UL * 60UL * 1000UL;   // 5 min

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

static void _formatTimeAgo(char* buf, size_t buf_sz, uint32_t age_ms) {
    const uint32_t s = age_ms / 1000;
    if      (s < 60)   snprintf(buf, buf_sz, "%us ago",     (unsigned)s);
    else if (s < 3600) snprintf(buf, buf_sz, "%um %us ago", (unsigned)(s / 60),   (unsigned)(s % 60));
    else               snprintf(buf, buf_sz, "%uh %um ago", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

// BT SIG company name for BLE with a mfg id, else the OUI org for the MAC
// (covers BLE-without-mfg and WiFi). Empty when nothing resolves (e.g. a
// random/private BLE address whose OUI isn't in the IEEE table).
static const char* _resolveMfg(const ScanResult& r) {
    const char* org = nullptr;
    if (r.domain == SCAN_BLE && r.mfg_id != 0) org = vendor_mfg_lookup(r.mfg_id);
    if (!org) org = vendor_for_mac(r.mac);
    return org ? org : "";
}

static void _fmtMac(char* buf, size_t sz, const uint8_t mac[6]) {
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void _fmtRssi(char* buf, size_t sz, int8_t rssi, uint32_t last_seen_ms) {
    char ago[24];
    _formatTimeAgo(ago, sizeof(ago), millis() - last_seen_ms);
    snprintf(buf, sz, "%ddBm %s", (int)rssi, ago);
}

static bool _sameDevice(const DeviceCard& c, const ScanResult& r) {
    return c.domain == r.domain && memcmp(c.mac, r.mac, sizeof(c.mac)) == 0;
}

// ---------------------------------------------------------------------------
// Selection pin — keep focus on the same device across refreshes / reorders.
// ---------------------------------------------------------------------------

static void _populateNavGroup();   // fwd

static bool    _has_pin           = false;
static uint8_t _pin_domain        = 0;
static uint8_t _pin_mac[6]        = {0};
static bool    _suppress_focus_cb = false;   // set during programmatic focus

static void _setPinFromCard(lv_obj_t* card_obj) {
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].card == card_obj) {
            _pin_domain = _cards[i].domain;
            memcpy(_pin_mac, _cards[i].mac, 6);
            _has_pin = true;
            return;
        }
    }
}

// User moved focus (left/right) → remember which device is selected.
static void _cardFocusCb(lv_event_t* e) {
    if (_suppress_focus_cb) return;
    _setPinFromCard((lv_obj_t*)lv_event_get_target(e));
}

static void _capturePin() {
    if (lv_screen_active() != objects.devices) return;
    lv_obj_t* f = lv_group_get_focused(groups.UINavigation);
    if (f) _setPinFromCard(f);
}

static void _focusPin() {
    if (_card_count == 0) return;
    if (_has_pin) {
        for (uint16_t i = 0; i < _card_count; i++) {
            if (_cards[i].domain == _pin_domain && memcmp(_cards[i].mac, _pin_mac, 6) == 0) {
                lv_group_focus_obj(_cards[i].card);
                return;
            }
        }
    }
    lv_group_focus_obj(_cards[0].card);
}

// Rebuild the nav group to match visual order and restore focus to the pinned
// device, with the FOCUSED callback suppressed so the auto-focus during re-add
// doesn't clobber the pin.
static void _rebuildGroupAndFocus() {
    _suppress_focus_cb = true;
    _populateNavGroup();
    _focusPin();
    _suppress_focus_cb = false;
}

// ---------------------------------------------------------------------------
// Device detail popup (lv_msgbox modal on the top layer)
//
// NOTE: this is the one place UI is created in code, by explicit request —
// EEZ Studio can't build this popup. Opened by pressing center on a card;
// closed by press-and-hold center (handled generically in UIController, which
// closes any open msgbox backdrop). While open, the two footer buttons own the
// keypad group; on close (LV_EVENT_DELETE) we restore the card list.
// ---------------------------------------------------------------------------

static lv_obj_t* _msgbox     = nullptr;
static int8_t    _mb_focus   = -1;   // -1 = scrolling content; 0/1 = footer button index
static lv_obj_t* _mb_content = nullptr;
static lv_obj_t* _mb_btn[2]  = {nullptr, nullptr};
static constexpr int MB_SCROLL_STEP = 30;   // px scrolled per left/right in content

// Highlight the focused footer button (none while scrolling content). The
// "Focused - Button" style renders LV_STATE_FOCUS_KEY.
static void _mbHighlight() {
    for (int i = 0; i < 2; i++) {
        if (!_mb_btn[i]) continue;
        if (_mb_focus == i) lv_obj_add_state   (_mb_btn[i], LV_STATE_FOCUS_KEY);
        else                lv_obj_remove_state(_mb_btn[i], LV_STATE_FOCUS_KEY);
    }
}

// Right/down: scroll the content until its bottom, then step onto the buttons.
static void _mbNavRight() {
    if (_mb_focus < 0) {
        if (lv_obj_get_scroll_bottom(_mb_content) > 0)
            lv_obj_scroll_by(_mb_content, 0, -MB_SCROLL_STEP, LV_ANIM_ON);
        else { _mb_focus = 0; _mbHighlight(); }
    } else if (_mb_focus == 0) {
        _mb_focus = 1; _mbHighlight();
    }
    // on the last button: stay
}

// Left/up: step back through the buttons, then scroll the content up.
static void _mbNavLeft() {
    if (_mb_focus == 1)      { _mb_focus = 0;  _mbHighlight(); }
    else if (_mb_focus == 0) { _mb_focus = -1; _mbHighlight(); }
    else if (lv_obj_get_scroll_top(_mb_content) > 0) {
        lv_obj_scroll_by(_mb_content, 0, MB_SCROLL_STEP, LV_ANIM_ON);
    }
}

// Center: activate the focused button (actions TBD).
static void _mbEnter() {
    if (_mb_focus == 0) {
        // TODO: Lock on
    } else if (_mb_focus == 1) {
        // TODO: Create rules
    }
}

static void _restoreNavAsync(void* /*unused*/) {
    if (lv_screen_active() == objects.devices) _rebuildGroupAndFocus();
}

static void _msgboxDeleteCb(lv_event_t* /*e*/) {
    _msgbox     = nullptr;
    _mb_content = nullptr;
    _mb_btn[0]  = _mb_btn[1] = nullptr;
    _mb_focus   = -1;
    // Defer the group rebuild until after LVGL finishes deleting the msgbox
    // (its footer buttons are mid-teardown right now).
    lv_async_call(_restoreNavAsync, nullptr);
}

static void _appendUuid(char* buf, size_t sz, const uint8_t uuid[16]) {
    size_t p = strlen(buf);
    for (int b = 0; b < 16 && p + 2 < sz; b++) p += (size_t)snprintf(buf + p, sz - p, "%02X", uuid[b]);
}

// BLE base UUID (0000xxxx-0000-1000-8000-00805F9B34FB) in the byte order the
// scan engine stores (NimBLE value order, little-endian), with the 16-bit slot
// (bytes 12-13) zeroed.
static const uint8_t BLE_BASE_UUID_LE[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// If `uuid` is a 16-bit assigned UUID promoted onto the BLE base, return its
// short value (0..0xFFFF); otherwise -1 (a 32/128-bit UUID with no short form).
static int _uuid16(const uint8_t uuid[16]) {
    for (int i = 0; i < 16; i++) {
        if (i == 12 || i == 13) continue;   // the 16-bit value slot
        if (uuid[i] != BLE_BASE_UUID_LE[i]) return -1;
    }
    return uuid[12] | (uuid[13] << 8);
}

static void _openMsgbox(uint8_t domain, const uint8_t mac[6]) {
    if (_msgbox) return;
    const ScanResult* r = g_scan_service.findSeen(domain, mac);
    if (!r) return;

    lv_obj_t* mb = lv_msgbox_create(nullptr);   // NULL parent → modal on top layer
    lv_obj_set_size(mb, LV_PCT(85), LV_PCT(85));
    lv_obj_set_style_radius(mb, 20, LV_PART_MAIN | LV_STATE_DEFAULT);

    char macbuf[24];
    snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    lv_msgbox_add_title(mb, r->name[0] ? r->name : macbuf);

    // Content labels (compact font so the summary fits the 85% box).
    lv_obj_t* content = lv_msgbox_get_content(mb);
    lv_obj_set_style_text_font(content, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);

    const uint32_t now = millis();
    char first_ago[24], last_ago[24];
    _formatTimeAgo(first_ago, sizeof(first_ago), now - r->first_seen_ms);
    _formatTimeAgo(last_ago,  sizeof(last_ago),  now - r->timestamp_ms);
    const char* bt  = (r->mfg_id != 0) ? vendor_mfg_lookup(r->mfg_id) : nullptr;
    const char* oui = vendor_for_mac(r->mac);

    char line[128];
    snprintf(line, sizeof(line), "Name: %s",        r->name[0] ? r->name : "(none)");  lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "MAC: %s",         macbuf);                            lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "Address type: %s", macTypeName(r->mac_type));         lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "First seen: %s",  first_ago);                         lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "Last seen: %s",   last_ago);                          lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "RSSI: %d dBm",    (int)r->rssi);                      lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "MFG: %s",         bt  ? bt  : "-");                   lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "OUI MFG: %s",     oui ? oui : "-");                   lv_msgbox_add_text(mb, line);
    snprintf(line, sizeof(line), "Services: %u",    (unsigned)r->service_count);        lv_msgbox_add_text(mb, line);
    for (uint8_t s = 0; s < r->service_count; s++) {
        const int u16 = _uuid16(r->service_uuids[s]);
        if (u16 >= 0) {
            snprintf(line, sizeof(line), "  0x%04X", (unsigned)u16);
        } else {
            // 32/128-bit UUID with no short form — show it in full.
            line[0] = ' '; line[1] = ' '; line[2] = '\0';
            _appendUuid(line, sizeof(line), r->service_uuids[s]);
        }
        lv_msgbox_add_text(mb, line);
    }

    // Footer actions (behavior TBD).
    lv_obj_t* b_lock = lv_msgbox_add_footer_button(mb, "Lock on");
    lv_obj_t* b_rule = lv_msgbox_add_footer_button(mb, "Create rules");
    add_style_focused___button(b_lock);
    add_style_focused___button(b_rule);

    // Custom keypad nav (see _mbNav*): left/right scrolls the content to the
    // bottom, then steps through the buttons. We drive it ourselves, so clear
    // the group — UIController's PREV/NEXT become no-ops while the popup is up.
    lv_group_remove_all_objs(groups.UINavigation);
    _mb_content = content;
    _mb_btn[0]  = b_lock;
    _mb_btn[1]  = b_rule;
    _mb_focus   = -1;                       // start in content-scroll mode
    lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
    _mbHighlight();

    lv_obj_add_event_cb(mb, _msgboxDeleteCb, LV_EVENT_DELETE, nullptr);
    _msgbox = mb;
}

static void _cardClickedCb(lv_event_t* e) {
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].card == obj) { _openMsgbox(_cards[i].domain, _cards[i].mac); return; }
    }
}

// ---------------------------------------------------------------------------
// Card build / update
// ---------------------------------------------------------------------------

// Apply a device's data to an existing card's labels.
static void _updateCard(DeviceCard* c, const ScanResult& r) {
    c->rssi         = r.rssi;
    c->last_seen_ms = r.timestamp_ms;

    lv_label_set_text(c->name_label, r.name);            // "" when no adv name
    lv_label_set_text(c->mfg_label,  _resolveMfg(r));
    lv_label_set_text(c->icon_label,
                      r.domain == SCAN_BLE ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI);

    char buf[40];
    _fmtRssi(buf, sizeof(buf), r.rssi, r.timestamp_ms);
    lv_label_set_text(c->rssi_label, buf);
    _fmtMac(buf, sizeof(buf), r.mac);
    lv_label_set_text(c->mac_label, buf);
}

// Instantiate the EEZ-designed Device user widget, so its layout, fonts, and
// styles come straight from create_user_widget_device() in src/UI/screens.c —
// the single place to edit them. This code never authors graphics; it only
// spawns the widget and reads back its objects to populate.
//
// The generated builder stores its child pointers into
// objects[startWidgetIndex + 0..5]. The widget isn't placed on a screen (so it
// has no reserved slots) and is never ticked by EEZ, so we lend it slots [0..5]
// for the call, read the created objects back, then restore the originals. The
// index/field mapping matches the assignments in create_user_widget_device():
//   +0 panel, +1 icon, +2 name, +3 mfg, +4 rssi, +5 mac.
static void _buildCard(lv_obj_t* parent, const ScanResult& r, DeviceCard* out) {
    lv_obj_t** slots = (lv_obj_t**)&objects;
    lv_obj_t*  saved[6];
    for (int i = 0; i < 6; i++) saved[i] = slots[i];

    create_user_widget_device(parent, nullptr, 0);

    out->card       = slots[0];
    out->icon_label = slots[1];
    out->name_label = slots[2];
    out->mfg_label  = slots[3];
    out->rssi_label = slots[4];
    out->mac_label  = slots[5];

    for (int i = 0; i < 6; i++) slots[i] = saved[i];

    out->domain = r.domain;
    memcpy(out->mac, r.mac, sizeof(out->mac));

    // Behavior only (not layout): focus tracking for the selection pin, and
    // opening the detail popup on select. Size, styles, and scroll flags all
    // come from the widget definition in EEZ.
    lv_obj_add_flag(out->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(out->card, _cardFocusCb,   LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(out->card, _cardClickedCb, LV_EVENT_CLICKED, nullptr);

    _updateCard(out, r);
}

// ---------------------------------------------------------------------------
// Nav group + refresh
// ---------------------------------------------------------------------------

// Repopulate the keypad nav group with card objects in current visual order.
// Cards are the only focusable widgets on the devices screen (EEZ's screen-
// load handler clears the group first), so a wholesale rebuild is safe.
static void _populateNavGroup() {
    lv_group_remove_all_objs(groups.UINavigation);
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].card) lv_group_add_obj(groups.UINavigation, _cards[i].card);
    }
}

// Sort the first `n` entries of `order` (seen-map indices) RSSI-strongest-first
// (insertion sort). Caller fills `order` with the indices to sort.
static void _sortByRssi(uint16_t* order, uint16_t n) {
    for (uint16_t i = 1; i < n; i++) {
        const uint16_t key = order[i];
        const int8_t   kr  = g_scan_service.seenAt(key).rssi;
        int j = (int)i - 1;
        while (j >= 0 && g_scan_service.seenAt(order[j]).rssi < kr) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

// One incremental build/diff pass. Reuses an existing card for every still-
// present device (cheap — just label updates) and creates at most
// NEW_CARDS_PER_PASS *new* cards, then reorders RSSI-strongest-first. Returns
// true if desired devices remain unbuilt (the driver should run another pass).
//
// Capping only NEW creation is what keeps a pass bounded: a device deferred by
// the cap has no card yet, so the eviction sweep can't touch it, and the next
// pass picks it up (found-by-MAC is position-independent, so re-sorting between
// passes is harmless). Existing cards are always reconciled so RSSI/age stay live.
static bool _refreshPass() {
    if (!objects.devices_container) return false;

    _capturePin();   // remember the focused device so we can restore it below

    const uint16_t n   = (uint16_t)g_scan_service.seenCount();
    const uint32_t now = millis();

    static uint16_t   order[ScanService::SEEN_CAPACITY];
    static DeviceCard next[ScanService::SEEN_CAPACITY];

    // Age filter: only devices seen within the window make the list. Devices
    // that drop out simply aren't in `order`, so their cards go unclaimed and
    // are deleted by the sweep below.
    uint16_t m = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (now - g_scan_service.seenAt(i).timestamp_ms <= DEVICE_LIST_AGE_MS) order[m++] = i;
    }
    _sortByRssi(order, m);

    uint16_t next_count = 0;
    uint16_t new_built  = 0;
    bool     deferred   = false;

    // Walk devices strongest-first. Reuse an existing card for the same
    // (domain, MAC); otherwise build a fresh one, up to the per-pass cap. A
    // reused card is claimed by nulling its old slot so the eviction sweep skips it.
    for (uint16_t k = 0; k < m; k++) {
        const ScanResult& r = g_scan_service.seenAt(order[k]);
        int found = -1;
        for (uint16_t i = 0; i < _card_count; i++) {
            if (_cards[i].card && _sameDevice(_cards[i], r)) { found = (int)i; break; }
        }
        if (found >= 0) {
            _updateCard(&_cards[found], r);
            next[next_count++]  = _cards[found];
            _cards[found].card  = nullptr;   // claimed
        } else if (new_built < NEW_CARDS_PER_PASS) {
            DeviceCard c{};
            _buildCard(objects.devices_container, r, &c);
            next[next_count++] = c;
            new_built++;
        } else {
            deferred = true;   // over the cap — pick this device up next pass
        }
    }

    // Unclaimed old cards → their device was evicted from the map. Delete.
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].card && lv_obj_is_valid(_cards[i].card)) {
            lv_obj_del(_cards[i].card);
        }
    }

    memcpy(_cards, next, sizeof(DeviceCard) * next_count);
    _card_count = next_count;

    // Reorder LVGL children so the flex column matches the RSSI sort.
    for (uint16_t i = 0; i < _card_count; i++) {
        lv_obj_move_to_index(_cards[i].card, i);
    }

    LV_LOG_USER("devices: pass +%u new, %u shown of %u%s",
                (unsigned)new_built, (unsigned)_card_count, (unsigned)m,
                deferred ? " (more pending)" : " (done)");

    // Restore keypad focus only once the list is complete, to avoid focus
    // hopping while cards stream in. The detail popup owns the group when open.
    if (!deferred && lv_screen_active() == objects.devices && !_msgbox) {
        _rebuildGroupAndFocus();
    }
    return deferred;
}

static void _stopBuildTimer() {
    if (_build_timer) { lv_timer_delete(_build_timer); _build_timer = nullptr; }
}

// Timer-driven continuation of an incremental build. Pauses (leaving _dirty
// set) whenever the devices screen isn't showing — no reason to spend the LVGL
// pool on a list nobody's looking at; it resumes on the next screen load.
static void _buildTimerCb(lv_timer_t* /*t*/) {
    if (lv_screen_active() != objects.devices) { _stopBuildTimer(); return; }
    if (!_refreshPass()) {          // caught up
        _dirty = false;
        _stopBuildTimer();
    }
}

// Start (or ensure running) the incremental build. Does an immediate first pass
// for snappiness, then arms the timer if devices remain. Callers decide whether
// building is appropriate (screen-load always; EV_SCAN_COMPLETE only while the
// devices screen shows) — this doesn't re-check the active screen so it works
// during the load transition, when lv_screen_active() may not have flipped yet.
static void _kickBuild() {
    if (_build_timer) return;                            // already building
    if (_refreshPass()) {
        _build_timer = lv_timer_create(_buildTimerCb, BUILD_PASS_MS, nullptr);
    } else {
        _dirty = false;
    }
}

static void _ageTimerCb(lv_timer_t* /*t*/) {
    char buf[40];
    for (uint16_t i = 0; i < _card_count; i++) {
        if (!_cards[i].rssi_label) continue;
        _fmtRssi(buf, sizeof(buf), _cards[i].rssi, _cards[i].last_seen_ms);
        lv_label_set_text(_cards[i].rssi_label, buf);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

uint16_t DevicesScreen::cardCount() const { return _card_count; }

void DevicesScreen::begin(EventBus& bus) {
    bus.subscribe(EV_SCAN_COMPLETE, this);
    // Button events drive the detail popup's custom scroll/nav (only while it's
    // open — cards use the LVGL group via UIController otherwise).
    bus.subscribe(EV_BTN_LEFT,         this);
    bus.subscribe(EV_BTN_RIGHT,        this);
    bus.subscribe(EV_BTN_CENTER_SHORT, this);

    _age_timer = lv_timer_create(_ageTimerCb, 1000, nullptr);

    // Don't build at boot — the devices screen isn't showing yet, and building
    // off-screen is exactly the freeze path. Mark dirty so the first time the
    // screen loads it reconciles against whatever the seen map holds by then.
    _dirty = true;

    // On screen load: kick the (incremental) build if the list is stale,
    // otherwise just restore keypad focus. EEZ's own handler clears the group first.
    if (objects.devices) {
        lv_obj_add_event_cb(objects.devices, [](lv_event_t* /*e*/) {
            if (_dirty) _kickBuild();       // (re)build the list for this screen view
            _rebuildGroupAndFocus();        // always restore keypad nav on entry
        }, LV_EVENT_SCREEN_LOAD_START, nullptr);
    }
}

void DevicesScreen::onEvent(const Event& e) {
    switch (e.id) {
        case EV_SCAN_COMPLETE:
            // Mark the list stale; build incrementally only if the devices
            // screen is actually showing. Off-screen we just stay dirty and let
            // the next screen load build it — so the whole seen-map is never
            // built in one blocking shot, and never built when nobody's looking.
            _dirty = true;
            if (lv_screen_active() == objects.devices) _kickBuild();
            break;

        // Popup navigation. Only act while the popup is open; otherwise the
        // card list is driven by the LVGL group through UIController.
        case EV_BTN_LEFT:         if (_msgbox) _mbNavLeft();  break;
        case EV_BTN_RIGHT:        if (_msgbox) _mbNavRight(); break;
        case EV_BTN_CENTER_SHORT: if (_msgbox) _mbEnter();    break;

        default:
            break;
    }
}
