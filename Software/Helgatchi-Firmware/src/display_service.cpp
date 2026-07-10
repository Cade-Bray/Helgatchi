#include "display_service.h"
#include "hal.h"
#include "power_manager.h"
#include "settings_service.h"
#include "alerts_service.h"
#include "UI/vars.h"
#include "UI/screens.h"     // objects.* + theme_colors[]
#include "UI/eez-flow.h"
#include <lvgl.h>
#include <stdio.h>

DisplayService g_display;

// Sets the EEZ global variable that every top bar's right_text expression
// references. EEZ Flow propagates it to all screens automatically.
static void _refreshBatteryStatus(uint16_t mv, uint8_t pct) {
    if (pct == 0xFF) {
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BATTERY_STATUS, eez::StringValue(""));
        return;
    }

    // Bucket hysteresis. Climbing is immediate; dropping requires HYST_PP below
    // the current bucket's lower edge to avoid flicker at boundaries.
    static uint8_t _last_bucket = 0xFF;
    constexpr uint8_t HYST_PP = 5;

    if (pct == BATT_PCT_MISSING) {
        _last_bucket = 0xFF;
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BATTERY_STATUS,
                               eez::StringValue(LV_SYMBOL_WARNING LV_SYMBOL_BATTERY_EMPTY));
        return;
    }

    uint8_t level = (pct > 100) ? pmBattPctFromVsenseMv((uint16_t)(mv / 2)) : pct;

    uint8_t cand;
    if      (level >= 80) cand = 4;
    else if (level >= 60) cand = 3;
    else if (level >= 40) cand = 2;
    else if (level >= 20) cand = 1;
    else                  cand = 0;

    uint8_t bucket;
    if (_last_bucket == 0xFF || cand >= _last_bucket) {
        bucket = cand;
    } else {
        uint8_t lower_edge = (uint8_t)(_last_bucket * 20);
        bucket = ((uint16_t)level + HYST_PP < lower_edge) ? cand : _last_bucket;
    }
    _last_bucket = bucket;

    const char* batt_sym;
    switch (bucket) {
        case 4:  batt_sym = LV_SYMBOL_BATTERY_FULL;  break;
        case 3:  batt_sym = LV_SYMBOL_BATTERY_3;     break;
        case 2:  batt_sym = LV_SYMBOL_BATTERY_2;     break;
        case 1:  batt_sym = LV_SYMBOL_BATTERY_1;     break;
        default: batt_sym = LV_SYMBOL_BATTERY_EMPTY; break;
    }

    const char* prefix = "";
    if      ((bool)Serial)                                          prefix = LV_SYMBOL_KEYBOARD;
    else if (g_hal.usbAttached())                                   prefix = LV_SYMBOL_USB;
    else if (pct == BATT_PCT_CHARGING || pct == BATT_PCT_CHARGED)  prefix = LV_SYMBOL_CHARGE;

    char buf[16];
    snprintf(buf, sizeof(buf), "%s%s", prefix, batt_sym);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_BATTERY_STATUS, eez::StringValue(buf));
}

// Apply `color` to the top-bar Left Text label (the BT / WiFi / Bell glyphs)
// somewhere under `obj`. The icons are font glyphs, so this is a plain text-
// color property set. The top bar is the only widget whose container holds
// exactly three labels aligned LEFT / CENTER / RIGHT; that signature locates it
// without naming per-screen objects, so the color follows every screen's top
// bar — including screens added later. Recurses; stops once the bar is found.
static bool _colorTopBarIcons(lv_obj_t* obj, lv_color_t color) {
    const uint32_t n = lv_obj_get_child_count(obj);
    if (n == 3) {
        lv_obj_t* left = nullptr;
        bool center = false, right = false, all_labels = true;
        for (uint32_t i = 0; i < 3; i++) {
            lv_obj_t* c = lv_obj_get_child(obj, i);
            if (!lv_obj_check_type(c, &lv_label_class)) { all_labels = false; break; }
            switch (lv_obj_get_style_align(c, LV_PART_MAIN)) {
                case LV_ALIGN_LEFT_MID:  left   = c;    break;
                case LV_ALIGN_CENTER:    center = true; break;
                case LV_ALIGN_RIGHT_MID: right  = true; break;
                default: break;
            }
        }
        if (all_labels && left && center && right) {
            lv_obj_set_style_text_color(left, color, LV_PART_MAIN | LV_STATE_DEFAULT);
            return true;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (_colorTopBarIcons(lv_obj_get_child(obj, i), color)) return true;
    }
    return false;
}

// Recolor the status icons on every screen's top bar (not just the active one),
// so the color is already correct when the user navigates. Screens occupy the
// first _SCREEN_ID_LAST slots of the objects struct, in ScreensEnum order.
static void _setStatusIconColor(lv_color_t color) {
    lv_obj_t* const* screens = (lv_obj_t* const*)&objects;
    for (int i = 0; i < _SCREEN_ID_LAST; i++) {
        if (screens[i]) _colorTopBarIcons(screens[i], color);
    }
}

void DisplayService::refreshStatusIcons() {
    // Scanning color is the EEZ theme accent while a scan window is open, else
    // white. Set as the label's text-color property (not baked into the text),
    // swept across every top bar — no per-screen object list.
    const uint32_t hex = _scanning
        ? (theme_colors[eez_flow_get_selected_theme_index()][0] & 0xFFFFFFu)
        : 0xFFFFFFu;
    _setStatusIconColor(lv_color_hex(hex));

    // Status-bar icon order (matches the Settings screen): Bluetooth, WiFi,
    // then Bell when any alert is active. ScanMode is a bitmask where bit 0
    // is BLE and bit 1 is WiFi, so we emit each independently.
    char buf[16] = {0};
    char* p      = buf;
    char* end    = buf + sizeof(buf);

    const uint32_t mode = g_settings.get(SKEY_SCAN_MODE);
    if (mode & 1u) p += snprintf(p, end - p, "%s", LV_SYMBOL_BLUETOOTH);
    if (mode & 2u) p += snprintf(p, end - p, "%s", LV_SYMBOL_WIFI);

    // Bell appears whenever there's at least one active alert. AlertsScreen
    // calls refreshStatusIcons() on alert raise/clear so this stays current
    // without DisplayService subscribing to alert events itself.
    if (g_alerts.count() > 0) snprintf(p, end - p, "%s", LV_SYMBOL_BELL);

    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_STATUS_ICONS, eez::StringValue(buf));
}

// ---------------------------------------------------------------------------
// Lifecycle — must be called AFTER g_ui.begin() so EEZ Flow assets are loaded.
// ---------------------------------------------------------------------------

void DisplayService::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(EV_BATTERY_UPDATED,  this);
    bus.subscribe(EV_TICK_1S,          this);
    bus.subscribe(EV_SETTINGS_CHANGED, this);
    bus.subscribe(CMD_SCAN_START,      this);
    bus.subscribe(CMD_SCAN_STOP,       this);

    refreshStatusIcons();
    _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);  // 0xFF pct = blank
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void DisplayService::onEvent(const Event& e) {
    switch (e.id) {
        case EV_BATTERY_UPDATED:
            _last_batt_mv  = e.data.battery.mv;
            _last_batt_pct = e.data.battery.pct;
            _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);
            break;

        case EV_TICK_1S:
            // USB/serial state can change any second; re-drive the prefix
            // without waiting up to 30s for the next BATTERY_UPDATED.
            _refreshBatteryStatus(_last_batt_mv, _last_batt_pct);
            break;

        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_SCAN) {
                refreshStatusIcons();
            }
            break;

        case CMD_SCAN_START:
            _scanning = true;
            refreshStatusIcons();
            break;

        case CMD_SCAN_STOP:
            _scanning = false;
            refreshStatusIcons();
            break;

        default:
            break;
    }
}
