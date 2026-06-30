#include "hal.h"
#include "settings_service.h"
#include <Arduino.h>
#include "soc/usb_serial_jtag_struct.h"
#include <driver/gpio.h>

HAL g_hal;


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void HAL::begin(EventBus& bus) {
    _bus = &bus;

    // Release any GPIO pad holds left over from a prior deep-sleep cycle (see
    // prepareForSleep). Without this, the held pins would stay locked LOW —
    // LEDC PWM would silently fail to drive the backlight, and FastLED would
    // be unable to push data to the LED chain.
    gpio_hold_dis((gpio_num_t)PIN_SPI_BL);
    gpio_hold_dis((gpio_num_t)PIN_LED_DATA);
    gpio_deep_sleep_hold_dis();

    // Drive the backlight + LED-data pins to a known-LOW state IMMEDIATELY,
    // before _initDisplay / _initLEDs run. Reasons:
    //   • PIN_SPI_BL (GPIO3) is a strapping pin and has an internal pull-up
    //     enabled by ROM. After gpio_hold_dis releases the latch, the pad
    //     enters HI-Z and the pull-up drives it HIGH — the backlight visibly
    //     flashes on for the duration of _tft.init() (which has its own
    //     ~150ms delays) until ledcAttachPin re-grabs the pin with duty 0.
    //   • PIN_LED_DATA (GPIO2) has no peripheral driver attached on wake.
    //     A floating SK6805 data line + radio noise during boot can be
    //     interpreted as data — typically as 0xFFFFFF (white flash).
    pinMode(PIN_SPI_BL, OUTPUT);
    digitalWrite(PIN_SPI_BL, LOW);
    pinMode(PIN_LED_DATA, OUTPUT);
    digitalWrite(PIN_LED_DATA, LOW);

    pinMode(PIN_BTN_1,   INPUT_PULLUP);
    pinMode(PIN_BTN_2,   INPUT_PULLUP);
    pinMode(PIN_VSENSE,  INPUT);

    // Vibration motor on its own LEDC channel. 20 kHz keeps PWM out of the
    // audible range so the motor doesn't whine; 8-bit gives 0–255 intensity.
    ledcSetup(HAL_VIBE_LEDC_CH, 20000, 8);
    ledcAttachPin(PIN_VIBRATE, HAL_VIBE_LEDC_CH);
    ledcWrite(HAL_VIBE_LEDC_CH, 0);

    _initDisplay();
    _initLEDs();
    // NOTE: brightness intentionally NOT applied here. PowerManager decides
    // whether to call wakeDisplay() based on the wake cause — TIMER (silent
    // scan) wakes leave the screen off entirely. _applyBrightnessSettings()
    // still runs on EV_SETTINGS_CHANGED so live adjustments work.

    bus.subscribe(EV_SETTINGS_CHANGED, this);
    // EV_ALERT_RAISED is handled by VibeService now — it owns the haptic
    // pattern catalog and decides what to play.
}

void HAL::tick() {
    _pollButtons();

    // SOF frames arrive every 1 ms while connected to a USB host.
    // Check every 100 ms — if the counter moved at all in the window we're
    // attached. The previous 2 ms window was too tight: hosts occasionally
    // pause SOFs (heavy traffic, brief suspend) for more than 2 ms, which
    // would falsely flip _usb_attached to false and cascade through to
    // _is_charging, breaking sleep inhibition.
    uint32_t now = millis();
    if (now - _last_sof_check_ms >= 100) {
        uint32_t sof   = USB_SERIAL_JTAG.fram_num.sof_frame_index;
        _usb_attached  = (sof != _last_sof);
        _last_sof      = sof;
        _last_sof_check_ms = now;
    }

}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void HAL::_initDisplay() {
    _tft.init();
    _tft.setRotation(1);  // landscape: logical 280 x 240
    _tft.fillScreen(TFT_BLACK);

    // Backlight via LEDC PWM (arduino-esp32 v2 channel-based API).
    //
    // Leave duty at 0 here — PowerManager decides whether to wake the screen
    // based on the wake cause (TIMER = silent scan, leave off; everything
    // else = wakeDisplay()). Explicit ledcWrite(0) so we don't depend on
    // arduino-esp32's LEDC default behaviour staying 0 across releases.
    ledcSetup(HAL_BL_LEDC_CH, 5000, 8);
    ledcAttachPin(PIN_SPI_BL, HAL_BL_LEDC_CH);
    ledcWrite(HAL_BL_LEDC_CH, 0);
}

void HAL::setBacklight(uint8_t val) {
    ledcWrite(HAL_BL_LEDC_CH, val);
}

void HAL::wakeDisplay() {
    uint8_t bl = g_settings.get(SKEY_SCREEN_BRIGHTNESS);
    if (bl >= SCREEN_BRIGHTNESS_COUNT) bl = SCREEN_BRIGHTNESS_HIGH;
    ledcWrite(HAL_BL_LEDC_CH, HAL_BL_LEVELS[bl]);
}

void HAL::dimDisplay() {
    ledcWrite(HAL_BL_LEDC_CH, HAL_BL_LEVELS[SCREEN_BRIGHTNESS_MIN]);
}

void HAL::sleepDisplay() {
    ledcWrite(HAL_BL_LEDC_CH, 0);
}

void HAL::prepareForSleep() {
    // Stop the vibration motor unconditionally — if a haptic pattern was
    // mid-play when sleep was triggered, we don't want to wake on a buzz.
    stopVibrate();

    // Put the LCD controller into its own sleep mode. The panel stops driving
    // pixels and drops controller current to ~50 µA. Cheap and reduces the
    // chance of a faint visible image from any residual VDD on the panel.
    _tft.sleep();

    // Backlight: detach LEDC, drive LOW, lock through sleep. ledcWrite(0)
    // alone isn't enough — when esp_deep_sleep_start() powers LEDC down the
    // pin loses its driver, drifts HIGH, and the backlight reverts on.
    ledcDetachPin(PIN_SPI_BL);
    pinMode(PIN_SPI_BL, OUTPUT);
    digitalWrite(PIN_SPI_BL, LOW);

    // LED data: take the pin away from FastLED's RMT routing, drive LOW, and
    // lock through sleep. Without this lock, the SK6805 data line floats for
    // the entire sleep window (tens of seconds) and EMI/noise drifts the
    // controllers' shift register into a bad state — visible as a single
    // white flash on the next silent (TIMER) wake when FastLED takes over.
    pinMode(PIN_LED_DATA, OUTPUT);
    digitalWrite(PIN_LED_DATA, LOW);

    // Lock both pads LOW for the duration of deep sleep. Released in begin().
    gpio_hold_en((gpio_num_t)PIN_SPI_BL);
    gpio_hold_en((gpio_num_t)PIN_LED_DATA);
    gpio_deep_sleep_hold_en();
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------

void HAL::_initLEDs() {
    // SK6812 chipset for SK6805 LEDs (R2.8+). SK6805 is the small EC15/EC20
    // package variant of the SK6812 family — same protocol/timing. Using the
    // WS2812B chipset works on tolerant SK6805 chips but fails intermittently
    // on stricter parts because the WS2812 T0H (250 ns) is below the SK6805
    // minimum (300 ns) and the chip latches ambiguous bits.
    FastLED.addLeds<SK6812, PIN_LED_DATA, GRB>(_leds, HAL_NUM_LEDS);
    FastLED.setBrightness(HAL_LED_LEVELS[1]);  // default: MEDIUM
    FastLED.clear(true);
}

void HAL::setLED(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx >= HAL_NUM_LEDS) return;
    _leds[idx] = CRGB(r, g, b);
    FastLED.show();
}

void HAL::setAllLEDs(uint8_t r, uint8_t g, uint8_t b) {
    fill_solid(_leds, HAL_NUM_LEDS, CRGB(r, g, b));
    FastLED.show();
}

void HAL::clearLEDs() {
    FastLED.clear(true);
}

void HAL::writeLEDFrame(const CRGB frame[HAL_NUM_LEDS]) {
    for (uint8_t i = 0; i < HAL_NUM_LEDS; i++) _leds[i] = frame[i];
    FastLED.show();
}

// ---------------------------------------------------------------------------
// Vibration
// ---------------------------------------------------------------------------

void HAL::setVibrate(uint8_t intensity) {
    ledcWrite(HAL_VIBE_LEDC_CH, intensity);
}

void HAL::stopVibrate() {
    ledcWrite(HAL_VIBE_LEDC_CH, 0);
}

// ---------------------------------------------------------------------------
// Power sensing
// ---------------------------------------------------------------------------


uint16_t HAL::readVsenseMv() {
    analogSetAttenuation(ADC_11db);
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VSENSE);
    return (uint16_t)(sum / 8);
}

// ---------------------------------------------------------------------------
// Button polling (2×2 diode matrix: GPIO6 + GPIO43 → Left / Right / Center)
//
// Matrix decode:
//   GPIO6 LOW  only  → Left
//   GPIO43 LOW only  → Right
//   Both LOW         → Center  (diodes prevent cross-drive)
// ---------------------------------------------------------------------------

void HAL::_pollButtons() {
    uint32_t now = millis();

    bool raw_a = !digitalRead(PIN_BTN_1);
    bool raw_b = !digitalRead(PIN_BTN_2);

    bool raw[3] = {
        raw_a && !raw_b,   // Left
        !raw_a && raw_b,   // Right
        raw_a &&  raw_b,   // Center
    };

    for (int i = 0; i < 3; i++) {
        if (raw[i] != _btn[i].raw_prev) {
            _btn[i].stable_since = now;
            _btn[i].raw_prev     = raw[i];
        }

        if ((now - _btn[i].stable_since) < HAL_DEBOUNCE_MS) continue;

        bool was       = _btn[i].state;
        _btn[i].state  = raw[i];

        if (!was && _btn[i].state) {
            // Falling edge (press)
            _bus->post(EV_UI_ACTIVITY);
            if      (i == 0) _bus->post(EV_BTN_LEFT);
            else if (i == 1) _bus->post(EV_BTN_RIGHT);
            else {
                _center_down_at    = now;
                _center_long_fired = false;
                _center_hold_fired = false;
            }
        } else if (was && !_btn[i].state) {
            // Rising edge (release)
            if (i == 2 && !_center_long_fired) {
                _bus->post(EV_BTN_CENTER_SHORT);
            }
        }
    }

    // Long-press detection for center button. Two thresholds:
    //   HAL_LONG_PRESS_MS (~600 ms)  → EV_BTN_CENTER_LONG  (back / pop screen)
    //   HAL_HOLD_MS       (~2000 ms) → EV_BTN_CENTER_HOLD  (sleep on main menu)
    if (_btn[2].state) {
        const uint32_t held = now - _center_down_at;
        if (!_center_long_fired && held >= HAL_LONG_PRESS_MS) {
            _center_long_fired = true;
            _bus->post(EV_BTN_CENTER_LONG);
        }
        if (!_center_hold_fired && held >= HAL_HOLD_MS) {
            _center_hold_fired = true;
            _bus->post(EV_BTN_CENTER_HOLD);
        }
    }
}

// ---------------------------------------------------------------------------
// Settings-driven brightness
// ---------------------------------------------------------------------------

void HAL::_applyBrightnessSettings() {
    uint8_t bl  = g_settings.get(SKEY_SCREEN_BRIGHTNESS);
    uint8_t led = g_settings.get(SKEY_LED_BRIGHTNESS);

    if (bl  >= SCREEN_BRIGHTNESS_COUNT) bl  = SCREEN_BRIGHTNESS_HIGH;
    if (led >= LED_BRIGHTNESS_COUNT)    led = LED_BRIGHTNESS_MEDIUM;

    ledcWrite(HAL_BL_LEDC_CH, HAL_BL_LEVELS[bl]);
    FastLED.setBrightness(HAL_LED_LEVELS[led]);
    FastLED.show();
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void HAL::onEvent(const Event& e) {
    switch (e.id) {
        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_UI) {
                _applyBrightnessSettings();
            }
            break;

        default:
            break;
    }
}
