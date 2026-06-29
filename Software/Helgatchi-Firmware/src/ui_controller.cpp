#include "ui_controller.h"
#include "hal.h"
#include "settings_service.h"
#include "power_manager.h"
#include "fa_icons.h"
#include "UI/ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

UIController g_ui;

// ---------------------------------------------------------------------------
// LVGL display driver — LVGL 9.x API, flushed via LovyanGFX
// ---------------------------------------------------------------------------

static lv_color_t _disp_buf[280 * 40];

// LVGL 9.x dropped LV_TICK_CUSTOM in favour of a runtime tick callback. Without
// this, no time passes from LVGL's point of view — animations never advance and
// delayed screen transitions (e.g. SquareLine's splash → status fade) never fire.
static uint32_t _tick_cb() {
    return millis();
}

static void _flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    g_hal.tft().startWrite();
    g_hal.tft().setAddrWindow(area->x1, area->y1, w, h);
    g_hal.tft().writePixels((lgfx::rgb565_t*)px_map, w * h);
    g_hal.tft().endWrite();
    lv_display_flush_ready(disp);
}

// ---------------------------------------------------------------------------
// Keypad input device
//
// Physical buttons fire EV_BTN_* once per press. LVGL's keypad indev expects
// PRESSED then RELEASED to register a key tap, so we queue keys and emit a
// press/release pair across two indev reads.
// ---------------------------------------------------------------------------

static constexpr uint8_t  KEY_QUEUE_SIZE = 8;
static uint32_t           _key_queue[KEY_QUEUE_SIZE];
static uint8_t            _key_head     = 0;
static uint8_t            _key_tail     = 0;
static bool               _key_pressed  = false;
static uint32_t           _current_key  = 0;
static lv_indev_t*        _indev_kbd    = nullptr;

static void _enqueueKey(uint32_t key) {
    uint8_t next = (_key_tail + 1) % KEY_QUEUE_SIZE;
    if (next == _key_head) return;          // queue full — drop
    _key_queue[_key_tail] = key;
    _key_tail = next;
}

static void _kbd_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    if (_key_pressed) {
        _key_pressed = false;
        data->key   = _current_key;
        data->state = LV_INDEV_STATE_RELEASED;
    } else if (_key_head != _key_tail) {
        _current_key = _key_queue[_key_head];
        _key_head    = (_key_head + 1) % KEY_QUEUE_SIZE;
        _key_pressed = true;
        data->key   = _current_key;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->key   = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---------------------------------------------------------------------------
// Per-screen focus groups
// ---------------------------------------------------------------------------

static lv_group_t* _grp_menu     = nullptr;
static lv_group_t* _grp_settings = nullptr;

static void _useGroup(lv_group_t* g) {
    if (_indev_kbd) lv_indev_set_group(_indev_kbd, g);
}

// Recursively walk a subtree and add every interactive widget to a group, in
// DOM order. Keypad navigation then matches the visual layout — the
// alternative (manual lv_group_add_obj per widget) silently couples nav order
// to the order things appear in code.
static void _populateGroupFromSubtree(lv_group_t* g, lv_obj_t* root) {
    if (!g || !root) return;
    if (lv_obj_check_type(root, &lv_switch_class)   ||
        lv_obj_check_type(root, &lv_checkbox_class) ||
        lv_obj_check_type(root, &lv_dropdown_class) ||
        lv_obj_check_type(root, &lv_roller_class)   ||
        lv_obj_check_type(root, &lv_slider_class)   ||
        lv_obj_check_type(root, &lv_arc_class)      ||
        lv_obj_check_type(root, &lv_button_class)) {
        lv_group_add_obj(g, root);
    }
    uint32_t cnt = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < cnt; i++) {
        _populateGroupFromSubtree(g, lv_obj_get_child(root, i));
    }
}

// ---------------------------------------------------------------------------
// Settings ⇄ widget binding (table-driven)
//
// Adding a 1:1 mappable setting widget = one line in _bindings[]. Type
// detection happens at runtime via lv_obj_check_type, so switch / checkbox /
// dropdown / roller / slider / arc all share one handler/refresh path.
//
// Adding a one-shot action button = one line in _actions[].
//
// Mappings that aren't 1:1 (e.g. SCAN_MODE built from two switches) live in
// their own _onXxxChanged handler and _refreshXxx() function below.
// ---------------------------------------------------------------------------

struct SettingsBinding {
    lv_obj_t**  widget;
    SettingsKey key;
};

struct ActionBinding {
    lv_obj_t** widget;
    EventId    cmd;
};

static const SettingsBinding _bindings[] = {
    { &ui_settings_screen_dropdown_brightness,                SKEY_SCREEN_BRIGHTNESS       },
    { &ui_settings_screen_dropdown_performance_mode,          SKEY_PERF_MODE               },
    { &ui_settings_screen_switch_debug_over_serial,           SKEY_DEBUG_SERIAL_ENABLED    },
    { &ui_settings_screen_dropdown_debug_level,               SKEY_DEBUG_LEVEL             },
    { &ui_settings_screen_switch_sleep_while_serial,          SKEY_DEBUG_SLEEP_WITH_SERIAL },
    { &ui_settings_screen_switch_sleep_while_usb,             SKEY_SLEEP_WHILE_USB         },
};

static const ActionBinding _actions[] = {
    { &ui_settings_screen_button_shipping_mode, CMD_POWER_SHIPPING_SLEEP },
    // CMD_POWER_SLEEP bypasses the inhibit check in PowerManager — pressing
    // "Sleep Now" works even with serial open / USB attached.
    { &ui_settings_screen_button_sleep,         CMD_POWER_SLEEP          },
    // button_reboot needs ESP.restart() — wired via _onRebootClicked below
    // since no CMD_POWER_REBOOT exists in the event bus.
};

static uint32_t _readWidget(lv_obj_t* w) {
    if (!w) return 0;
    if (lv_obj_check_type(w, &lv_switch_class) || lv_obj_check_type(w, &lv_checkbox_class))
        return lv_obj_has_state(w, LV_STATE_CHECKED) ? 1u : 0u;
    if (lv_obj_check_type(w, &lv_dropdown_class)) return lv_dropdown_get_selected(w);
    if (lv_obj_check_type(w, &lv_roller_class))   return lv_roller_get_selected(w);
    if (lv_obj_check_type(w, &lv_slider_class))   return (uint32_t)lv_slider_get_value(w);
    if (lv_obj_check_type(w, &lv_arc_class))      return (uint32_t)lv_arc_get_value(w);
    return 0;
}

static void _writeWidget(lv_obj_t* w, uint32_t v) {
    if (!w) return;
    if (lv_obj_check_type(w, &lv_switch_class) || lv_obj_check_type(w, &lv_checkbox_class)) {
        if (v) lv_obj_add_state(w, LV_STATE_CHECKED);
        else   lv_obj_remove_state(w, LV_STATE_CHECKED);
    } else if (lv_obj_check_type(w, &lv_dropdown_class)) {
        lv_dropdown_set_selected(w, (uint16_t)v);
    } else if (lv_obj_check_type(w, &lv_roller_class)) {
        lv_roller_set_selected(w, (uint16_t)v, LV_ANIM_OFF);
    } else if (lv_obj_check_type(w, &lv_slider_class)) {
        lv_slider_set_value(w, (int32_t)v, LV_ANIM_OFF);
    } else if (lv_obj_check_type(w, &lv_arc_class)) {
        lv_arc_set_value(w, (int32_t)v);
    }
}

static void _onWidgetChanged(lv_event_t* e) {
    const SettingsBinding* b = (const SettingsBinding*)lv_event_get_user_data(e);
    if (!b || !b->widget || !*b->widget) return;
    EventPayload p{};
    p.settings_set.key   = b->key;
    p.settings_set.value = _readWidget(*b->widget);
    g_bus.post(CMD_SETTINGS_SET, p);
}

static void _onActionClicked(lv_event_t* e) {
    const ActionBinding* a = (const ActionBinding*)lv_event_get_user_data(e);
    if (a) g_bus.post(a->cmd);
}

// ---------------------------------------------------------------------------
// Custom (non-1:1) bindings
// ---------------------------------------------------------------------------

// SKEY_SCAN_MODE encodes a 2-bit field (bit 0 = BLE, bit 1 = WiFi). Two
// independent switches map to it: each toggle rebuilds the combined value.
static void _onScanBitsChanged(lv_event_t* /*e*/) {
    uint32_t mode = 0;
    if (ui_settings_screen_switch_ble_scanning &&
        lv_obj_has_state(ui_settings_screen_switch_ble_scanning, LV_STATE_CHECKED))
        mode |= 1u;  // SCAN_BLE_ONLY bit
    if (ui_settings_screen_switch_wifi_scanning &&
        lv_obj_has_state(ui_settings_screen_switch_wifi_scanning, LV_STATE_CHECKED))
        mode |= 2u;  // SCAN_WIFI_ONLY bit
    EventPayload p{};
    p.settings_set.key   = SKEY_SCAN_MODE;
    p.settings_set.value = mode;
    g_bus.post(CMD_SETTINGS_SET, p);
}

static void _refreshScanBits() {
    uint32_t mode = g_settings.get(SKEY_SCAN_MODE);
    if (ui_settings_screen_switch_ble_scanning) {
        if (mode & 1u) lv_obj_add_state(ui_settings_screen_switch_ble_scanning, LV_STATE_CHECKED);
        else           lv_obj_remove_state(ui_settings_screen_switch_ble_scanning, LV_STATE_CHECKED);
    }
    if (ui_settings_screen_switch_wifi_scanning) {
        if (mode & 2u) lv_obj_add_state(ui_settings_screen_switch_wifi_scanning, LV_STATE_CHECKED);
        else           lv_obj_remove_state(ui_settings_screen_switch_wifi_scanning, LV_STATE_CHECKED);
    }
}

// Reboot button — no CMD_* exists for this, so call ESP.restart() directly.
// Tiny delay first so the serial line gets a final flush, plus we have to
// wind down the motor before resetting: the click haptic just fired ~5 ms
// ago, and ESP.restart() leaves LEDC frozen at the current duty across the
// reset. Without an explicit stop, the motor keeps driving from the click
// haptic *plus* the boot haptic on the other side of the reset = one long
// jarring buzz instead of a click.
static void _onRebootClicked(lv_event_t*) {
    Serial.println("[ui] reboot button pressed");
    Serial.flush();
    g_hal.stopVibrate();
    delay(150);   // motor spin-down + final serial flush window
    ESP.restart();
}

static void _refreshSettingsWidgets() {
    for (const auto& b : _bindings) {
        if (b.widget && *b.widget) _writeWidget(*b.widget, g_settings.get(b.key));
    }
    _refreshScanBits();
}

// ---------------------------------------------------------------------------
// Top-bar battery % — written to every screen's right-text label on
// EV_BATTERY_UPDATED. Add a screen here when its top bar exposes a
// battery slot.
// ---------------------------------------------------------------------------

static lv_obj_t* const* _batteryLabels[] = {
    &ui_status_screen_top_bar_right_text_righttext,
    &ui_menu_screen_top_bar_right_text_righttext1,
    &ui_settings_screen_top_bar_right_text_righttext2,
};

// Top-bar left-text slot — shows wifi / bluetooth icons based on the active
// scan mode (SKEY_SCAN_MODE bit 0 = BLE, bit 1 = WiFi). Add a screen here
// when its top bar exposes a left-text label.
static lv_obj_t* const* _radioIconLabels[] = {
    &ui_status_screen_top_bar_left_text_top_bar_left_text1,
    &ui_menu_screen_top_bar_left_text_top_bar_left_text2,
    &ui_settings_screen_top_bar_left_text_top_bar_left_text3,
};

// Cached so a newly-loaded screen can paint immediately instead of waiting
// up to 30s for the next BATTERY_UPDATED. 0xFF pct = "no reading yet".
static uint16_t _last_batt_mv  = 0;
static uint8_t  _last_batt_pct = 0xFF;

static void _writeBatteryLabel(lv_obj_t* label, uint16_t mv, uint8_t pct) {
    if (!label) return;

    // 0xFF sentinel = no reading yet (cold-boot first few seconds). Keep the
    // slot empty so we don't flash a warning before the first battery sample.
    if (pct == 0xFF) {
        lv_label_set_text(label, "");
        return;
    }

    // Bucket hysteresis state — shared across all label slots (every slot
    // shows the same battery, so they should all transition together).
    // Buckets: 0=EMPTY, 1=_1, 2=_2, 3=_3, 4=FULL. Lower edges at 0/20/40/60/80.
    // Climbing a bucket is immediate; dropping requires HYST_PP below the
    // current bucket's lower edge to avoid flicker at boundaries.
    static uint8_t _last_bucket = 0xFF;
    constexpr uint8_t HYST_PP = 5;

    // Battery missing / 0 mV / floating ADC: warning glyph + empty battery,
    // no connection prefix (a missing battery overrides everything). Reset
    // hysteresis so a recovered reading paints a fresh bucket from scratch.
    if (pct == BATT_PCT_MISSING) {
        _last_bucket = 0xFF;
        lv_label_set_text(label, LV_SYMBOL_WARNING " " LV_SYMBOL_BATTERY_EMPTY);
        return;
    }

    // The CHARGING / CHARGED sentinels don't carry the actual percentage, so
    // derive a level from mv via the same curve PowerManager uses — the bar
    // then reflects how full the battery actually is while charging.
    uint8_t level = (pct > 100) ? pmBattPctFromVsenseMv((uint16_t)(mv / 2)) : pct;

    // Map to bucket with hysteresis on step-down only.
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

    // Connection prefix — most specific wins:
    //   serial open                            → keyboard
    //   USB cable detected (host enumerated)   → usb
    //   charging sentinel without host         → bolt (rare on this hardware,
    //                                            ~only if SOF detection misses
    //                                            during enumeration / dumb charger)
    //   none of the above                      → no prefix (battery-only)
    const char* prefix = "";
    if      ((bool)Serial)                                       prefix = LV_SYMBOL_KEYBOARD " ";
    else if (g_hal.usbAttached())                                prefix = LV_SYMBOL_USB " ";
    else if (pct == BATT_PCT_CHARGING || pct == BATT_PCT_CHARGED) prefix = LV_SYMBOL_CHARGE " ";

    char buf[16];
    snprintf(buf, sizeof(buf), "%s%s", prefix, batt_sym);
    lv_label_set_text(label, buf);
}

static void _refreshBatteryLabels(uint16_t mv, uint8_t pct) {
    for (auto* slot : _batteryLabels) {
        if (slot && *slot) _writeBatteryLabel(*slot, mv, pct);
    }
}

static void _refreshRadioIcons() {
    // SCAN_MODE bits: 0=BLE, 1=WiFi. Both = both icons (wifi-then-BT).
    uint32_t mode = g_settings.get(SKEY_SCAN_MODE);
    const char* text;
    switch (mode & 3u) {
        case 1u: text = LV_SYMBOL_BLUETOOTH;                       break;  // BLE only
        case 2u: text = LV_SYMBOL_WIFI;                            break;  // WiFi only
        case 3u: text = LV_SYMBOL_WIFI " " LV_SYMBOL_BLUETOOTH;    break;  // both
        default: text = "";                                        break;  // disabled
    }
    for (auto* slot : _radioIconLabels) {
        if (slot && *slot) lv_label_set_text(*slot, text);
    }
}

// ---------------------------------------------------------------------------
// Screen lifecycle: logging + group switching + data refresh
// ---------------------------------------------------------------------------

static void _onScreenLoadStart(lv_obj_t* scr) {
    if      (scr == ui_screen_menu_screen)     _useGroup(_grp_menu);
    else if (scr == ui_screen_settings_screen) { _useGroup(_grp_settings); _refreshSettingsWidgets(); }
    else                                        _useGroup(nullptr);

    // Re-paint the battery slot on this screen with the most recent reading
    // (its label was just (re)created by SLS's screen_init and is empty/default).
    if (_last_batt_pct != 0xFF) {
        _refreshBatteryLabels(_last_batt_mv, _last_batt_pct);
    }
    // Same for the radio-status icons.
    _refreshRadioIcons();
}

static void _scr_evt_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const char* name = (const char*)lv_event_get_user_data(e);

    if (code == LV_EVENT_SCREEN_LOAD_START) {
        _onScreenLoadStart((lv_obj_t*)lv_event_get_target(e));
    }

    if (!g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED)) return;

    const char* tag = nullptr;
    switch (code) {
        case LV_EVENT_SCREEN_LOAD_START:    tag = "SCREEN_LOAD_START";    break;
        case LV_EVENT_SCREEN_LOADED:        tag = "SCREEN_LOADED";        break;
        case LV_EVENT_SCREEN_UNLOAD_START:  tag = "SCREEN_UNLOAD_START";  break;
        case LV_EVENT_SCREEN_UNLOADED:      tag = "SCREEN_UNLOADED";      break;
        default: return;
    }
    Serial.printf("[%8lu] %-19s %s\n", millis(), tag, name);
}

static void _registerScreenLogging(lv_obj_t* scr, const char* name) {
    lv_obj_add_event_cb(scr, _scr_evt_cb, LV_EVENT_SCREEN_LOAD_START,   (void*)name);
    lv_obj_add_event_cb(scr, _scr_evt_cb, LV_EVENT_SCREEN_LOADED,       (void*)name);
    lv_obj_add_event_cb(scr, _scr_evt_cb, LV_EVENT_SCREEN_UNLOAD_START, (void*)name);
    lv_obj_add_event_cb(scr, _scr_evt_cb, LV_EVENT_SCREEN_UNLOADED,     (void*)name);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void UIController::begin(EventBus& bus) {
    _bus = &bus;

    lv_init();
    lv_tick_set_cb(_tick_cb);

    lv_display_t* disp = lv_display_create(280, 240);
    lv_display_set_flush_cb(disp, _flush_cb);
    lv_display_set_buffers(disp, _disp_buf, nullptr,
                           sizeof(_disp_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    ui_init();  // SquareLine Studio — inits theme and loads splash

    // --- Keypad indev (no group attached yet — _onScreenLoadStart sets it) ---
    _indev_kbd = lv_indev_create();
    lv_indev_set_type(_indev_kbd, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(_indev_kbd, _kbd_read_cb);

    // --- Menu screen group: iterate the carousel children in DOM order ---
    _grp_menu = lv_group_create();
    lv_group_set_wrap(_grp_menu, true);
    if (ui_menu_screen_container_container5) {
        uint32_t cnt = lv_obj_get_child_count(ui_menu_screen_container_container5);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_group_add_obj(_grp_menu, lv_obj_get_child(ui_menu_screen_container_container5, i));
        }
    }

    // --- Settings screen group: auto-populate from DOM in visual order ---
    _grp_settings = lv_group_create();
    lv_group_set_wrap(_grp_settings, true);
    _populateGroupFromSubtree(_grp_settings, ui_screen_settings_screen);

    // --- Bindings: register handlers (group membership done above) ---
    for (const auto& b : _bindings) {
        if (b.widget && *b.widget) {
            lv_obj_add_event_cb(*b.widget, _onWidgetChanged,
                                LV_EVENT_VALUE_CHANGED, (void*)&b);
        }
    }
    for (const auto& a : _actions) {
        if (a.widget && *a.widget) {
            lv_obj_add_event_cb(*a.widget, _onActionClicked,
                                LV_EVENT_CLICKED, (void*)&a);
        }
    }

    // --- Custom bindings ---
    if (ui_settings_screen_switch_ble_scanning) {
        lv_obj_add_event_cb(ui_settings_screen_switch_ble_scanning,
                            _onScanBitsChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    if (ui_settings_screen_switch_wifi_scanning) {
        lv_obj_add_event_cb(ui_settings_screen_switch_wifi_scanning,
                            _onScanBitsChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    if (ui_settings_screen_button_reboot) {
        lv_obj_add_event_cb(ui_settings_screen_button_reboot,
                            _onRebootClicked, LV_EVENT_CLICKED, nullptr);
    }

    // --- Screen lifecycle hooks (logging + group switching + data refresh) ---
    _registerScreenLogging(ui_screen_splash_screen,   "Splash");
    _registerScreenLogging(ui_screen_status_screen,   "Status");
    _registerScreenLogging(ui_screen_menu_screen,     "Menu");
    _registerScreenLogging(ui_screen_settings_screen, "Settings");

    // --- Static icon overrides ---------------------------------------------
    // SLS exports the menu settings icon as the literal 8-character string
    // "&#xf085;" (it doesn't UTF-8-encode HTML entities on export). Re-write
    // the label here so it actually renders a glyph. We don't edit the SLS
    // .c file because re-export overwrites it.
    //
    // Stock lv_font_montserrat_36 doesn't contain U+F085 (fa-cogs), so for
    // now we use FA_COG (U+F013, single gear, in stock Montserrat). To use
    // FA_GEARS instead, regenerate Montserrat with the FA range included
    // (see include/fa_icons.h for the lv_font_conv command) and swap the
    // line below to FA_GEARS.
    if (ui_menu_screen_label_settings_icon) {
        lv_label_set_text(ui_menu_screen_label_settings_icon, FA_COG);
        // After regenerating the font with U+F085:
        //   lv_label_set_text(ui_menu_screen_label_settings_icon, FA_GEARS);
    }
    if (g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED)) {
        Serial.printf("[%8lu] %-19s %s\n", millis(), "SCREEN_INITIAL", "Splash");
    }

    bus.subscribe(EV_BTN_LEFT,                this);
    bus.subscribe(EV_BTN_RIGHT,               this);
    bus.subscribe(EV_BTN_CENTER_SHORT,        this);
    bus.subscribe(EV_BTN_CENTER_LONG,         this);
    bus.subscribe(EV_SETTINGS_CHANGED,        this);
    bus.subscribe(EV_BATTERY_UPDATED,         this);
    bus.subscribe(EV_SLEEP_COUNTDOWN_UPDATED, this);
    bus.subscribe(EV_TICK_1S,                 this);
}

void UIController::tick() {
    // Skip LVGL rendering when the display is off. The screen is unlit, so
    // any frame produced is invisible — and lv_timer_handler is the heaviest
    // thing in the main loop (~70 % CPU on this hardware). PowerManager
    // toggles _render_enabled via setRenderEnabled() in _setDisplay.
    if (!_render_enabled) return;
    lv_timer_handler();
}

// ---------------------------------------------------------------------------
// IEventHandler — button routing
// ---------------------------------------------------------------------------

void UIController::onEvent(const Event& e) {
    lv_group_t* g = _indev_kbd ? lv_indev_get_group(_indev_kbd) : nullptr;

    switch (e.id) {

        case EV_BTN_LEFT:
        case EV_BTN_RIGHT: {
            const bool is_left = (e.id == EV_BTN_LEFT);
            lv_obj_t* focused = g ? lv_group_get_focused(g) : nullptr;

            // A dropdown's open-list state is a property of the widget, not the
            // group — pressing ENTER on a dropdown opens the list but DOESN'T
            // flip lv_group_get_editing(). Check the dropdown directly.
            const bool dropdown_open = focused
                && lv_obj_check_type(focused, &lv_dropdown_class)
                && lv_dropdown_is_open(focused);

            if (dropdown_open) {
                _enqueueKey(is_left ? LV_KEY_UP : LV_KEY_DOWN);
            } else if (g && lv_group_get_editing(g)) {
                _enqueueKey(is_left ? LV_KEY_LEFT : LV_KEY_RIGHT);
            } else {
                _enqueueKey(is_left ? LV_KEY_PREV : LV_KEY_NEXT);
            }
            break;
        }

        case EV_BTN_CENTER_SHORT: {
            lv_obj_t* active = lv_screen_active();
            if (active == ui_screen_status_screen) {
                _ui_screen_change(&ui_screen_menu_screen, LV_SCR_LOAD_ANIM_NONE,
                                  0, 0, &ui_screen_menu_screen_screen_init);
            } else {
                _enqueueKey(LV_KEY_ENTER);
            }
            break;
        }

        case EV_BTN_CENTER_LONG:
            if (lv_screen_active() != ui_screen_status_screen) {
                _ui_screen_change(&ui_screen_status_screen, LV_SCR_LOAD_ANIM_NONE,
                                  0, 0, &ui_screen_status_screen_screen_init);
            }
            break;

        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_SCAN) {
                // BLE/WiFi switch toggled — refresh the radio icons on every
                // screen's top bar regardless of which one is currently active,
                // so navigating to another screen shows fresh state.
                _refreshRadioIcons();
            }
            if (lv_screen_active() == ui_screen_settings_screen) {
                _refreshSettingsWidgets();
            }
            break;

        case EV_BATTERY_UPDATED:
            _last_batt_mv  = e.data.battery.mv;
            _last_batt_pct = e.data.battery.pct;
            _refreshBatteryLabels(_last_batt_mv, _last_batt_pct);
            break;

        case EV_TICK_1S:
            // Re-read live USB/serial state into the right-text label every
            // second. Battery sample interval is 30s but cable + terminal
            // state can change on any second — driving the icon refresh from
            // the 1Hz heartbeat keeps the indicator responsive.
            _refreshBatteryLabels(_last_batt_mv, _last_batt_pct);
            break;

        case EV_SLEEP_COUNTDOWN_UPDATED:
            // PowerManager sends 0xFFFF when sleep is inhibited (USB/serial),
            // otherwise the seconds remaining until interactive-timeout sleep.
            if (ui_settings_screen_label_sleep_text) {
                if (e.data.sleep_count.seconds == 0xFFFF) {
                    lv_label_set_text(ui_settings_screen_label_sleep_text, "Will not sleep");
                } else {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "Sleep in %us", e.data.sleep_count.seconds);
                    lv_label_set_text(ui_settings_screen_label_sleep_text, buf);
                }
            }
            break;

        default:
            break;
    }
}
