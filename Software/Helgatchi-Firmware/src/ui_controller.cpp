#include "ui_controller.h"
#include "hal.h"
#include "settings_service.h"
#include "power_manager.h"
#include "vibe_service.h"
#include "version.h"
#include "UI/ui.h"
#include "UI/screens.h"
#include "UI/eez-flow.h"          // eez_flow_pop_screen
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <esp32s3/rom/cache.h>   // Cache_WriteBack_Addr — IDF 4.4 ESP32-S3 ROM cache API

UIController g_ui;

static EventBus* _ui_bus = nullptr;

// ---------------------------------------------------------------------------
// LVGL display driver — LVGL 9.x API, flushed via LovyanGFX
// ---------------------------------------------------------------------------

// Two equal-sized partial-render buffers let LVGL pipeline render-vs-flush
// across strips. 120 rows × 280 px = 2 strips per 240-row screen. At
// lv_color_t = 2 bytes (RGB565) each buffer is 67,200 B — together ~134 KB.
// Allocated in PSRAM at begin() time so they don't eat internal DRAM.
// Larger strips → fewer flushes per frame → fewer tear seams.
//
// Flush strategy: writePixelsDMA + deferred endWrite + PSRAM cache writeback.
// Each strip's DMA runs in the background while the CPU renders the next
// strip into the OTHER buffer. The bus stays "held" with a pending DMA
// between strips and between frames; endWrite is called at the START of the
// next flush to drain the previous transfer before reusing the bus.
//
// PSRAM cache caveat: the CPU writes pixels through its data cache, but GDMA
// reads from physical PSRAM. Dirty cache lines must be written back before
// each DMA — otherwise DMA pulls stale bytes (the green-glitch symptom).
// Cache_WriteBack_Addr handles this; the address/size are rounded out to the
// 32-byte cache line.
//
// FULL mode was tried (one flush per frame, atomic full-screen write); it
// reduced strip seams from 2 → 1 but the remaining seam was still visible
// during scrolls, while costing a brief FPS dip on small isolated updates.
// Not worth the trade — reverted.
static constexpr size_t DISP_BUF_PX    = 280 * 120;
static constexpr size_t DISP_BUF_BYTES = DISP_BUF_PX * sizeof(lv_color_t);
static lv_color_t* _disp_buf1 = nullptr;
static lv_color_t* _disp_buf2 = nullptr;
static bool        _dma_pending = false;   // an endWrite is owed at next flush

// Ground-truth flush counter. Incremented once per flush_cb call.
// Counts strips, not frames — in PARTIAL mode that's 1-2 calls/frame.
static uint32_t    _flush_count          = 0;
static uint32_t    _flush_last_sample_ms = 0;

static uint32_t _tick_cb() {
    return millis();
}

static void _flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto& tft = g_hal.tft();
    const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    const size_t bytes = (size_t)w * h * sizeof(lv_color_t);

    // Drain the previous DMA (and release its transaction) before grabbing
    // the bus for this strip. No-op on the very first flush.
    if (_dma_pending) {
        tft.endWrite();
        _dma_pending = false;
    }

    // Write back any dirty PSRAM cache lines covering px_map so GDMA reads
    // the pixels the CPU just rendered. 32-byte cache line on ESP32-S3 — we
    // align the address down and the size up so partial-line writes flush.
    const uintptr_t aligned_addr = (uintptr_t)px_map & ~31U;
    const size_t    aligned_size = (((uintptr_t)px_map + bytes + 31U) & ~31U) - aligned_addr;
    Cache_WriteBack_Addr((uint32_t)aligned_addr, (uint32_t)aligned_size);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    // `swap=true`: LVGL stores RGB565 in CPU-native (little-endian) byte
    // order, but the ST7789 wants MSB-first. The DMA overload with swap
    // handles this — the raw writePixelsDMA(uint16_t*, len) variant does
    // NOT and produces a green/yellow color mash.
    tft.writePixelsDMA((uint16_t*)px_map, (int32_t)(w * h), true);
    _dma_pending = true;
    _flush_count++;
    // Intentionally NOT endWrite()-ing — let DMA run in background. The next
    // flush_cb call drains it before re-using the bus.
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

    // Allocate render buffers in PSRAM. ps_malloc returns nullptr if PSRAM
    // is absent (base XIAO ESP32-S3 variant); fall back to internal heap so
    // boards without PSRAM still work.
    _disp_buf1 = (lv_color_t*)ps_malloc(DISP_BUF_BYTES);
    _disp_buf2 = (lv_color_t*)ps_malloc(DISP_BUF_BYTES);
    if (!_disp_buf1 || !_disp_buf2) {
        if (_disp_buf1) free(_disp_buf1);
        if (_disp_buf2) free(_disp_buf2);
        _disp_buf1 = (lv_color_t*)heap_caps_malloc(DISP_BUF_BYTES, MALLOC_CAP_8BIT);
        _disp_buf2 = (lv_color_t*)heap_caps_malloc(DISP_BUF_BYTES, MALLOC_CAP_8BIT);
    }
    lv_display_set_buffers(disp, _disp_buf1, _disp_buf2,
                           DISP_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // EEZ: ui_init() runs eez_flow_init which calls create_screens() and then
    // replacePageHook(1,...) — page 1 is the first entry in screen_names[],
    // which is Main Menu. So Main Menu is the default screen after ui_init().
    ui_init();

    // Replace the EEZ-baked version strings with live values from version.h.
    // BUILD_DATE_STR and UI_VERSION_STR are regenerated each build by
    // scripts/build_info.py.
    lv_label_set_text_fmt(objects.version_info,
                          "%s\n%s\n%s\n%s\n%s",
                          HW_REV_STR, FW_VERSION_STR, UI_VERSION_STR,
                          GAME_VERSION_STR, BUILD_DATE_STR);

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
    bus.subscribe(EV_BTN_CENTER_HOLD,   this);
}

void UIController::tick() {
    if (!_render_enabled) return;
    ui_tick();          // EEZ Flow runtime + per-screen tick handlers
    lv_timer_handler();
}

void UIController::getDisplayStats(uint32_t& flushes_out, uint32_t& elapsed_ms_out) {
    uint32_t now = millis();
    flushes_out    = _flush_count;
    elapsed_ms_out = now - _flush_last_sample_ms;
    _flush_count          = 0;
    _flush_last_sample_ms = now;
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
            // Main menu has no "previous" — ignore the regular long-press
            // there. Sleep is reached via the longer EV_BTN_CENTER_HOLD.
            if (lv_screen_active() != objects.main_menu) {
                g_vibe.play(HAPTIC_BUMP);
                eez_flow_pop_screen(LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
            }
            break;

        case EV_BTN_CENTER_HOLD:
            // Sleep / screen-off only triggers on main menu (matches the
            // shipping-wake hold duration). PowerManager fires its own
            // confirmation haptic synchronously since vibe_service.tick()
            // won't run during the sleep teardown.
            if (lv_screen_active() == objects.main_menu) {
                g_power.requestSleepOrScreenOff();
            }
            break;

        default:
            break;
    }
}
