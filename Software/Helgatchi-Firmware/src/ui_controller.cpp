#include "ui_controller.h"
#include "hal.h"
#include "settings_service.h"
#include "alerts_service.h"
#include "version.h"
#include "UI/ui.h"
#include "UI/screens.h"
#include "UI/styles.h"
#include "UI/eez-flow.h"
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

// ---------------------------------------------------------------------------
// Alerts list — dynamic cards rendered into objects.alert_container.
//
// Architecture: EEZ owns the visuals (Alert user widget design, styles,
// container layout). C owns the data flow — we build one LVGL card per
// AlertRecord whenever the alerts change. The card replicates the Alert
// user widget's tree structure (container + 2 labels + dismiss button)
// using EEZ-exported styles (`add_style_alert_card`,
// `add_style_focused___button`), so visuals match the design exactly.
//
// Dismiss flow mirrors EEZ's "Fade and Hide Alert" user action:
// `lv_anim_t` opacity 255→0 over 500 ms with EASE_OUT, then completion
// callback calls g_alerts.ack() which emits EV_ALERT_CLEARED and triggers
// the next rebuild (which deletes this card).
// ---------------------------------------------------------------------------

struct AlertCard {
    lv_obj_t* card        = nullptr;
    lv_obj_t* time_label  = nullptr;
    lv_obj_t* dismiss_btn = nullptr;
    uint16_t  alert_id    = AlertsService::INVALID_ALERT;
    uint32_t  last_seen_ms = 0;          // cached for time-ago refresh
};

static AlertCard _alert_cards[AlertsService::MAX_ALERTS];
static uint8_t   _alert_card_count = 0;
static lv_timer_t* _alert_time_refresh_timer = nullptr;

static void _formatTimeAgo(char* buf, size_t buf_sz, uint32_t age_ms) {
    const uint32_t s = age_ms / 1000;
    if      (s < 60)   snprintf(buf, buf_sz, "%us ago",      (unsigned)s);
    else if (s < 3600) snprintf(buf, buf_sz, "%um %us ago",  (unsigned)(s / 60),   (unsigned)(s % 60));
    else               snprintf(buf, buf_sz, "%uh %um ago",  (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

static void _on_dismiss_anim_done(lv_anim_t* a) {
    auto* card = (lv_obj_t*)a->var;
    const uint16_t alert_id = (uint16_t)(uintptr_t)lv_obj_get_user_data(card);
    // Calling ack() emits EV_ALERT_CLEARED → UIController::onEvent →
    // _rebuildAlertCards() which deletes this widget. Don't lv_obj_del here
    // (would double-delete on the rebuild path).
    g_alerts.ack(alert_id);
}

static void _on_dismiss_click(lv_event_t* e) {
    auto* card = (lv_obj_t*)lv_event_get_user_data(e);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 500);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_completed_cb(&a, _on_dismiss_anim_done);
    lv_anim_start(&a);
}

// Builds one alert card mirroring the Alert user widget definition in EEZ.
// See create_user_widget_alert() in src/UI/screens.c for the source-of-truth
// hierarchy and styling.
static void _buildAlertCard(lv_obj_t* parent, const AlertRecord* rec, AlertCard* out) {
    lv_obj_t* card = lv_obj_create(parent);
    add_style_alert_card(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(card, (void*)(uintptr_t)rec->id);

    // Title label — top-left, Montserrat 16. Prefixed with a type icon
    // + single space so the source is identifiable at a glance. Skipped
    // entirely if the type has no symbol.
    const char* type_sym = nullptr;
    switch (rec->type) {
        case ALERT_BLE:          type_sym = LV_SYMBOL_BLUETOOTH;     break;
        case ALERT_WIFI:         type_sym = LV_SYMBOL_WIFI;          break;
        case ALERT_SYSTEM:       type_sym = LV_SYMBOL_BELL;          break;
        case ALERT_BATTERY_LOW:  type_sym = LV_SYMBOL_BATTERY_EMPTY; break;
        default:                 type_sym = nullptr;                 break;
    }
    char title_buf[sizeof(rec->title) + 8];
    if (type_sym && type_sym[0]) {
        snprintf(title_buf, sizeof(title_buf), "%s %s", type_sym, rec->title);
    } else {
        snprintf(title_buf, sizeof(title_buf), "%s", rec->title);
    }

    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_size(title, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(title, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(title, title_buf);

    // Time-ago label — bottom-left, Montserrat 12, theme accent color [2].
    lv_obj_t* time_label = lv_label_create(card);
    lv_obj_set_size(time_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(time_label, LV_ALIGN_BOTTOM_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(time_label,
        lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][2]),
        LV_PART_MAIN | LV_STATE_DEFAULT);
    char buf[24];
    _formatTimeAgo(buf, sizeof(buf), millis() - rec->last_seen_ms);
    lv_label_set_text(time_label, buf);

    // Dismiss button — right side, with "Dismiss" label child.
    lv_obj_t* dismiss = lv_button_create(card);
    lv_obj_set_size(dismiss, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    add_style_focused___button(dismiss);
    lv_obj_add_event_cb(dismiss, _on_dismiss_click, LV_EVENT_CLICKED, card);
    lv_obj_t* dismiss_label = lv_label_create(dismiss);
    lv_label_set_text_static(dismiss_label, "Dismiss");
    lv_obj_set_style_align(dismiss_label, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    out->card         = card;
    out->time_label   = time_label;
    out->dismiss_btn  = dismiss;
    out->alert_id     = rec->id;
    out->last_seen_ms = rec->last_seen_ms;
}

static void _refreshTimeLabels() {
    const uint32_t now = millis();
    char buf[24];
    for (uint8_t i = 0; i < _alert_card_count; i++) {
        if (!_alert_cards[i].time_label) continue;
        _formatTimeAgo(buf, sizeof(buf), now - _alert_cards[i].last_seen_ms);
        lv_label_set_text(_alert_cards[i].time_label, buf);
    }
}

static void _alertTimeTimerCb(lv_timer_t* /*t*/) {
    _refreshTimeLabels();
}

// EV_ALERT_RAISED handler — inserts a new card at the TOP of the list.
// Newest alert visible first; older cards slide down. The no_alerts_label
// (which lives in the same container) is hidden when there are alerts, so
// putting the new card at child index 0 places it visually at the top.
static void _onAlertRaised(uint16_t alert_id) {
    if (!objects.alert_container) return;
    if (_alert_card_count >= AlertsService::MAX_ALERTS) return;
    const AlertRecord* rec = g_alerts.find(alert_id);
    if (!rec) return;

    // Shift the tracking array down so index 0 is the freshest card —
    // keeps _alert_cards[] aligned with on-screen render order.
    for (uint8_t i = _alert_card_count; i > 0; i--) {
        _alert_cards[i] = _alert_cards[i - 1];
    }
    AlertCard& slot = _alert_cards[0];
    slot = AlertCard{};
    _buildAlertCard(objects.alert_container, rec, &slot);
    _alert_card_count++;

    // lv_obj_create appends to the end of the parent's child list. Move to
    // index 0 so the flex layout places it at the top.
    lv_obj_move_to_index(slot.card, 0);

    // If the alerts screen is the active screen, sync the keypad nav group
    // to match visual order. LVGL 9 lv_group has no insert-at-index, so we
    // clear and re-add — cheap with ≤16 entries. The alerts screen has no
    // other widgets in this group (EEZ's screen-load handler clears it),
    // so wholesale clear is safe.
    if (lv_screen_active() == objects.alerts) {
        lv_group_remove_all_objs(groups.UINavigation);
        for (uint8_t i = 0; i < _alert_card_count; i++) {
            if (_alert_cards[i].dismiss_btn) {
                lv_group_add_obj(groups.UINavigation, _alert_cards[i].dismiss_btn);
            }
        }
    }
}

// EV_ALERT_UPDATED handler — dedup hit. Refresh the existing card's time
// label to "0s ago" immediately (rather than waiting up to a second for the
// next tick of the refresh timer).
static void _onAlertUpdated(uint16_t alert_id) {
    const AlertRecord* rec = g_alerts.find(alert_id);
    if (!rec) return;
    for (uint8_t i = 0; i < _alert_card_count; i++) {
        if (_alert_cards[i].alert_id != alert_id) continue;
        _alert_cards[i].last_seen_ms = rec->last_seen_ms;
        if (_alert_cards[i].time_label) {
            lv_label_set_text(_alert_cards[i].time_label, "0s ago");
        }
        return;
    }
}

// EV_ALERT_CLEARED handler — locate the card, delete it, compact the array.
// LVGL flex reflows the remaining cards automatically.
static void _onAlertCleared(uint16_t alert_id) {
    for (uint8_t i = 0; i < _alert_card_count; i++) {
        if (_alert_cards[i].alert_id != alert_id) continue;
        if (_alert_cards[i].card && lv_obj_is_valid(_alert_cards[i].card)) {
            lv_obj_del(_alert_cards[i].card);
        }
        for (uint8_t j = i; j + 1 < _alert_card_count; j++) {
            _alert_cards[j] = _alert_cards[j + 1];
        }
        _alert_card_count--;
        _alert_cards[_alert_card_count] = AlertCard{};
        return;
    }
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

    // Alert list emptiness drives the "No active alerts" placeholder label
    // EEZ designed into the Alert Container. Logic-side (when to show) lives
    // here; the label's existence/style/position is owned by EEZ.
    bus.subscribe(EV_ALERT_RAISED,  this);
    bus.subscribe(EV_ALERT_UPDATED, this);
    bus.subscribe(EV_ALERT_CLEARED, this);
    _refreshNoAlertsLabel();        // initial state: count == 0 → label shown

    // Alert cards are LVGL widgets built in C, one per AlertRecord. Each
    // alert event adds/updates/removes a single card; no global rebuild.
    // 1 Hz timer refreshes the "Xs ago" labels on all live cards.
    _alert_time_refresh_timer = lv_timer_create(_alertTimeTimerCb, 1000, nullptr);

    // Restore cards for any alerts that survived a deep-sleep wake (alerts
    // store persists in RTC slow memory). Iterate in reverse so the newest
    // ends up at index 0 — each _onAlertRaised shifts existing cards down.
    for (int i = (int)g_alerts.count() - 1; i >= 0; i--) {
        const AlertRecord* rec = g_alerts.get((uint8_t)i);
        if (rec) _onAlertRaised(rec->id);
    }

    // Repopulate keypad nav group with dismiss buttons when the alerts
    // screen loads (EEZ's own handler clears the group first).
    if (objects.alerts) {
        lv_obj_add_event_cb(objects.alerts, [](lv_event_t* /*e*/) {
            for (uint8_t i = 0; i < _alert_card_count; i++) {
                if (_alert_cards[i].dismiss_btn) {
                    lv_group_add_obj(groups.UINavigation, _alert_cards[i].dismiss_btn);
                }
            }
        }, LV_EVENT_SCREEN_LOAD_START, nullptr);
    }
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
            eez_flow_pop_screen(LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
            break;

        case EV_ALERT_RAISED:
            _onAlertRaised(e.data.alert.alert_id);
            _refreshNoAlertsLabel();
            break;

        case EV_ALERT_UPDATED:
            _onAlertUpdated(e.data.alert.alert_id);
            break;

        case EV_ALERT_CLEARED:
            _onAlertCleared(e.data.alert.alert_id);
            _refreshNoAlertsLabel();
            break;

        default:
            break;
    }
}

void UIController::_refreshNoAlertsLabel() {
    // Hidden when there are any alerts to show, visible when the list is
    // empty. The label itself is created and styled in EEZ Studio.
    const bool any = g_alerts.count() > 0;
    if (objects.no_alerts_label) {
        if (any) lv_obj_add_flag   (objects.no_alerts_label, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_remove_flag(objects.no_alerts_label, LV_OBJ_FLAG_HIDDEN);
    }
}
