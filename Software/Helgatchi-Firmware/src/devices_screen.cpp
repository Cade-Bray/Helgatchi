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

    // Behavior only (not layout): focus tracking for the selection pin. Size,
    // styles, and scroll flags all come from the widget definition in EEZ.
    lv_obj_add_event_cb(out->card, _cardFocusCb, LV_EVENT_FOCUSED, nullptr);

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
        const int8_t   kr  = g_scan.seenAt(key).rssi;
        int j = (int)i - 1;
        while (j >= 0 && g_scan.seenAt(order[j]).rssi < kr) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

// Diff the current seen-map against the on-screen cards, then reorder.
static void _refresh() {
    if (!objects.devices_container) return;

    _capturePin();   // remember the focused device so we can restore it below

    const uint16_t n   = (uint16_t)g_scan.seenCount();
    const uint32_t now = millis();

    static uint16_t   order[ScanService::SEEN_CAPACITY];
    static DeviceCard next[ScanService::SEEN_CAPACITY];

    // Age filter: only devices seen within the window make the list. Devices
    // that drop out simply aren't in `order`, so their cards go unclaimed and
    // are deleted by the sweep below.
    uint16_t m = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (now - g_scan.seenAt(i).timestamp_ms <= DEVICE_LIST_AGE_MS) order[m++] = i;
    }
    _sortByRssi(order, m);
    uint16_t next_count = 0;

    // Walk devices strongest-first, reusing an existing card for the same
    // (domain, MAC) or building a fresh one. A reused card is claimed by
    // nulling its old slot's pointer so the eviction sweep below skips it.
    for (uint16_t k = 0; k < m; k++) {
        const ScanResult& r = g_scan.seenAt(order[k]);
        int found = -1;
        for (uint16_t i = 0; i < _card_count; i++) {
            if (_cards[i].card && _sameDevice(_cards[i], r)) { found = (int)i; break; }
        }
        if (found >= 0) {
            _updateCard(&_cards[found], r);
            next[next_count++]  = _cards[found];
            _cards[found].card  = nullptr;   // claimed
        } else {
            DeviceCard c{};
            _buildCard(objects.devices_container, r, &c);
            next[next_count++] = c;
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

    if (lv_screen_active() == objects.devices) _rebuildGroupAndFocus();
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

void DevicesScreen::begin(EventBus& bus) {
    bus.subscribe(EV_SCAN_COMPLETE, this);

    _age_timer = lv_timer_create(_ageTimerCb, 1000, nullptr);

    // Initial build from whatever's already in the seen map (a scan may have
    // run before this screen was first viewed).
    _refresh();

    // Repopulate the keypad nav group with cards when the devices screen loads
    // (EEZ's own handler clears the group first).
    if (objects.devices) {
        lv_obj_add_event_cb(objects.devices, [](lv_event_t* /*e*/) {
            _rebuildGroupAndFocus();
        }, LV_EVENT_SCREEN_LOAD_START, nullptr);
    }
}

void DevicesScreen::onEvent(const Event& e) {
    switch (e.id) {
        case EV_SCAN_COMPLETE:
            _refresh();
            break;
        default:
            break;
    }
}
