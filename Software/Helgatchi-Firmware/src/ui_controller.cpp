#include "ui_controller.h"
#include "hal.h"
#include "settings_service.h"
#include "UI/ui.h"
#include "UI/screens.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

UIController g_ui;

static EventBus* _ui_bus = nullptr;

// ---------------------------------------------------------------------------
// LVGL display driver — LVGL 9.x API, flushed via LovyanGFX
// ---------------------------------------------------------------------------

// Two equal-sized buffers allow LVGL to track dirty regions more efficiently.
// 60 rows × 280 px = 4 strips per 240-row screen (vs 6 with 40-row buffers).
static lv_color_t _disp_buf1[280 * 60];
static lv_color_t _disp_buf2[280 * 60];

static uint32_t _tick_cb() {
    return millis();
}

static void _flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    g_hal.tft().startWrite();
    g_hal.tft().setAddrWindow(area->x1, area->y1, w, h);
    g_hal.tft().writePixels((lgfx::rgb565_t*)px_map, (int32_t)(w * h));
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
// Per-screen focus save/restore
//
// EEZ's SCREEN_LOAD_START handler calls lv_group_remove_all_objs then
// re-adds widgets, which resets focus to the first widget and scrolls to it.
// Our handlers are registered after ui_init() so they fire after EEZ's in
// LVGL's dispatch order — we save focus on unload and restore it on load-start
// before the first frame renders.
// ---------------------------------------------------------------------------

static lv_obj_t* _saved_main_menu_focus = nullptr;

static void _on_screen_unload(lv_event_t* /*e*/) {
    _saved_main_menu_focus = lv_group_get_focused(groups.UINavigation);
}

static void _on_screen_load_start(lv_event_t* /*e*/) {
    if (_saved_main_menu_focus) lv_group_focus_obj(_saved_main_menu_focus);
}

static void _on_tutorial_splash_load(lv_event_t* /*e*/) {
    if (!_ui_bus) return;
    EventPayload p{};
    p.settings_set.key   = SKEY_TUTORIAL_SHOWN;
    p.settings_set.value = 1;
    _ui_bus->post(CMD_SETTINGS_SET, p);
}



void UIController::begin(EventBus& bus) {
    _bus    = &bus;
    _ui_bus = &bus;

    lv_init();
    lv_tick_set_cb(_tick_cb);

    lv_display_t* disp = lv_display_create(280, 240);
    lv_display_set_flush_cb(disp, _flush_cb);
    lv_display_set_buffers(disp, _disp_buf1, _disp_buf2,
                           sizeof(_disp_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // EEZ: ui_init() runs eez_flow_init which calls create_screens() and then
    // replacePageHook(1,...) — page 1 is the first entry in screen_names[],
    // which is Main Menu. So Main Menu is the default screen after ui_init().
    ui_init();

    // Register splash-load handler before we (maybe) load the splash, so the
    // initial load also marks SKEY_TUTORIAL_SHOWN.
    lv_obj_add_event_cb(objects.tutorial_splash_screen, _on_tutorial_splash_load,
                        LV_EVENT_SCREEN_LOAD_START, nullptr);

    // Show the tutorial on first flash or after shipping-mode exit.
    // PowerManager resets SKEY_TUTORIAL_SHOWN to 0 before shipping sleep;
    // blank/old NVS also defaults to 0.
    if (!g_settings.getBool(SKEY_TUTORIAL_SHOWN)) {
        lv_scr_load(objects.tutorial_splash_screen);
    }

    lv_obj_add_event_cb(objects.main_menu, _on_screen_unload,     LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_obj_add_event_cb(objects.main_menu, _on_screen_load_start, LV_EVENT_SCREEN_LOAD_START,   nullptr);

    // Keypad indev wired to EEZ's single nav group. Each screen's
    // SCREEN_LOAD_START handler in screens.c repopulates groups.UINavigation
    // with that screen's focusable widgets, so the indev follows screen
    // changes without any work on our side.
    _indev_kbd = lv_indev_create();
    lv_indev_set_type(_indev_kbd, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(_indev_kbd, _kbd_read_cb);
    lv_indev_set_group(_indev_kbd, groups.UINavigation);

    bus.subscribe(EV_BTN_LEFT,          this);
    bus.subscribe(EV_BTN_RIGHT,         this);
    bus.subscribe(EV_BTN_CENTER_SHORT,  this);
    bus.subscribe(EV_BTN_CENTER_LONG,   this);
}

void UIController::tick() {
    if (!_render_enabled) return;
    ui_tick();          // EEZ Flow runtime + per-screen tick handlers
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

        case EV_BTN_CENTER_SHORT:
            _enqueueKey(LV_KEY_ENTER);
            break;

        case EV_BTN_CENTER_LONG:
            eez_flow_pop_screen(LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
            break;

        default:
            break;
    }
}
