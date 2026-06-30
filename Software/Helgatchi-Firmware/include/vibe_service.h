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
// SKEY_ALERT_VIBRATION gates only the bus path. Button-press haptics and any
// direct play() call always fire.
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

// ---------------------------------------------------------------------------
// Name registry — string identifiers for each HapticPatternId. Used by the
// serial console for `vibe <name>` and by RulesService at rule load time to
// resolve `vibe=double_tap` style criteria.
// ---------------------------------------------------------------------------

// Returns the registered name for `id` or "?" if out of range.
const char* vibePatternName(HapticPatternId id);

// Case-insensitive name -> id. Returns HAPTIC_PATTERN_COUNT (sentinel) if
// no pattern carries that name. Callers (RulesService) substitute their
// own default when the lookup misses.
HapticPatternId vibePatternByName(const char* name);

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
