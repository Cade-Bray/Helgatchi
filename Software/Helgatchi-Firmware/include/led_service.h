#pragma once
#include "event_bus.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// LED pattern catalog
//
// Patterns are referenced by enum value (1 byte). Adding a new look:
//   1. Add an enum entry below
//   2. Add a renderer function in led_service.cpp
//   3. Add a case in the dispatch switch
//
// Layering: an "alert" pattern preempts whatever ambient pattern is currently
// active. When an alert ends (timeout or EV_ALERT_CLEARED), we fade-out over
// 500 ms and the underlying ambient resumes.
// ---------------------------------------------------------------------------

enum LedPatternId : uint8_t {
    LED_PATTERN_OFF = 0,

    // --- Ambient (device-state driven) ---
    LED_PATTERN_CHARGING,         // slow green pulse
    LED_PATTERN_FULLY_CHARGED,    // steady dim green
    LED_PATTERN_SERIAL,           // cyan comet rotation
    LED_PATTERN_LOW_BATTERY,      // slow red pulse

    // --- Alert effects (rule-driven, neutral names) ---
    LED_PATTERN_ALERT_DEFAULT,    // red strobe (fallback when rule has none)
    LED_PATTERN_RED_BLUE_CHASER,  // 3+3 split, alternating red/blue strobe
    LED_PATTERN_RAINBOW_FAST,     // rainbow rotation, ~1.5s/lap
    LED_PATTERN_RAINBOW_SLOW,     // rainbow rotation, ~5s/lap
    LED_PATTERN_WHITE_CHASER,     // single white LED chasing the ring with fast fade

    LED_PATTERN_COUNT,
};

class LedService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Trigger an alert pattern manually (rules engine, serial console testing).
    // duration_ms = 0 means "play indefinitely until EV_ALERT_CLEARED arrives".
    // SKEY_ALERT_LED gates EV_ALERT_RAISED-driven calls but NOT this method —
    // explicit programmatic triggers always fire.
    void playAlertPattern(LedPatternId pattern, uint32_t duration_ms);

private:
    EventBus* _bus = nullptr;

    // Layer state
    LedPatternId _ambient = LED_PATTERN_OFF;
    LedPatternId _alert   = LED_PATTERN_OFF;
    uint32_t     _alert_until_ms      = 0;  // millis() when alert expires (0 = no expiry)
    uint32_t     _alert_fade_start_ms = 0;  // millis() when fade-out began (0 = not fading)

    // Cached ambient inputs
    bool _is_charging = false;
    bool _is_charged  = false;
    bool _is_low_batt = false;
    bool _last_serial = false;

    uint32_t _last_render_ms = 0;

    void _recomputeAmbient();
};

extern LedService g_leds;
