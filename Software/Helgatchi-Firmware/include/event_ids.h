#pragma once
#include <stdint.h>

enum EventId : uint16_t {
    // --- Commands (imperative requests, may be ignored/rejected) ---
    CMD_SCAN_START = 0,
    CMD_SCAN_STOP,
    CMD_SCAN_LOCKON_START,
    CMD_SCAN_LOCKON_STOP,

    CMD_RULE_PACK_ENABLE,
    CMD_RULE_PACK_DISABLE,
    CMD_RULE_ADD_CUSTOM,
    CMD_RULE_WIPE_CUSTOM,

    CMD_ALERT_ACK,
    CMD_ALERT_SNOOZE,

    CMD_SETTINGS_SET,
    CMD_SETTINGS_SAVE,
    CMD_SETTINGS_RESET_DEFAULTS,

    CMD_POWER_SLEEP,
    CMD_POWER_SHIPPING_SLEEP,       // enter shipping sleep (no scheduled wakeup)
    CMD_POWER_SHIPPING_RESET,       // factory-reset NVS then enter shipping sleep

    CMD_STATS_RESET,                // wipe runtime scan/entity statistics

    CMD_UI_NAV_NEXT,
    CMD_UI_NAV_BACK,
    CMD_UI_CONFIRM,

    // --- Events (immutable facts, never rejected) ---
    EV_SCAN_STATE_CHANGED,
    EV_OBS_BATCH_READY,        // raw observation batch in ring buffer
    EV_OBS_CANONICAL,          // parser emits normalized observation
    EV_OBS_ENRICHED,           // lookup/enrichment layer emits annotated obs

    EV_ENTITY_UPDATED,

    EV_RULES_CHANGED,
    EV_RULE_TRIGGERED_LOCAL,

    EV_ALERT_RAISED,
    EV_ALERT_UPDATED,
    EV_ALERT_CLEARED,
    EV_ALERT_SNOOZED,

    EV_POWER_STATE_CHANGED,
    EV_BATTERY_UPDATED,
    EV_SLEEP_COUNTDOWN_UPDATED,

    EV_SETTINGS_CHANGED,

    EV_UI_ACTIVITY,

    EV_MESH_RULE_FIRED_RX,

    EV_TICK_1S,

    // --- Button events (normalized by HAL before UIController maps to CMD_*) ---
    EV_BTN_LEFT,            // left button debounced press
    EV_BTN_RIGHT,           // right button debounced press
    EV_BTN_CENTER_SHORT,    // center released before long-press threshold
    EV_BTN_CENTER_LONG,     // center held >= HAL_LONG_PRESS_MS

    EVENT_ID_COUNT,
    EVENT_ID_INVALID = 0xFFFF
};
