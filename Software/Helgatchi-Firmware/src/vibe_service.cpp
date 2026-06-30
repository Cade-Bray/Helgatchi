#include "vibe_service.h"
#include "hal.h"
#include "settings_service.h"
#include "alerts_service.h"
#include "event_payload.h"
#include <Arduino.h>
#include <strings.h>   // strcasecmp

VibeService g_vibe;

// ---------------------------------------------------------------------------
// Name registry — string identifiers for each HapticPatternId. Order must
// match the enum; the static_assert catches drift at compile time.
// ---------------------------------------------------------------------------

static const char* const s_vibe_name[] = {
    "off",           // HAPTIC_OFF
    "tick_light",    // HAPTIC_TICK_LIGHT
    "tick",          // HAPTIC_TICK
    "bump",          // HAPTIC_BUMP
    "double_tap",    // HAPTIC_DOUBLE_TAP
    "long_buzz",     // HAPTIC_LONG_BUZZ
};
static_assert(sizeof(s_vibe_name) / sizeof(s_vibe_name[0]) == HAPTIC_PATTERN_COUNT,
              "s_vibe_name out of sync with HapticPatternId");

const char* vibePatternName(HapticPatternId id) {
    if (id >= HAPTIC_PATTERN_COUNT) return "?";
    return s_vibe_name[id];
}

HapticPatternId vibePatternByName(const char* name) {
    if (!name || !*name) return HAPTIC_PATTERN_COUNT;
    for (uint8_t i = 0; i < HAPTIC_PATTERN_COUNT; i++) {
        if (strcasecmp(name, s_vibe_name[i]) == 0) return (HapticPatternId)i;
    }
    return HAPTIC_PATTERN_COUNT;
}

// ---------------------------------------------------------------------------
// Pattern definitions
//
// Each pattern is an array of {intensity, duration_ms} steps terminated by
// {0, 0}. The state machine in tick() advances through them, writing the
// step's intensity to the motor and ticking duration_ms before moving on.
// After the terminator, the motor is driven to 0 and _current goes OFF.
// ---------------------------------------------------------------------------

struct Step {
    uint8_t  intensity;
    uint16_t duration_ms;
};

// Intensity numbers are uint8 PWM duty (0..255). For ERM motors, anything
// below ~150 just whines without spinning — the eccentric mass needs enough
// average voltage to overcome static friction. The "minor" feel of TICK_LIGHT
// comes from a *short* duration, not a low duty cycle.
static const Step PAT_TICK_LIGHT[] = { {255,  35}, {0, 0} };
static const Step PAT_TICK[]       = { {220,  45}, {0, 0} };
static const Step PAT_BUMP[]       = { {255,  70}, {0, 0} };
static const Step PAT_DOUBLE_TAP[] = { {220,  40}, {0, 60}, {220, 40}, {0, 0} };
static const Step PAT_LONG_BUZZ[]  = { {255, 500}, {0, 0} };

// Indexed by HapticPatternId. HAPTIC_OFF maps to nullptr — "no steps".
static const Step* const PATTERNS[HAPTIC_PATTERN_COUNT] = {
    nullptr,            // HAPTIC_OFF
    PAT_TICK_LIGHT,     // HAPTIC_TICK_LIGHT
    PAT_TICK,           // HAPTIC_TICK
    PAT_BUMP,           // HAPTIC_BUMP
    PAT_DOUBLE_TAP,     // HAPTIC_DOUBLE_TAP
    PAT_LONG_BUZZ,      // HAPTIC_LONG_BUZZ
};

// ---------------------------------------------------------------------------

void VibeService::begin(EventBus& bus) {
    _bus = &bus;

    // Direct subscription to button events: every button press fires a haptic
    // tick automatically, regardless of which screen the UI is on.
    bus.subscribe(EV_BTN_LEFT,         this);
    bus.subscribe(EV_BTN_RIGHT,        this);
    bus.subscribe(EV_BTN_CENTER_SHORT, this);
    bus.subscribe(EV_BTN_CENTER_LONG,  this);

    // Alerts: rules engine fires EV_ALERT_RAISED, we pick a pattern.
    bus.subscribe(EV_ALERT_RAISED, this);
}

void VibeService::tick() {
    if (_current == HAPTIC_OFF || _steps == nullptr) return;

    const Step* steps = static_cast<const Step*>(_steps);
    const Step& step  = steps[_step_index];

    // {0, 0} sentinel — pattern complete.
    if (step.duration_ms == 0) {
        g_hal.stopVibrate();
        _current    = HAPTIC_OFF;
        _steps      = nullptr;
        _step_index = 0;
        return;
    }

    if (millis() - _step_start_ms >= step.duration_ms) {
        // Advance to next step and write its intensity. The next iteration
        // will check the sentinel if we just walked off the end.
        _step_index++;
        _step_start_ms = millis();
        const Step& next = steps[_step_index];
        g_hal.setVibrate(next.intensity);
    }
}

void VibeService::onEvent(const Event& e) {
    switch (e.id) {
        case EV_BTN_LEFT:
        case EV_BTN_RIGHT:
            play(HAPTIC_TICK_LIGHT);
            break;

        case EV_BTN_CENTER_SHORT:
            play(HAPTIC_TICK);
            break;

        case EV_BTN_CENTER_LONG:
            play(HAPTIC_BUMP);
            break;

        case EV_ALERT_RAISED: {
            // SKEY_ALERT_VIBRATION gates alerts only — button-press haptics
            // and direct play() calls bypass this.
            if (!g_settings.getBool(SKEY_ALERT_VIBRATION)) break;
            // Look up the alert's per-record vibe pattern. AlertsService is
            // the source of truth — AlertPayload only carries the alert_id,
            // not the pattern itself. Fall back to a sensible default if
            // the record is gone (race-cleared or unknown id).
            const AlertRecord* rec = g_alerts.find(e.data.alert.alert_id);
            play(rec ? rec->vibe : HAPTIC_DOUBLE_TAP);
            break;
        }

        default:
            break;
    }
}

void VibeService::play(HapticPatternId pattern) {
    if (pattern >= HAPTIC_PATTERN_COUNT) return;
    if (pattern == HAPTIC_OFF) {
        g_hal.stopVibrate();
        _current = HAPTIC_OFF;
        _steps   = nullptr;
        return;
    }

    const Step* steps = PATTERNS[pattern];
    if (!steps) return;

    _current       = pattern;
    _steps         = steps;
    _step_index    = 0;
    _step_start_ms = millis();
    g_hal.setVibrate(steps[0].intensity);
}
