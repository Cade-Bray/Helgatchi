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

// The "Device Card" style defines only padding; the EEZ widget relied on
// LV_PCT(100) height inside a fixed slot. Two text rows (Montserrat 16 over
// 12) plus padding fit in ~44 px.
static constexpr int DEVICE_CARD_H = 44;

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

// Builds one device card mirroring create_user_widget_device() in
// src/UI/screens.c — same label positions, fonts, alignments, accent color.
static void _buildCard(lv_obj_t* parent, const ScanResult& r, DeviceCard* out) {
    const uint32_t theme  = eez_flow_get_selected_theme_index();
    const lv_color_t acc  = lv_color_hex(theme_colors[theme][2]);

    lv_obj_t* card = lv_obj_create(parent);
    add_style_device_card(card);
    lv_obj_set_size(card, LV_PCT(100), DEVICE_CARD_H);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Device icon — top-left, Montserrat 16.
    lv_obj_t* icon = lv_label_create(card);
    lv_obj_set_pos(icon, 3, 1);
    lv_obj_set_size(icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(icon, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Device name — top-left after the icon, Montserrat 16.
    lv_obj_t* name = lv_label_create(card);
    lv_obj_set_pos(name, 17, 1);
    lv_obj_set_size(name, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(name, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(name, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // MFG — top-right, Montserrat 12, accent color.
    lv_obj_t* mfg = lv_label_create(card);
    lv_obj_set_pos(mfg, -3, 1);
    lv_obj_set_size(mfg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(mfg, LV_ALIGN_TOP_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(mfg, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(mfg, acc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(mfg, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);

    // RSSI + age — bottom-right, Montserrat 12, accent color.
    lv_obj_t* rssi = lv_label_create(card);
    lv_obj_set_pos(rssi, -3, -1);
    lv_obj_set_size(rssi, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(rssi, LV_ALIGN_BOTTOM_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(rssi, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(rssi, acc, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(rssi, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);

    // MAC — bottom-left, Montserrat 12, accent color.
    lv_obj_t* mac = lv_label_create(card);
    lv_obj_set_pos(mac, 3, -1);
    lv_obj_set_size(mac, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(mac, LV_ALIGN_BOTTOM_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(mac, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(mac, acc, LV_PART_MAIN | LV_STATE_DEFAULT);

    out->card       = card;
    out->icon_label = icon;
    out->name_label = name;
    out->mfg_label  = mfg;
    out->rssi_label = rssi;
    out->mac_label  = mac;
    out->domain     = r.domain;
    memcpy(out->mac, r.mac, sizeof(out->mac));

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

// Sort seen-map indices RSSI-strongest-first (insertion sort; n <= 128).
static void _sortByRssi(uint16_t* order, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) order[i] = i;
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

    const uint16_t n = (uint16_t)g_scan.seenCount();

    static uint16_t   order[ScanService::SEEN_CAPACITY];
    static DeviceCard next[ScanService::SEEN_CAPACITY];
    _sortByRssi(order, n);
    uint16_t next_count = 0;

    // Walk devices strongest-first, reusing an existing card for the same
    // (domain, MAC) or building a fresh one. A reused card is claimed by
    // nulling its old slot's pointer so the eviction sweep below skips it.
    for (uint16_t k = 0; k < n; k++) {
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

    if (lv_screen_active() == objects.devices) _populateNavGroup();
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
            _populateNavGroup();
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
