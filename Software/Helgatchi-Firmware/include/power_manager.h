#pragma once
#include "event_bus.h"

// ---------------------------------------------------------------------------
// Power states — carried in EV_POWER_STATE_CHANGED.data.power.state
// ---------------------------------------------------------------------------
enum PowerState : uint8_t {
    POWER_AWAKE    = 0,
    POWER_SLEEPING = 1,
};

// ---------------------------------------------------------------------------
// Battery pct sentinels (values above 100 encode special states)
// ---------------------------------------------------------------------------
static constexpr uint8_t BATT_PCT_CHARGING = 200;
static constexpr uint8_t BATT_PCT_CHARGED  = 201;
static constexpr uint8_t BATT_PCT_MISSING  = 202;

// ---------------------------------------------------------------------------
// VSENSE-to-percentage curve.
//
// Circuit: R2(100k) VBATT→node, R3(100k) node→GND  (R4 USB rail removed)
//   VSENSE = VBATT / 2
//
// LiPo discharge is non-linear — voltage stays high through most of the
// useful capacity, then drops sharply near depletion. A naive linear map
// across [3.0, 4.2]V both undersells fresh-charged packs (they never quite
// hit 4.20V at rest under load) and overstates remaining runtime in the
// middle of the curve.
//
// We use a piecewise-linear LUT instead, with deliberately conservative
// anchors:
//   100% at 4.15V  — settled-fresh resting voltage; avoids "less than full
//                     bar" when the icon is checked seconds after unplug.
//     0% at 3.40V  — well above the 3.00V protection-cutoff floor; leaves
//                     headroom for radio/LED load sag and protects cell life.
// VBATT readings outside [3.40, 4.15]V are clamped to 0/100.
//
// Points below approximate a typical 1S LiPo discharge curve, expressed in
// vsense-mV (= VBATT / 2). Add/remove points to tune; pmBattPctFromVsenseMv
// linearly interpolates between consecutive entries (must be sorted high→low
// on vsense_mv).
//
// USB/charging state is detected via (bool)Serial (USB CDC presence) and
// HAL::usbAttached() — independent of this curve.
// ---------------------------------------------------------------------------
struct BattCurvePoint { uint16_t vsense_mv; uint8_t pct; };
static constexpr BattCurvePoint PM_BATT_CURVE[] = {
    {2075, 100},  // 4.15 V
    {2025,  90},  // 4.05 V
    {1975,  75},  // 3.95 V
    {1925,  55},  // 3.85 V
    {1900,  40},  // 3.80 V
    {1875,  25},  // 3.75 V
    {1825,  10},  // 3.65 V
    {1700,   0},  // 3.40 V
};
static constexpr unsigned PM_BATT_CURVE_N    = sizeof(PM_BATT_CURVE) / sizeof(PM_BATT_CURVE[0]);
static constexpr uint16_t PM_VSENSE_FULL_MV  = PM_BATT_CURVE[0].vsense_mv;
static constexpr uint16_t PM_VSENSE_DEAD_MV  = PM_BATT_CURVE[PM_BATT_CURVE_N - 1].vsense_mv;

inline uint8_t pmBattPctFromVsenseMv(uint16_t vsense_mv) {
    if (vsense_mv >= PM_VSENSE_FULL_MV) return 100;
    if (vsense_mv <= PM_VSENSE_DEAD_MV) return 0;
    for (unsigned i = 0; i + 1 < PM_BATT_CURVE_N; ++i) {
        const BattCurvePoint& hi = PM_BATT_CURVE[i];
        const BattCurvePoint& lo = PM_BATT_CURVE[i + 1];
        if (vsense_mv <= hi.vsense_mv && vsense_mv >= lo.vsense_mv) {
            int32_t span_mv  = (int32_t)hi.vsense_mv - lo.vsense_mv;
            int32_t span_pct = (int32_t)hi.pct      - lo.pct;
            int32_t off_mv   = (int32_t)vsense_mv   - lo.vsense_mv;
            return (uint8_t)(lo.pct + (off_mv * span_pct) / span_mv);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------

class PowerManager : public IEventHandler {
public:
    // Call FIRST in setup(), before any other init. If the device is being
    // woken from shipping-mode deep sleep, this verifies the user is holding
    // CENTER (both PIN_BTN_1 and PIN_BTN_2 LOW) for SHIPPING_WAKE_HOLD_MS.
    //   • Held for full duration → flag cleared, function returns, boot continues
    //   • Released early or wrong button → re-enters shipping sleep, never returns
    // Cold boot or non-shipping wake → returns immediately (no-op).
    static void checkShippingWakeOrResleep();

    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Turn the display off without entering deep sleep. Cleared by a button
    // press or an alert raised with SKEY_ALERT_WAKE_SCREEN enabled. Serial
    // input does not clear it.
    void sleepScreen();

    // Deep-sleep if conditions permit, otherwise just turn the screen off.
    // Wraps the inhibit check so callers don't have to duplicate it.
    void requestSleepOrScreenOff();

    // Last EV_BATTERY_UPDATED values posted to the bus (for diagnostics).
    // pct may be a BATT_PCT_* sentinel; mv is always the literal vbatt mv last
    // sampled. 0xFF in pct = no sample taken yet.
    uint16_t lastBatteryMv()  const { return _last_batt_mv;  }
    uint8_t  lastBatteryPct() const { return _last_batt_pct; }

private:
    enum class DisplayState : uint8_t { OFF, ON, DIM };

    void _syncSettings();
    void _sampleBattery();
    void _enterSleep();
    void _enterShippingSleep();
    void _postCountdown(uint32_t now);
    uint32_t _calcRemainingS(uint32_t now) const;
    bool _isInhibited();           // non-const: maintains hysteresis state
    void _setDisplay(DisplayState s);

    EventBus* _bus = nullptr;
    DisplayState _disp_state = DisplayState::OFF;
    uint32_t _last_inhibit_seen_ms = 0;  // hysteresis timestamp for _isInhibited

    // Timing
    uint32_t _wake_ms          = 0;
    uint32_t _last_activity_ms = 0;
    uint32_t _last_batt_ms     = 0;
    uint32_t _last_tick_ms     = 0;
    uint32_t _stop_ms          = 0;   // millis() when last CMD_SCAN_STOP was posted

    // Cached settings
    uint16_t _scan_duration_s          = 5;
    uint16_t _sleep_duration_s         = 30;
    uint16_t _interactive_timeout_s    = 30;
    bool     _sleep_w_serial           = false;  // SKEY_DEBUG_SLEEP_WITH_SERIAL — true = allow sleep with serial open
    bool     _sleep_while_usb          = false;  // SKEY_SLEEP_WHILE_USB         — true = allow sleep with USB attached

    // State
    bool _user_active          = false;  // true once EV_UI_ACTIVITY received this cycle
    bool _scan_stop_posted     = false;
    bool _is_charging          = false;  // true when USB charging detected
    bool _screen_off_override  = false;  // sleepScreen() set; cleared by buttons or wake-screen alerts

    // Battery EMA (smooths single-sample noise / transient load sag).
    // Only applied while discharging — sentinel values pass through unsmoothed,
    // and the EMA is reset on the charging→discharging transition so the first
    // post-unplug sample isn't biased toward the inflated charging voltage.
    bool    _have_smoothed_pct = false;
    uint8_t _smoothed_pct      = 0;

    // Last posted battery values (mirrored for the `battery` serial command).
    uint16_t _last_batt_mv  = 0;
    uint8_t  _last_batt_pct = 0xFF;
};

extern PowerManager g_power;
