#include "settings_screen.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "event_ids.h"
#include "event_payload.h"
#include "UI/screens.h"
#include <lvgl.h>
#include <Arduino.h>

SettingsScreen g_settings_screen;

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static EventBus* _bus     = nullptr;
static bool      _inhibit = false;  // blocks VALUE_CHANGED callbacks while populating

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void _postSetting(SettingsKey key, uint32_t value) {
    if (_inhibit || !_bus) return;
    EventPayload p{};
    p.settings_set.key   = key;
    p.settings_set.value = value;
    _bus->post(CMD_SETTINGS_SET, p);
    _bus->post(CMD_SETTINGS_SAVE);
}

static void _setSwitch(lv_obj_t* sw, bool on) {
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

// Perf mode ↔ scan_mode_dropdown index:
//   dropdown 0 = "Power Saver" = PERF_BATTERY_SAVER(2)
//   dropdown 1 = "Balanced"    = PERF_BALANCED(1)
//   dropdown 2 = "Performance" = PERF_PERFORMANCE(0)
static constexpr uint8_t kPerfToIdx[PERF_MODE_COUNT] = {2, 1, 0, 1};  // DYNAMIC→Balanced
static constexpr uint8_t kIdxToPerf[]                = {PERF_BATTERY_SAVER, PERF_BALANCED, PERF_PERFORMANCE};

// ---------------------------------------------------------------------------
// Populate all widgets from current settings (inhibits feedback callbacks)
// ---------------------------------------------------------------------------

static void _populate() {
    _inhibit = true;

    lv_dropdown_set_selected(objects.screen_brightness_dropdown,
                             g_settings.get(SKEY_SCREEN_BRIGHTNESS));
    lv_dropdown_set_selected(objects.led_brightness_dropdown,
                             g_settings.get(SKEY_LED_BRIGHTNESS));

    uint8_t perf = (uint8_t)g_settings.get(SKEY_PERF_MODE);
    lv_dropdown_set_selected(objects.scan_mode_dropdown,
                             kPerfToIdx[perf < PERF_MODE_COUNT ? perf : PERF_BALANCED]);

    lv_dropdown_set_selected(objects.debug_level_dropdown,
                             g_settings.get(SKEY_DEBUG_LEVEL));

    _setSwitch(objects.vibrate_on_alert_switch,     g_settings.getBool(SKEY_ALERT_VIBRATION));
    _setSwitch(objects.le_ds_on_alert_switch,        g_settings.getBool(SKEY_ALERT_LED));
    _setSwitch(objects.wake_screen_on_alert_switch,  g_settings.getBool(SKEY_ALERT_WAKE_SCREEN));

    uint32_t scan = g_settings.get(SKEY_SCAN_MODE);
    _setSwitch(objects.ble_scanning_switch,   scan & 1u);
    _setSwitch(objects.wi_fi_scanning_switch, scan & 2u);

    _setSwitch(objects.debug_over_serial_switch,  g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED));
    _setSwitch(objects.sleep_with_serial_switch,  g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL));
    _setSwitch(objects.sleep_with_usb_switch,     g_settings.getBool(SKEY_SLEEP_WHILE_USB));

    lv_label_set_text_static(objects.sleep_timer_label, "...");

    _inhibit = false;
}

// ---------------------------------------------------------------------------
// Widget VALUE_CHANGED callbacks
// ---------------------------------------------------------------------------

static void _on_screen_brightness(lv_event_t* /*e*/) {
    _postSetting(SKEY_SCREEN_BRIGHTNESS,
                 lv_dropdown_get_selected(objects.screen_brightness_dropdown));
}

static void _on_led_brightness(lv_event_t* /*e*/) {
    _postSetting(SKEY_LED_BRIGHTNESS,
                 lv_dropdown_get_selected(objects.led_brightness_dropdown));
}

static void _on_perf_mode(lv_event_t* /*e*/) {
    uint16_t idx = lv_dropdown_get_selected(objects.scan_mode_dropdown);
    _postSetting(SKEY_PERF_MODE, idx < 3 ? kIdxToPerf[idx] : PERF_BALANCED);
}

static void _on_debug_level(lv_event_t* /*e*/) {
    _postSetting(SKEY_DEBUG_LEVEL,
                 lv_dropdown_get_selected(objects.debug_level_dropdown));
}

static void _on_vibrate_on_alert(lv_event_t* /*e*/) {
    _postSetting(SKEY_ALERT_VIBRATION,
                 lv_obj_has_state(objects.vibrate_on_alert_switch, LV_STATE_CHECKED));
}

static void _on_leds_on_alert(lv_event_t* /*e*/) {
    _postSetting(SKEY_ALERT_LED,
                 lv_obj_has_state(objects.le_ds_on_alert_switch, LV_STATE_CHECKED));
}

static void _on_wake_screen_on_alert(lv_event_t* /*e*/) {
    _postSetting(SKEY_ALERT_WAKE_SCREEN,
                 lv_obj_has_state(objects.wake_screen_on_alert_switch, LV_STATE_CHECKED));
}

static void _on_scan_switches(lv_event_t* /*e*/) {
    bool ble  = lv_obj_has_state(objects.ble_scanning_switch,   LV_STATE_CHECKED);
    bool wifi = lv_obj_has_state(objects.wi_fi_scanning_switch, LV_STATE_CHECKED);
    _postSetting(SKEY_SCAN_MODE, (wifi ? 2u : 0u) | (ble ? 1u : 0u));
}

static void _on_debug_over_serial(lv_event_t* /*e*/) {
    _postSetting(SKEY_DEBUG_SERIAL_ENABLED,
                 lv_obj_has_state(objects.debug_over_serial_switch, LV_STATE_CHECKED));
}

static void _on_sleep_with_serial(lv_event_t* /*e*/) {
    _postSetting(SKEY_DEBUG_SLEEP_WITH_SERIAL,
                 lv_obj_has_state(objects.sleep_with_serial_switch, LV_STATE_CHECKED));
}

static void _on_sleep_with_usb(lv_event_t* /*e*/) {
    _postSetting(SKEY_SLEEP_WHILE_USB,
                 lv_obj_has_state(objects.sleep_with_usb_switch, LV_STATE_CHECKED));
}

// ---------------------------------------------------------------------------
// Button CLICKED callbacks
// ---------------------------------------------------------------------------

static void _on_sleep_button(lv_event_t* /*e*/) {
    if (_bus) _bus->post(CMD_POWER_SLEEP);
}

static void _on_reboot_button(lv_event_t* /*e*/) {
    ESP.restart();
}

static void _on_shipping_mode_button(lv_event_t* /*e*/) {
    if (_bus) _bus->post(CMD_POWER_SHIPPING_SLEEP);
}

// ---------------------------------------------------------------------------
// Screen load — fires after EEZ's handler (registered after ui_init)
// ---------------------------------------------------------------------------

static void _on_settings_load(lv_event_t* /*e*/) {
    _populate();
}

// ---------------------------------------------------------------------------
// Lifecycle — must be called after g_ui.begin() so objects.* are valid
// ---------------------------------------------------------------------------

void SettingsScreen::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(EV_SETTINGS_CHANGED,        this);
    bus.subscribe(EV_SLEEP_COUNTDOWN_UPDATED, this);

    lv_obj_add_event_cb(objects.settings, _on_settings_load, LV_EVENT_SCREEN_LOAD_START, nullptr);

    lv_obj_add_event_cb(objects.screen_brightness_dropdown,  _on_screen_brightness,   LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.led_brightness_dropdown,     _on_led_brightness,      LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.scan_mode_dropdown,          _on_perf_mode,           LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.debug_level_dropdown,        _on_debug_level,         LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_add_event_cb(objects.vibrate_on_alert_switch,     _on_vibrate_on_alert,    LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.le_ds_on_alert_switch,       _on_leds_on_alert,       LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.wake_screen_on_alert_switch, _on_wake_screen_on_alert,LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.ble_scanning_switch,         _on_scan_switches,       LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.wi_fi_scanning_switch,       _on_scan_switches,       LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.debug_over_serial_switch,    _on_debug_over_serial,   LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.sleep_with_serial_switch,    _on_sleep_with_serial,   LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.sleep_with_usb_switch,       _on_sleep_with_usb,      LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_add_event_cb(objects.sleep_button,         _on_sleep_button,         LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.reboot_button,        _on_reboot_button,        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.shipping_mode_button, _on_shipping_mode_button, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void SettingsScreen::onEvent(const Event& e) {
    if (e.id == EV_SETTINGS_CHANGED && lv_scr_act() == objects.settings) {
        _populate();
        return;
    }
    if (e.id == EV_SLEEP_COUNTDOWN_UPDATED && lv_scr_act() == objects.settings) {
        uint16_t s = e.data.sleep_count.seconds;
        if (s == 0xFFFFu) {
            lv_label_set_text_static(objects.sleep_timer_label, "Will not sleep");
        } else {
            char buf[24];
            if (s >= 60)
                snprintf(buf, sizeof(buf), "Sleeping in %um %us", s / 60, s % 60);
            else
                snprintf(buf, sizeof(buf), "Sleeping in %us", s);
            lv_label_set_text(objects.sleep_timer_label, buf);
        }
    }
}
