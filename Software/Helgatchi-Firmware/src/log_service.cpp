#include "log_service.h"
#include "power_manager.h"
#include <Arduino.h>
#include <lvgl.h>

LogService g_logger;

// ---------------------------------------------------------------------------
// Level definitions (matches DebugLevel enum / SLS Debug Level dropdown)
//
//   DEBUG_INFORMATIONAL  Sleep-soon warnings, sleep entry, scan start/stop,
//                        settings changes, alerts.
//   DEBUG_HIGH           All INFO + UI activity, button presses, battery,
//                        every sleep-countdown tick.
//   DEBUG_RENDERING_PERF Quiet on the event firehose; emits a one-line perf
//                        summary every second + flips on the LVGL FPS overlay.
//   DEBUG_SCANNING_PERF  Reserved for scanner-perf instrumentation. Treated
//                        as RENDERING_PERF for now.
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
        case EV_ALERT_CLEARED:
        case EV_ALERT_SNOOZED:
        case CMD_SCAN_START:
        case CMD_SCAN_STOP:
        case CMD_POWER_SLEEP:
        case CMD_POWER_SHIPPING_SLEEP:
        case CMD_POWER_SHIPPING_RESET:
        case CMD_SETTINGS_RESET_DEFAULTS:
            return DEBUG_INFORMATIONAL;

        // HIGH tier — user-visible interaction & periodic.
        case EV_UI_ACTIVITY:
        case EV_BTN_LEFT:
        case EV_BTN_RIGHT:
        case EV_BTN_CENTER_SHORT:
        case EV_BTN_CENTER_LONG:
        case EV_BATTERY_UPDATED:
        case EV_TICK_1S:
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
        bool was_perf = (_debug_level >= DEBUG_RENDERING_PERF);
        _syncSettings();
        bool is_perf = (_debug_level >= DEBUG_RENDERING_PERF);
        if (was_perf != is_perf) _applyPerfMonitor();
    }

    if (!_enabled) return;

    // PERF level: emit the perf summary once per second on EV_TICK_1S, and
    // suppress all other event firehose. Cleaner trace for performance work.
    if (_debug_level >= DEBUG_RENDERING_PERF) {
        if (e.id == EV_TICK_1S) _emitPerfLine();
        return;
    }

    // INFO/HIGH: per-event level filter, plus a couple of custom overrides.
    if (e.id == EV_SLEEP_COUNTDOWN_UPDATED) {
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

void LogService::_applyPerfMonitor() {
    // Show the LVGL FPS+CPU overlay at RENDERING_PERF or above; hide otherwise.
#if LV_USE_PERF_MONITOR
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return;
    if (_debug_level >= DEBUG_RENDERING_PERF) {
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
        case CMD_STATS_RESET:            return "CMD_STATS_RESET";
        case CMD_UI_NAV_NEXT:            return "CMD_UI_NAV_NEXT";
        case CMD_UI_NAV_BACK:            return "CMD_UI_NAV_BACK";
        case CMD_UI_CONFIRM:             return "CMD_UI_CONFIRM";
        case EV_SCAN_STATE_CHANGED:      return "EV_SCAN_STATE_CHANGED";
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
        default:                         return "UNKNOWN";
    }
}
