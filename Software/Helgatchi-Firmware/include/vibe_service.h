#pragma once
#include "event_bus.h"
#include <stdint.h>
#include <esp_timer.h>

// ---------------------------------------------------------------------------
// Haptic pattern catalog
//
// Patterns are short sequences of (intensity, duration) steps that VibeService
// plays out via PWM on the vibration motor. Patterns are fire-and-forget — a
// subsequent play() preempts whatever was running.
//
// Step timing runs on a one-shot esp_timer, NOT the main loop: each step arms
// the timer for its duration and the callback advances to the next step (or
// drives the motor off at the terminating step). This decouples haptics from
// UI/render latency — a stalled loop can no longer stretch a buzz — and makes
// motor-off structurally guaranteed, since it's always the last scheduled
// step. play() / stop() / the timer callback are serialized by a mutex because
// the callback fires on the esp_timer task, so play() is safe from any task.
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
    void onEvent(const Event& e) override;

    // Fire-and-forget pattern playback. Preempts any in-flight pattern and
    // always fires (the SKEY_ALERT_VIBRATION gate is applied on the bus path
    // before calling play). Safe to call from any task.
    void play(HapticPatternId pattern);

    // Cancel any in-flight pattern and drive the motor off now.
    void stop();

private:
    static void _timerCb(void* arg);   // esp_timer callback trampoline → _onTimer()
    void _onTimer();                   // advance to the next step
    void _armCurrentLocked();          // drive current step + arm its timer, or
                                       // finish; caller must hold the vibe lock

    EventBus*          _bus         = nullptr;
    esp_timer_handle_t _timer       = nullptr;
    HapticPatternId    _current     = HAPTIC_OFF;
    const void*        _steps       = nullptr;  // const Step* erased to keep header light
    uint8_t            _step_index  = 0;
};

extern VibeService g_vibe;
