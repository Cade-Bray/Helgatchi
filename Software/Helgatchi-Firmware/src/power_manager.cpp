#include "power_manager.h"
#include "settings_service.h"
#include "hal.h"
#include "ui_controller.h"
#include "log_service.h"
#include "scan_service.h"
#include "scan_engine.h"
#include "rules_service.h"
#include "event_payload.h"
#include <Arduino.h>
#include <esp_sleep.h>

PowerManager g_power;

static constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS = 30000;
static constexpr uint32_t TICK_1S_INTERVAL_MS        =  1000;

// ---------------------------------------------------------------------------
// Deep-sleep wake handshake
//
// The button matrix is two RTC-readable-but-only-one-RTC-wake-capable pins:
//   GPIO6  (PIN_BTN_1) — LOW when LEFT or CENTER pressed. RTC-capable → EXT1.
//   GPIO43 (PIN_BTN_2) — LOW when RIGHT or CENTER pressed. NOT RTC-capable.
// So the only viable deep-sleep wake trigger is GPIO6 LOW (left or center).
//
// Deep sleep GPIO wake can't natively require a hold or a specific button, so
// we do it in software at boot: EXT1 wakes the chip on GPIO6 LOW, then
// checkWakeHoldOrResleep() (called FIRST in setup) demands the user hold
// CENTER — both rails LOW — for WAKE_HOLD_MS. A left-only press, a right-only
// press (can't even wake), or a brief accidental bump falls short and the
// device re-enters the exact sleep it came from without spinning anything up.
// This makes pocket carry reliable: nothing but a deliberate center hold wakes.
//
// Two sleep flavors re-armed on a failed hold:
//   • shipping (_shipping_pending) — EXT1 only, never auto-wakes.
//   • regular  — EXT1 + the scan-cycle timer (_deep_sleep_timer_us), so the
//                autonomous scan cadence resumes.
//
// TIMER wakes (silent scan cycle) and cold boot / soft reset skip the check
// entirely — no button was involved.
//
// RTC memory survives deep sleep but is cleared on full power loss, so a
// battery pull during sleep cold-boots normally on next power-up.
// ---------------------------------------------------------------------------

RTC_DATA_ATTR static bool     _shipping_pending    = false;
RTC_DATA_ATTR static uint64_t _deep_sleep_timer_us = 0;   // scan-cycle timer, re-armed on failed hold

// Center-hold duration required to wake. Shipping demands a longer, more
// deliberate hold than a normal sleep wake since it's the "unbox / take out
// of storage" gesture and should never trigger by accident.
static constexpr uint32_t SLEEP_WAKE_HOLD_MS    = 1000;
static constexpr uint32_t SHIPPING_WAKE_HOLD_MS = 2200;
static constexpr uint32_t WAKE_POLL_MS          =   10;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void PowerManager::checkWakeHoldOrResleep() {
    // Only button (EXT1) wakes need the hold gate. Timer wake (scan cycle),
    // cold boot, and soft reset proceed straight to normal boot.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;

    // A button pulled GPIO6 low. Demand CENTER (both rails LOW) held for the
    // full duration before continuing.
    pinMode(PIN_BTN_1, INPUT_PULLUP);
    pinMode(PIN_BTN_2, INPUT_PULLUP);
    delayMicroseconds(200);  // let pullups settle

    const uint32_t hold_target = _shipping_pending ? SHIPPING_WAKE_HOLD_MS
                                                    : SLEEP_WAKE_HOLD_MS;
    bool held_ok = true;
    uint32_t held = 0;
    while (held < hold_target) {
        if (digitalRead(PIN_BTN_1) != LOW || digitalRead(PIN_BTN_2) != LOW) {
            held_ok = false;  // not center, or released early
            break;
        }
        delay(WAKE_POLL_MS);
        held += WAKE_POLL_MS;
    }

    if (held_ok) {
        // Deliberate center hold — wake for real. Shipping exits shipping mode.
        _shipping_pending = false;
        return;
    }

    // Hold not satisfied. Wait for full release so EXT1 (wake-on-LOW) doesn't
    // immediately re-fire on a still-held left/right button, then re-enter the
    // sleep we came from.
    while (digitalRead(PIN_BTN_1) == LOW || digitalRead(PIN_BTN_2) == LOW) {
        delay(WAKE_POLL_MS);
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_1, ESP_EXT1_WAKEUP_ANY_LOW);
    if (!_shipping_pending && _deep_sleep_timer_us > 0) {
        esp_sleep_enable_timer_wakeup(_deep_sleep_timer_us);  // resume scan cadence
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_deep_sleep_start();
    // Does not return.
}

void PowerManager::begin(EventBus& bus) {
    _bus = &bus;
    _syncSettings();

    bus.subscribe(EV_SETTINGS_CHANGED,      this);
    bus.subscribe(EV_UI_ACTIVITY,           this);
    bus.subscribe(EV_ALERT_RAISED,          this);
    bus.subscribe(CMD_POWER_SLEEP,          this);
    bus.subscribe(CMD_POWER_SHIPPING_SLEEP, this);
    bus.subscribe(CMD_POWER_SHIPPING_RESET, this);
    bus.subscribe(EV_BTN_LEFT,              this);
    bus.subscribe(EV_BTN_RIGHT,             this);
    bus.subscribe(EV_BTN_CENTER_SHORT,      this);
    bus.subscribe(EV_BTN_CENTER_LONG,       this);

    _wake_ms          = millis();
    _last_batt_ms     = _wake_ms;
    _last_tick_ms     = _wake_ms;
    _user_active      = false;
    _scan_stop_posted = false;
    _disp_state       = DisplayState::OFF;

    // Decide what the display does based on what woke us:
    //   • TIMER     → autonomous scan cycle, screen stays off
    //   • EXT1      → user pressed a button, light up + interactive timeout
    //   • UNDEFINED → cold boot / soft reset, treat like a button wake so the
    //                 splash + status screens are visible after flashing/reboot
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        // Silent scan wake — leave display off, run the scan-then-sleep cycle.
        _setDisplay(DisplayState::OFF);
    } else {
        _user_active      = true;
        _last_activity_ms = _wake_ms;
        _setDisplay(DisplayState::ON);
    }

    EventPayload p{};
    p.power.state = POWER_AWAKE;
    _bus->post(EV_POWER_STATE_CHANGED, p);
    _bus->post(CMD_SCAN_START);

    _sampleBattery();
}

// ---------------------------------------------------------------------------
// Main loop tick
// ---------------------------------------------------------------------------

void PowerManager::tick() {
    uint32_t now = millis();

    // 1-second heartbeat.
    if (now - _last_tick_ms >= TICK_1S_INTERVAL_MS) {
        _last_tick_ms += TICK_1S_INTERVAL_MS;
        _bus->post(EV_TICK_1S);
        _postCountdown(now);

        // Dim countdown: when ≤5 s remain in the interactive timeout, drop the
        // screen to MIN brightness as a "going to sleep" warning. Skipped when
        // sleep is inhibited (USB / serial) since the device won't actually
        // sleep, so dimming would be misleading. Also skipped when
        // _screen_off_override is set so the screen stays dark.
        if (_user_active && !_screen_off_override) {
            if (_isInhibited()) {
                _setDisplay(DisplayState::ON);
            } else {
                uint32_t remaining = _calcRemainingS(now);
                if (remaining > 0 && remaining <= 5) _setDisplay(DisplayState::DIM);
                else if (remaining > 5)              _setDisplay(DisplayState::ON);
            }
        }
    }

    // Periodic battery sample.
    if (now - _last_batt_ms >= BATTERY_SAMPLE_INTERVAL_MS) {
        _last_batt_ms = now;
        _sampleBattery();
    }

    // React immediately to USB attach/detach rather than waiting 30 s.
    if (g_hal.usbAttached() != _is_charging) {
        _last_batt_ms = now;  // reset interval to avoid double-sample
        _sampleBattery();
    }

    // End of scan window — fire CMD_SCAN_STOP once.
    if (!_scan_stop_posted &&
        (now - _wake_ms) >= (uint32_t)_scan_duration_s * 1000) {
        _scan_stop_posted = true;
        _stop_ms          = now;
        _bus->post(CMD_SCAN_STOP);
    }

    // Scan window still open — nothing more to decide this tick.
    if (!_scan_stop_posted) return;

    // After scan stop: wait for the ScanEngine queue + ScanService ring to
    // drain so every advertisement caught this window has been seen by the
    // rules engine before we sleep / start the next cycle.
    const uint32_t ring_pending  = g_scan.writePos() - g_rules.ringReadPos();
    const size_t   queue_pending = g_scan_engine.queueDepth();
    if (ring_pending != 0 || queue_pending != 0) return;

    // Drain complete — every advertisement caught this window is now in the
    // seen-devices map (and seen by the rules engine). Fire EV_SCAN_COMPLETE
    // once so the device list can refresh against a fully-populated map.
    if (!_scan_complete_posted) {
        _scan_complete_posted = true;
        _bus->post(EV_SCAN_COMPLETE);
    }

    // Drain complete. Decide what's next:
    //   - autonomous (no user activity, no alert, not on USB/serial) → deep sleep
    //   - everything else (interactive, inhibited, or alert fired) → stay
    //     awake and start the next scan cycle after _sleep_duration_s elapses
    const bool stays_awake = _user_active || _isInhibited();

    if (!stays_awake) {
        _enterSleep();
        return;
    }

    // Interactive timeout — only fires when on battery (inhibit freezes the
    // clock). Without an alert or further button press, the device eventually
    // sleeps on its own.
    if (_isInhibited()) {
        _last_activity_ms = now;
    } else if ((now - _last_activity_ms) >= (uint32_t)_interactive_timeout_s * 1000) {
        _enterSleep();
        return;
    }

    // Wait _sleep_duration_s between scan windows, then open the next one
    // in place (no deep sleep — we're staying awake on purpose).
    if ((now - _stop_ms) >= (uint32_t)_sleep_duration_s * 1000) {
        _scan_stop_posted     = false;
        _scan_complete_posted = false;
        _wake_ms              = now;
        _bus->post(CMD_SCAN_START);
    }
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void PowerManager::onEvent(const Event& e) {
    switch (e.id) {
        case EV_UI_ACTIVITY:
            _user_active      = true;
            _last_activity_ms = millis();
            if (!_screen_off_override) {
                _setDisplay(DisplayState::ON);
            }
            break;

        case EV_BTN_LEFT:
        case EV_BTN_RIGHT:
        case EV_BTN_CENTER_SHORT:
        case EV_BTN_CENTER_LONG:
            _screen_off_override = false;
            _user_active         = true;
            _last_activity_ms    = millis();
            _setDisplay(DisplayState::ON);
            break;

        case EV_ALERT_RAISED:
            // Alerts always keep the device awake — without this, an alert
            // fired during the post-scan drain would sleep immediately and
            // the haptic/LED/screen reactions would never get to play. The
            // interactive timeout still applies, so a quiet device returns
            // to sleep on its own. Wake-screen behavior remains gated by
            // SKEY_ALERT_WAKE_SCREEN.
            _user_active      = true;
            _last_activity_ms = millis();
            if (g_settings.getBool(SKEY_ALERT_WAKE_SCREEN)) {
                _screen_off_override = false;
                _setDisplay(DisplayState::ON);
            }
            break;

        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_POWER) {
                _syncSettings();
            }
            break;

        case CMD_POWER_SLEEP:
            _enterSleep();
            break;

        case CMD_POWER_SHIPPING_SLEEP:
            _enterShippingSleep();
            break;

        case CMD_POWER_SHIPPING_RESET:
            // Device already rebooted from shipping sleep — nothing to do.
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void PowerManager::_syncSettings() {
    _scan_duration_s         = g_settings.getU16(SKEY_SCAN_DURATION_S);
    _sleep_duration_s        = g_settings.getU16(SKEY_SLEEP_DURATION_S);
    _interactive_timeout_s   = g_settings.getU16(SKEY_INTERACTIVE_TIMEOUT_S);
    _sleep_w_serial          = g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL);
    _sleep_while_usb         = g_settings.getBool(SKEY_SLEEP_WHILE_USB);
    _vsense_5v_divider       = g_settings.getBool(SKEY_VSENSE_5V_DIVIDER);

    if (_scan_duration_s       == 0) _scan_duration_s       = 5;
    if (_sleep_duration_s      == 0) _sleep_duration_s      = 30;
    if (_interactive_timeout_s == 0) _interactive_timeout_s = 30;
}

void PowerManager::_sampleBattery() {
    uint16_t vsense  = g_hal.readVsenseMv();

    bool was_charging = _is_charging;
    _is_charging      = g_hal.usbAttached();

    // Two hardware variants share this ADC node:
    //   • R4 cut (default): R2/R3 form a plain 2:1 divider → VSENSE = VBATT/2,
    //     so VBATT = 2·VSENSE.
    //   • R4 populated: R2/R3/R4 (all 100k) meet at VSENSE with R4 tied to the
    //     +5V charger rail. Node equation VSENSE·(1/R2+1/R3+1/R4)=VBATT/R2+V5/R4
    //     with equal resistors collapses to 3·VSENSE = VBATT + V5, i.e.
    //     VBATT = 3·VSENSE − V5. V5 is ~5 V while USB is attached and 0 V
    //     otherwise (VBUS-derived rail collapses when unplugged).
    uint16_t batt_mv;
    if (_vsense_5v_divider) {
        int32_t v5 = _is_charging ? PM_V5_RAIL_MV : 0;
        int32_t vb = 3 * (int32_t)vsense - v5;
        if (vb < 0) vb = 0;
        batt_mv = (uint16_t)vb;
    } else {
        batt_mv = vsense * 2;
    }

    // The curve LUT is expressed in VSENSE-mV at the classic VBATT/2 scale, so
    // feed it the equivalent half-VBATT regardless of which divider is fitted.
    uint8_t raw_pct = pmBattPctFromVsenseMv(batt_mv / 2);

    // Reset EMA when USB is unplugged: the first discharging reading should
    // not be averaged with stale charging-voltage samples.
    if (was_charging && !_is_charging) _have_smoothed_pct = false;

    uint8_t pct;
    if (_is_charging) {
        // Sentinels carry charging state; the actual mv is still in the event
        // and the UI re-derives a level glyph from it via pmBattPctFromVsenseMv.
        pct = (raw_pct >= 95) ? BATT_PCT_CHARGED : BATT_PCT_CHARGING;
    } else {
        if (_have_smoothed_pct) {
            pct = (uint8_t)(((uint32_t)_smoothed_pct * 7 + raw_pct + 4) / 8);
        } else {
            pct = raw_pct;
            _have_smoothed_pct = true;
        }
        _smoothed_pct = pct;
    }

    _last_batt_mv  = batt_mv;
    _last_batt_pct = pct;

    EventPayload p{};
    p.battery.mv  = batt_mv;
    p.battery.pct = pct;
    _bus->post(EV_BATTERY_UPDATED, p);
}

uint16_t PowerManager::secondsUntilNextScan() const {
    // Scanning off entirely — the cycle still runs but no radio starts.
    if ((g_settings.get(SKEY_SCAN_MODE) & 0x3u) == 0) return 0xFFFFu;
    // Scan window currently open (CMD_SCAN_STOP not yet posted this cycle).
    if (!_scan_stop_posted) return 0;
    // Between windows: next CMD_SCAN_START fires at _stop_ms + _sleep_duration_s.
    // (A deep-sleep autonomous cycle re-arms the same timer, so the value holds.)
    uint32_t elapsed = (millis() - _stop_ms) / 1000;
    return (elapsed < _sleep_duration_s)
           ? (uint16_t)(_sleep_duration_s - elapsed) : 0;
}

uint32_t PowerManager::_calcRemainingS(uint32_t now) const {
    if (_user_active) {
        uint32_t elapsed = (now - _last_activity_ms) / 1000;
        return (elapsed < _interactive_timeout_s)
               ? (_interactive_timeout_s - elapsed) : 0;
    }
    uint32_t elapsed = (now - _wake_ms) / 1000;
    return (elapsed < _scan_duration_s)
           ? (_scan_duration_s - elapsed) : 0;
}

void PowerManager::_postCountdown(uint32_t now) {
    EventPayload p{};
    // 0xFFFF is the sentinel for "won't sleep" (USB connected / serial inhibit).
    // UIController renders this as "Will not sleep" in the settings screen.
    p.sleep_count.seconds = _isInhibited()
                             ? 0xFFFFu
                             : (uint16_t)_calcRemainingS(now);
    _bus->post(EV_SLEEP_COUNTDOWN_UPDATED, p);
}

bool PowerManager::_isInhibited() {
    // Hysteresis: if either condition is currently true, latch the timestamp.
    // For a few seconds after that, keep returning true even if the raw checks
    // momentarily disagree. (USB hosts occasionally pause SOFs and CDC's
    // _connected flag can briefly drop during heavy traffic — without grace,
    // a single tick where both checks read false is enough to fire _enterSleep
    // even though the user is actively using the terminal.)
    static constexpr uint32_t INHIBIT_GRACE_MS = 2000;

    // Both clauses are positive-form "allow sleep" → inverted into inhibit:
    //   Serial open + user said don't-sleep-with-serial → inhibit
    //   USB attached + user said don't-sleep-on-USB     → inhibit
    bool raw = ((bool)Serial && !_sleep_w_serial)
            || (_is_charging && !_sleep_while_usb);

    if (raw) {
        _last_inhibit_seen_ms = millis();
        return true;
    }
    if (_last_inhibit_seen_ms != 0 &&
        (millis() - _last_inhibit_seen_ms) < INHIBIT_GRACE_MS) {
        return true;
    }
    return false;
}

void PowerManager::sleepScreen() {
    _screen_off_override = true;
    _setDisplay(DisplayState::OFF);
}

void PowerManager::requestSleepOrScreenOff() {
    // Confirmation haptic — direct HAL drive so it plays even when the next
    // call enters deep sleep (vibe_service.tick() won't run).
    g_hal.setVibrate(220);
    delay(70);
    g_hal.stopVibrate();
    if (_isInhibited()) sleepScreen();
    else                _enterSleep();
}

void PowerManager::_setDisplay(DisplayState s) {
    if (s == _disp_state) return;
    _disp_state = s;
    // OFF: skip both the backlight AND LVGL rendering. Saves ~70 % CPU
    // during silent TIMER-wake scan windows, leaving the cycles for scanner
    // / rules engine work. ON/DIM resume rendering immediately.
    switch (s) {
        case DisplayState::OFF:
            g_hal.sleepDisplay();
            g_ui.setRenderEnabled(false);
            break;
        case DisplayState::ON:
            g_hal.wakeDisplay();
            g_ui.setRenderEnabled(true);
            g_logger.applyPerfMonitor();   // re-sync overlay visibility on wake
            break;
        case DisplayState::DIM:
            g_hal.dimDisplay();
            g_ui.setRenderEnabled(true);
            g_logger.applyPerfMonitor();
            break;
    }
}

void PowerManager::_enterSleep() {
    // Wait for buttons to be released so EXT1 (wake-on-LOW) doesn't fire
    // immediately on the press that triggered this call. No-op when entered
    // from the serial `sleep` command (no button held).
    while (digitalRead(PIN_BTN_1) == LOW || digitalRead(PIN_BTN_2) == LOW) {
        delay(10);
    }

    // Pre-sleep cleanup.
    // Order matters: clearLEDs needs RMT to still own PIN_LED_DATA. Once
    // prepareForSleep runs pinMode(OUTPUT) on it, RMT is detached and
    // FastLED.show() can't push the all-off frame — the LEDs would hang at
    // their last state through deep sleep.
    g_hal.clearLEDs();
    g_hal.prepareForSleep();   // backlight off, LCD sleep, both pads held LOW
    _disp_state = DisplayState::OFF;

    EventPayload p{};
    p.power.state = POWER_SLEEPING;
    _bus->post(EV_POWER_STATE_CHANGED, p);
    _bus->dispatch();   // flush queue before losing power

    // Wake sources:
    //   1. Timer — scheduled scan cycle. Stashed in RTC so a failed wake-hold
    //      check (checkWakeHoldOrResleep) can re-arm the same cadence.
    //   2. GPIO6 (PIN_BTN_1) LOW — left or center press wakes the chip, but
    //      checkWakeHoldOrResleep then requires CENTER held to stay awake.
    //      GPIO43 (PIN_BTN_2) is not RTC-capable on ESP32-S3, so it can't be a
    //      wake source; it's only read after wake to confirm the center hold.
    _deep_sleep_timer_us = (uint64_t)_sleep_duration_s * 1000000ULL;
    esp_sleep_enable_timer_wakeup(_deep_sleep_timer_us);

    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_1, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_deep_sleep_start();
    // Does not return — device resets on wakeup and setup() runs again.
}

void PowerManager::_enterShippingSleep() {
    // Order matters: clearLEDs needs RMT to still own PIN_LED_DATA. Once
    // prepareForSleep runs pinMode(OUTPUT) on it, RMT is detached and
    // FastLED.show() can't push the all-off frame — the LEDs would hang at
    // their last state through deep sleep.
    g_hal.clearLEDs();
    g_hal.prepareForSleep();   // backlight off, LCD sleep, both pads held LOW
    _disp_state = DisplayState::OFF;

    // Mark us as shipping-pending so the next boot demands a long-press
    // before resuming normal operation.
    _shipping_pending = true;

    Serial.println("[power] shipping sleep — long-press CENTER to wake");

    // Reset tutorial flag so the tutorial shows when the device is taken out
    // of shipping mode (next boot after shipping-mode wake is treated like
    // a first-time power-on from the user's perspective).
    {
        EventPayload tp{};
        tp.settings_set.key   = SKEY_TUTORIAL_SHOWN;
        tp.settings_set.value = 0;
        _bus->post(CMD_SETTINGS_SET, tp);
    }

    EventPayload p{};
    p.power.state = POWER_SLEEPING;
    _bus->post(EV_POWER_STATE_CHANGED, p);
    _bus->dispatch();   // flush queue before losing power

    // Wait for buttons to be released, otherwise EXT1 fires immediately and
    // we wake right back up.
    while (digitalRead(PIN_BTN_1) == LOW || digitalRead(PIN_BTN_2) == LOW) {
        delay(10);
    }

    // Wake source: any button press (GPIO6 LOW). NO timer — shipping mode
    // never auto-wakes.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_1, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_deep_sleep_start();
    // Does not return — device resets on wake and setup() runs from scratch.
}
