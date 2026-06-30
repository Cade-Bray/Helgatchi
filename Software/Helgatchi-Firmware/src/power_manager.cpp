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
// Shipping-mode wake handshake
//
// When _enterShippingSleep() is called we set this RTC-persistent flag and
// deep-sleep with EXT1 (GPIO6) as the only wake source. On the next boot,
// checkShippingWakeOrResleep() reads the flag — if set, it polls for the
// user to hold CENTER for SHIPPING_WAKE_HOLD_MS. Holding for the full
// duration clears the flag and continues boot; releasing early re-enters
// shipping sleep without returning.
//
// RTC memory is preserved across deep sleep but lost on full power loss, so
// a device whose battery is removed during shipping sleep cold-boots normally
// on the next power-up — which is the intended behavior (treat as fresh).
// ---------------------------------------------------------------------------

RTC_DATA_ATTR static bool _shipping_pending = false;

static constexpr uint32_t SHIPPING_WAKE_HOLD_MS = 2000;
static constexpr uint32_t SHIPPING_POLL_MS      =   10;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void PowerManager::checkShippingWakeOrResleep() {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;
    if (!_shipping_pending) return;

    // Woke from shipping sleep on a button press. Demand a long CENTER hold
    // (both rails LOW) before continuing — anything else (released early,
    // wrong button, spurious wake) re-enters shipping sleep.
    pinMode(PIN_BTN_1, INPUT_PULLUP);
    pinMode(PIN_BTN_2, INPUT_PULLUP);
    delayMicroseconds(200);  // let pullups settle

    Serial.println("[shipping] woke — checking long-press of center...");

    uint32_t held = 0;
    while (held < SHIPPING_WAKE_HOLD_MS) {
        if (digitalRead(PIN_BTN_1) != LOW || digitalRead(PIN_BTN_2) != LOW) {
            Serial.println("[shipping] released early — re-entering shipping sleep");
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
            esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_1, ESP_EXT1_WAKEUP_ANY_LOW);
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
            esp_deep_sleep_start();
            // Does not return.
        }
        delay(SHIPPING_POLL_MS);
        held += SHIPPING_POLL_MS;
    }

    Serial.println("[shipping] long-press confirmed — exiting shipping mode");
    _shipping_pending = false;
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
        _scan_stop_posted = false;
        _wake_ms          = now;
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

    if (_scan_duration_s       == 0) _scan_duration_s       = 5;
    if (_sleep_duration_s      == 0) _sleep_duration_s      = 30;
    if (_interactive_timeout_s == 0) _interactive_timeout_s = 30;
}

void PowerManager::_sampleBattery() {
    uint16_t vsense  = g_hal.readVsenseMv();
    uint16_t batt_mv = vsense * 2;

    uint8_t raw_pct = pmBattPctFromVsenseMv(vsense);

    bool was_charging = _is_charging;
    _is_charging      = g_hal.usbAttached();

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
    //   1. Timer — scheduled scan cycle.
    //   2. GPIO6 (PIN_BTN_1) LOW — any button press wakes the device.
    //      GPIO43 (PIN_BTN_2) is not RTC-capable on ESP32-S3; GPIO6 is pulled
    //      low by either button in the diode matrix, so it covers all buttons.
    esp_sleep_enable_timer_wakeup((uint64_t)_sleep_duration_s * 1000000ULL);

    // Wake on GPIO6 (PIN_BTN_1) going LOW — covers all buttons via diode matrix.
    // ext1 wakeup is the deep-sleep GPIO path available in this ESP-IDF version.
    // ALL_LOW with a single pin = wake when that pin is LOW (button pressed).
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
