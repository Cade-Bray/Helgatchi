#include "log_service.h"
#include "power_manager.h"
#include "scan_service.h"
#include "scan_engine.h"
#include "rules_service.h"
#include "devices_screen.h"
#include "perf_stats.h"
#include <Arduino.h>
#include <lvgl.h>

LogService g_logger;

// ---------------------------------------------------------------------------
// Level definitions (matches DebugLevel enum / SLS Debug Level dropdown)
//
//   DEBUG_INFORMATIONAL  Sleep-soon warnings, sleep entry, scan window
//                        boundaries, settings changes, alerts.
//   DEBUG_HIGH           All INFO + UI activity, button presses, battery,
//                        every sleep-countdown tick.
//   DEBUG_RENDERING_PERF Quiet on the event firehose; emits a one-line perf
//                        summary every second + flips on the LVGL FPS overlay.
//   DEBUG_SCANNING_PERF  Scan-focused trace — only CMD_SCAN_START/STOP,
//                        EV_SCAN_STATE_CHANGED, and alert events flow through
//                        the bus logger. ScanEngine dumps each raw BLE
//                        advertisement as it's drained from the queue.
// ---------------------------------------------------------------------------

// Minimum level at which a given event id should be logged. Returning
// DEBUG_LEVEL_COUNT means "never log this event regardless of level".
static DebugLevel _minLevelForEvent(EventId id) {
    switch (id) {
        // INFO tier — significant state changes.
        case EV_POWER_STATE_CHANGED:
        case EV_SCAN_STATE_CHANGED:
        case EV_SETTINGS_CHANGED:
        case EV_ALERT_RAISED:
        case EV_ALERT_UPDATED:
        case EV_ALERT_CLEARED:
        case EV_ALERT_SNOOZED:
        case CMD_SCAN_START:
        case CMD_SCAN_STOP:
        case CMD_POWER_SLEEP:
        case CMD_POWER_SHIPPING_SLEEP:
        case CMD_POWER_SHIPPING_RESET:
        case CMD_POWER_REBOOT:
        case CMD_SETTINGS_RESET_DEFAULTS:
            return DEBUG_INFORMATIONAL;

        // HIGH tier — user-visible interaction & periodic.
        case EV_UI_ACTIVITY:
        case EV_BTN_LEFT:
        case EV_BTN_RIGHT:
        case EV_BTN_CENTER_SHORT:
        case EV_BTN_CENTER_LONG:
        case EV_BTN_CENTER_HOLD:
        case EV_BATTERY_UPDATED:
        case CMD_SETTINGS_SET:
        case CMD_SETTINGS_SAVE:
        case CMD_ALERT_ACK:
        case CMD_ALERT_SNOOZE:
            return DEBUG_HIGH;

        default:
            return DEBUG_LEVEL_COUNT;  // not in either tier
    }
}

// Custom override for SLEEP_COUNTDOWN: log at INFO only when ≤5 s remain
// (the dim warning) AND it's not the inhibit sentinel; log every tick at HIGH+.
static bool _shouldLogSleepCountdown(uint16_t seconds, DebugLevel level) {
    if (level >= DEBUG_HIGH) return seconds != 0xFFFF;  // skip noisy inhibit sentinel
    // INFO: only the final 5s warning ticks.
    return seconds > 0 && seconds <= 5 && seconds != 0xFFFF;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void LogService::begin(EventBus& bus) {
    _syncSettings();
    bus.subscribeAll(this);
    _applyPerfMonitor();
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void LogService::onEvent(const Event& e) {
    if (e.id == EV_SETTINGS_CHANGED) {
        bool was_overlay = (_debug_level == DEBUG_RENDERING_PERF);
        bool was_perf    = (_debug_level == DEBUG_PERF);
        _syncSettings();
        bool is_overlay = (_debug_level == DEBUG_RENDERING_PERF);
        if (was_overlay != is_overlay) _applyPerfMonitor();
        // Entering PERF: start the timing window + rate baselines fresh so the
        // first telemetry line isn't skewed by whatever accumulated at other
        // levels.
        if (!was_perf && _debug_level == DEBUG_PERF) {
            g_loop_perf.reset();
            _last_cb      = g_scan_engine.callbacks();
            _last_pub     = g_scan_engine.published();
            _last_perf_ms = millis();
        }
    }

    if (!_enabled) return;

    // RENDERING_PERF: one perf summary line per second on EV_TICK_1S, suppress
    // all other event firehose. Plus the LVGL FPS+CPU overlay.
    if (_debug_level == DEBUG_RENDERING_PERF) {
        if (e.id == EV_TICK_1S) _emitPerfLine();
        return;
    }

    // PERF: periodic combined telemetry (memory + scan pressure + loop timing)
    // once per second on EV_TICK_1S; suppress the rest of the event firehose.
    if (_debug_level == DEBUG_PERF) {
        if (e.id == EV_TICK_1S) _emitPerfTelemetry();
        return;
    }

    // SCANNING_PERF: scan-related bus events only. ScanEngine separately
    // dumps every raw advertisement (it doesn't go through the bus).
    if (_debug_level == DEBUG_SCANNING_PERF) {
        switch (e.id) {
            case CMD_SCAN_START:
            case CMD_SCAN_STOP:
            case EV_SCAN_STATE_CHANGED:
            case EV_ALERT_RAISED:
            case EV_ALERT_UPDATED:
            case EV_ALERT_CLEARED:
                break;
            default:
                return;
        }
        // Fall through to the regular formatter below.
    } else if (e.id == EV_SLEEP_COUNTDOWN_UPDATED) {
        // INFO/HIGH: per-event filter, plus the sleep-countdown override.
        if (!_shouldLogSleepCountdown(e.data.sleep_count.seconds, _debug_level)) return;
    } else {
        DebugLevel min = _minLevelForEvent(e.id);
        if (min >= DEBUG_LEVEL_COUNT) return;     // never logged
        if (_debug_level < min)       return;     // current level too low
    }

    Serial.printf("[%8lu] %s", millis(), _eventName(e.id));

    switch (e.id) {
        case EV_SETTINGS_CHANGED:
            Serial.printf("  mask=0x%08lX  seq=%u",
                          (unsigned long)e.data.settings.mask,
                          e.data.settings.version);
            break;
        case EV_SCAN_STATE_CHANGED:
            Serial.printf("  state=%u", e.data.scan_state.state);
            break;
        case EV_ENTITY_UPDATED:
            Serial.printf("  id=%lu", (unsigned long)e.data.entity.entity_id);
            break;
        case EV_ALERT_RAISED:
        case EV_ALERT_UPDATED:
        case EV_ALERT_CLEARED:
        case EV_ALERT_SNOOZED:
            Serial.printf("  alert_id=%u  state=%u",
                          e.data.alert.alert_id, e.data.alert.state);
            break;
        case EV_POWER_STATE_CHANGED:
            Serial.printf("  state=%s",
                          e.data.power.state == POWER_SLEEPING ? "SLEEPING" : "AWAKE");
            break;
        case EV_BATTERY_UPDATED: {
            uint8_t pct = e.data.battery.pct;
            if      (pct == BATT_PCT_CHARGING) Serial.printf("  mv=%u  CHARGING",  e.data.battery.mv);
            else if (pct == BATT_PCT_CHARGED)  Serial.printf("  mv=%u  CHARGED",   e.data.battery.mv);
            else if (pct == BATT_PCT_MISSING)  Serial.printf("  MISSING/FAULT");
            else                               Serial.printf("  mv=%u  pct=%u%%",  e.data.battery.mv, pct);
            break;
        }
        case EV_SLEEP_COUNTDOWN_UPDATED:
            Serial.printf("  sleep_in=%u s", e.data.sleep_count.seconds);
            break;
        case CMD_SETTINGS_SET:
            Serial.printf("  key=%u  val=%lu",
                          e.data.settings_set.key,
                          (unsigned long)e.data.settings_set.value);
            break;
        default:
            break;
    }

    Serial.println();
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void LogService::_syncSettings() {
    _enabled = g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED);
    uint32_t lvl = g_settings.get(SKEY_DEBUG_LEVEL);
    if (lvl >= DEBUG_LEVEL_COUNT) lvl = DEBUG_INFORMATIONAL;
    _debug_level = (DebugLevel)lvl;
}

// LVGL routes all its log output here. Gated to DEBUG_RENDERING_PERF so LVGL's
// warnings/errors — notably the "lv_malloc failed" that fires right before the
// LV_ASSERT_HANDLER while(1) halt — surface in "Render" debug mode and nowhere
// else. Reads settings directly so it stays valid regardless of LogService
// state.
static void _lvglLogCb(lv_log_level_t /*level*/, const char* buf) {
    if (!g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED)) return;
    if (g_settings.get(SKEY_DEBUG_LEVEL) != DEBUG_RENDERING_PERF) return;
    Serial.print(buf);
}

void LogService::attachLvglLog() {
    lv_log_register_print_cb(_lvglLogCb);
}

void LogService::_applyPerfMonitor() {
    // Show the LVGL FPS+CPU overlay at RENDERING_PERF or above; hide otherwise.
#if LV_USE_PERF_MONITOR
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return;
    if (_debug_level == DEBUG_RENDERING_PERF) {
        lv_sysmon_show_performance(disp);
    } else {
        lv_sysmon_hide_performance(disp);
    }
    // hide_performance only flips LV_OBJ_FLAG_HIDDEN — it doesn't invalidate
    // the area the overlay was painted to, so the stale pixels stay on screen
    // until something else triggers a redraw. Force a full screen invalidate.
    lv_obj_t* scr = lv_screen_active();
    if (scr) lv_obj_invalidate(scr);
#endif
}

void LogService::_emitPerfLine() {
    if (!_enabled) return;
    auto p = g_bus.perfSnapshotAndReset();
    uint32_t avg_us = p.dispatches ? (p.total_us / p.dispatches) : 0;
    Serial.printf("[%8lu] PERF  events=%lu  disp=%lu  avg_us=%lu  max_us=%lu  drops=%lu  heap=%lu\n",
                  millis(),
                  (unsigned long)p.events,
                  (unsigned long)p.dispatches,
                  (unsigned long)avg_us,
                  (unsigned long)p.max_us,
                  (unsigned long)g_bus.droppedCount(),
                  (unsigned long)ESP.getFreeHeap());
}

// DEBUG_PERF: three lines per second capturing the state that precedes a
// dense-scan lockup, so the LAST lines in a tester's log show what was
// happening right before the freeze:
//   mem   — internal heap + PSRAM free/min, LVGL pool used/frag (OOM watch)
//   scan  — seen-map size, card count, callback/publish rates, drops, noise
//   loop  — worst per-phase micros this window; a slow phase names the culprit
void LogService::_emitPerfTelemetry() {
    if (!_enabled) return;
    const unsigned long now = millis();

    // --- Memory ---
    lv_mem_monitor_t lv;
    lv_mem_monitor(&lv);
    Serial.printf("[%8lu] PERF mem   heap=%luk (min %luk)  psram=%luk (min %luk)  "
                  "lv=%luk/%luk frag=%u%%\n",
                  now,
                  (unsigned long)(ESP.getFreeHeap()     / 1024),
                  (unsigned long)(ESP.getMinFreeHeap()  / 1024),
                  (unsigned long)(ESP.getFreePsram()    / 1024),
                  (unsigned long)(ESP.getMinFreePsram() / 1024),
                  (unsigned long)((lv.total_size - lv.free_size) / 1024),
                  (unsigned long)(lv.total_size / 1024),
                  (unsigned)lv.frag_pct);

    // --- Scan pressure --- (cb/pub are deltas since last line ≈ per second)
    const uint32_t cb  = g_scan_engine.callbacks();
    const uint32_t pub = g_scan_engine.published();
    const uint32_t cb_rate  = cb  - _last_cb;
    const uint32_t pub_rate = pub - _last_pub;
    _last_cb  = cb;
    _last_pub = pub;
    const unsigned seen  = (unsigned)g_scan.seenCount();
    const unsigned cards = (unsigned)g_devices_screen.cardCount();
    const uint32_t qovf  = g_scan_engine.queueOverflows();
    const uint32_t lost  = g_rules.lostScans();
    const uint32_t noise = g_scan.noiseFiltered();
    Serial.printf("[%8lu] PERF scan  seen=%u cards=%u  cb=%lu/s pub=%lu/s  "
                  "qovf=%lu lost=%lu noise=%lu\n",
                  now, seen, cards,
                  (unsigned long)cb_rate, (unsigned long)pub_rate,
                  (unsigned long)qovf, (unsigned long)lost, (unsigned long)noise);

    // --- Loop timing --- (worst single-iteration micros per phase this window)
    const LoopPerf lp = g_loop_perf;
    g_loop_perf.reset();
    const uint32_t elapsed_ms = (_last_perf_ms && now > _last_perf_ms)
                                ? (uint32_t)(now - _last_perf_ms) : 0;
    _last_perf_ms = now;
    const uint32_t loop_hz = elapsed_ms
                             ? (uint32_t)((uint64_t)lp.iterations * 1000 / elapsed_ms) : 0;
    Serial.printf("[%8lu] PERF loop  iters=%lu (%lu Hz)  max us: hal=%lu bus=%lu "
                  "con=%lu pwr=%lu scan=%lu rules=%lu led=%lu ui=%lu  whole=%lu\n",
                  now,
                  (unsigned long)lp.iterations, (unsigned long)loop_hz,
                  (unsigned long)lp.hal_us,   (unsigned long)lp.bus_us,
                  (unsigned long)lp.console_us,(unsigned long)lp.power_us,
                  (unsigned long)lp.scan_us,  (unsigned long)lp.rules_us,
                  (unsigned long)lp.leds_us,
                  (unsigned long)lp.ui_us,    (unsigned long)lp.loop_us);

    // A single iteration this long is what reads as a freeze — flag it loudly.
    static constexpr uint32_t SLOW_TICK_US = 200000;   // 200 ms
    if (lp.loop_us > SLOW_TICK_US) {
        Serial.printf("[%8lu] PERF WARN  slow loop: whole=%lu us "
                      "(bus=%lu ui=%lu) — likely device-list rebuild\n",
                      now,
                      (unsigned long)lp.loop_us,
                      (unsigned long)lp.bus_us,
                      (unsigned long)lp.ui_us);
    }

    // --- Teleplot stream --- (VS Code Teleplot: serial now, UDP later). Each
    // ">k:v" line is graphed; the human lines above are ignored by Teleplot but
    // stay readable in a raw log. loop_hz is the core-1 CPU-health proxy (drops
    // when a phase stalls); cb_rate/qovf proxy core-0 (BLE) pressure.
    Serial.printf(">heap:%lu\xC2\xA7KB\n>heap_min:%lu\xC2\xA7KB\n>psram:%lu\xC2\xA7KB\n"
                  ">lv_used:%lu\xC2\xA7KB\n>lv_frag:%u\n",
                  (unsigned long)(ESP.getFreeHeap()     / 1024),
                  (unsigned long)(ESP.getMinFreeHeap()  / 1024),
                  (unsigned long)(ESP.getFreePsram()    / 1024),
                  (unsigned long)((lv.total_size - lv.free_size) / 1024),
                  (unsigned)lv.frag_pct);
    Serial.printf(">seen:%u\n>cards:%u\n>cb_rate:%lu\n>pub_rate:%lu\n"
                  ">qovf:%lu\n>lost:%lu\n>noise:%lu\n",
                  seen, cards,
                  (unsigned long)cb_rate, (unsigned long)pub_rate,
                  (unsigned long)qovf, (unsigned long)lost, (unsigned long)noise);
    Serial.printf(">loop_hz:%lu\xC2\xA7Hz\n>loop_max:%lu\xC2\xA7us\n"
                  ">ui_max:%lu\xC2\xA7us\n>bus_max:%lu\xC2\xA7us\n",
                  (unsigned long)loop_hz,
                  (unsigned long)lp.loop_us,
                  (unsigned long)lp.ui_us,
                  (unsigned long)lp.bus_us);
}

const char* LogService::_eventName(EventId id) {
    switch (id) {
        case CMD_SCAN_START:             return "CMD_SCAN_START";
        case CMD_SCAN_STOP:              return "CMD_SCAN_STOP";
        case CMD_SCAN_LOCKON_START:      return "CMD_SCAN_LOCKON_START";
        case CMD_SCAN_LOCKON_STOP:       return "CMD_SCAN_LOCKON_STOP";
        case CMD_RULE_PACK_ENABLE:       return "CMD_RULE_PACK_ENABLE";
        case CMD_RULE_PACK_DISABLE:      return "CMD_RULE_PACK_DISABLE";
        case CMD_RULE_ADD_CUSTOM:        return "CMD_RULE_ADD_CUSTOM";
        case CMD_RULE_WIPE_CUSTOM:       return "CMD_RULE_WIPE_CUSTOM";
        case CMD_ALERT_ACK:              return "CMD_ALERT_ACK";
        case CMD_ALERT_SNOOZE:           return "CMD_ALERT_SNOOZE";
        case CMD_SETTINGS_SET:           return "CMD_SETTINGS_SET";
        case CMD_SETTINGS_SAVE:          return "CMD_SETTINGS_SAVE";
        case CMD_SETTINGS_RESET_DEFAULTS:return "CMD_SETTINGS_RESET_DEFAULTS";
        case CMD_POWER_SLEEP:            return "CMD_POWER_SLEEP";
        case CMD_POWER_SHIPPING_SLEEP:   return "CMD_POWER_SHIPPING_SLEEP";
        case CMD_POWER_SHIPPING_RESET:   return "CMD_POWER_SHIPPING_RESET";
        case CMD_POWER_REBOOT:           return "CMD_POWER_REBOOT";
        case CMD_STATS_RESET:            return "CMD_STATS_RESET";
        case CMD_UI_NAV_NEXT:            return "CMD_UI_NAV_NEXT";
        case CMD_UI_NAV_BACK:            return "CMD_UI_NAV_BACK";
        case CMD_UI_CONFIRM:             return "CMD_UI_CONFIRM";
        case EV_SCAN_STATE_CHANGED:      return "EV_SCAN_STATE_CHANGED";
        case EV_SCAN_COMPLETE:           return "EV_SCAN_COMPLETE";
        case EV_OBS_BATCH_READY:         return "EV_OBS_BATCH_READY";
        case EV_OBS_CANONICAL:           return "EV_OBS_CANONICAL";
        case EV_OBS_ENRICHED:            return "EV_OBS_ENRICHED";
        case EV_ENTITY_UPDATED:          return "EV_ENTITY_UPDATED";
        case EV_RULES_CHANGED:           return "EV_RULES_CHANGED";
        case EV_RULE_TRIGGERED_LOCAL:    return "EV_RULE_TRIGGERED_LOCAL";
        case EV_ALERT_RAISED:            return "EV_ALERT_RAISED";
        case EV_ALERT_UPDATED:           return "EV_ALERT_UPDATED";
        case EV_ALERT_CLEARED:           return "EV_ALERT_CLEARED";
        case EV_ALERT_SNOOZED:           return "EV_ALERT_SNOOZED";
        case EV_POWER_STATE_CHANGED:     return "EV_POWER_STATE_CHANGED";
        case EV_BATTERY_UPDATED:         return "EV_BATTERY_UPDATED";
        case EV_SLEEP_COUNTDOWN_UPDATED: return "EV_SLEEP_COUNTDOWN_UPDATED";
        case EV_SETTINGS_CHANGED:        return "EV_SETTINGS_CHANGED";
        case EV_UI_ACTIVITY:             return "EV_UI_ACTIVITY";
        case EV_MESH_RULE_FIRED_RX:      return "EV_MESH_RULE_FIRED_RX";
        case EV_TICK_1S:                 return "EV_TICK_1S";
        case EV_BTN_LEFT:                return "EV_BTN_LEFT";
        case EV_BTN_RIGHT:               return "EV_BTN_RIGHT";
        case EV_BTN_CENTER_SHORT:        return "EV_BTN_CENTER_SHORT";
        case EV_BTN_CENTER_LONG:         return "EV_BTN_CENTER_LONG";
        case EV_BTN_CENTER_HOLD:         return "EV_BTN_CENTER_HOLD";
        default:                         return "UNKNOWN";
    }
}
