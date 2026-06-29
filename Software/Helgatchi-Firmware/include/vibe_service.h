#pragma once
#include "event_bus.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Haptic pattern catalog
//
// Patterns are short sequences of (intensity, duration) steps that VibeService
// plays out via PWM on the vibration motor. Patterns are fire-and-forget — a
// subsequent play() preempts whatever was running.
//
// Two trigger paths:
//   • Direct: g_vibe.play(HAPTIC_TICK_LIGHT) for instant UI haptics.
//   • Bus:    EV_ALERT_RAISED → VibeService picks the alert pattern.
//
// Both paths are gated by SKEY_ALERT_VIBRATION — turning that off silences
// every haptic. Future: add a separate SKEY_UI_HAPTICS toggle if users want
// to silence button taps independently of alerts.
// ---------------------------------------------------------------------------

enum HapticPatternId : uint8_t {
    HAPTIC_OFF = 0,
    HAPTIC_TICK_LIGHT,    // ~25 ms light pulse — left/right buttons
    HAPTIC_TICK,          // ~35 ms medium pulse — center button, boot/wake
    HAPTIC_BUMP,          // ~70 ms firmer pulse — value-changed confirms
    HAPTIC_DOUBLE_TAP,    // two quick pulses — alert default
    HAPTIC_LONG_BUZZ,     // 500 ms strong continuous — error / loud alert
    HAPTIC_PATTERN_COUNT,
};

class VibeService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Fire-and-forget pattern playback. Preempts any in-flight pattern.
    // Gated by SKEY_ALERT_VIBRATION.
    void play(HapticPatternId pattern);

private:
    EventBus*      _bus            = nullptr;
    HapticPatternId _current        = HAPTIC_OFF;
    const void*    _steps          = nullptr;  // const Step* erased to keep header light
    uint8_t        _step_index     = 0;
    uint32_t       _step_start_ms  = 0;
};

extern VibeService g_vibe;
