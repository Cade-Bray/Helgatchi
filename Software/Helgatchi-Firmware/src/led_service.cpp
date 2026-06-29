#include "led_service.h"
#include "hal.h"
#include "settings_service.h"
#include "power_manager.h"
#include "event_payload.h"
#include <Arduino.h>
#include <FastLED.h>

LedService g_leds;

static constexpr uint32_t FRAME_PERIOD_MS = 33;     // ~30 FPS render cap
static constexpr uint32_t ALERT_DEFAULT_MS = 3000;  // default alert duration when raised via the bus
static constexpr uint32_t ALERT_FADE_MS    = 500;   // fade-out length on alert end
static constexpr uint8_t  LOW_BATTERY_PCT  = 15;    // pulse below this threshold (and not charging)

// ---------------------------------------------------------------------------
// Pattern renderers — each fills a 6-element CRGB array based on now_ms.
//
// Conventions:
//   • Final brightness scaling is applied by FastLED's global setBrightness
//     (which HAL syncs from SKEY_LED_BRIGHTNESS), so each pattern caps its
//     internal brightness at modest values and the user setting scales down.
//   • No FastLED.show() in the pattern functions — the service writes once.
//   • Use sin8/scale8/CHSV for cheap integer math (FastLED helpers).
// ---------------------------------------------------------------------------

static void _patOff(CRGB out[6], uint32_t /*now_ms*/) {
    for (int i = 0; i < 6; i++) out[i] = CRGB::Black;
}

static void _patCharging(CRGB out[6], uint32_t now_ms) {
    // Slow green pulse, ~2 s period, brightness ~30..160.
    uint8_t phase = (uint8_t)((now_ms * 256u / 2000u) & 0xFF);
    uint8_t b     = (uint8_t)(scale8(sin8(phase), 130) + 30);
    CRGB c(0, b, 0);
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patFullyCharged(CRGB out[6], uint32_t /*now_ms*/) {
    // Restful steady dim green.
    CRGB c(0, 60, 0);
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patLowBattery(CRGB out[6], uint32_t now_ms) {
    // Slow red pulse, ~3 s period, brightness ~30..200.
    uint8_t phase = (uint8_t)((now_ms * 256u / 3000u) & 0xFF);
    uint8_t b     = (uint8_t)(scale8(sin8(phase), 170) + 30);
    CRGB c(b, 0, 0);
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patSerial(CRGB out[6], uint32_t now_ms) {
    // Smooth blue half-ring gradient that rotates around the loop. Brightness
    // peaks at the "head" and falls along a cosine curve over half the ring
    // behind it; the leading half stays dark. Half off / half lit, no discrete
    // bright pixels — the whole ring fades through together.
    const uint32_t period_ms = 3000;
    uint8_t head = (uint8_t)((now_ms * 256u / period_ms) & 0xFF);

    for (int i = 0; i < 6; i++) {
        uint8_t led_pos = (uint8_t)((i * 256) / 6);
        // uint8 modular subtraction = "how far is this LED behind the head"
        // (0 at head, 128 at exactly opposite, wrapping at 256).
        uint8_t diff = (uint8_t)(head - led_pos);

        // Cosine falloff over the trailing half; ahead-of-head half is dark.
        // cos8 returns 255 at 0, drops smoothly to ~0 at 128.
        uint8_t b = (diff < 128) ? cos8(diff) : 0;

        // Deep blue with a small cyan tint at the brightest part.
        out[i] = CRGB(0, scale8(b, 40), b);
    }
}

static void _patAlertDefault(CRGB out[6], uint32_t now_ms) {
    // Bright red strobe, 150 ms on / 150 ms off.
    bool on = ((now_ms / 150u) & 1u) == 0u;
    CRGB c = on ? CRGB(255, 0, 0) : CRGB::Black;
    for (int i = 0; i < 6; i++) out[i] = c;
}

static void _patRedBlueChaser(CRGB out[6], uint32_t now_ms) {
    // 3+3 split alternating ~120 ms — uses the physical left/right grouping.
    bool red_phase = ((now_ms / 120u) & 1u) == 0u;
    CRGB red(220, 0, 0);
    CRGB blue(0, 0, 220);
    if (red_phase) {
        out[0] = out[1] = out[2] = red;
        out[3] = out[4] = out[5] = CRGB::Black;
    } else {
        out[0] = out[1] = out[2] = CRGB::Black;
        out[3] = out[4] = out[5] = blue;
    }
}

static void _renderRainbow(CRGB out[6], uint32_t now_ms, uint32_t period_ms) {
    // Smooth rainbow, all six lit, hue offset 60° apart, rotating once per period.
    uint8_t base_hue = (uint8_t)((now_ms * 256u / period_ms) & 0xFF);
    for (int i = 0; i < 6; i++) {
        uint8_t hue = base_hue + (uint8_t)((i * 256) / 6);
        out[i] = CHSV(hue, 255, 200);
    }
}

static void _patRainbowFast(CRGB out[6], uint32_t now_ms) { _renderRainbow(out, now_ms, 1500); }
static void _patRainbowSlow(CRGB out[6], uint32_t now_ms) { _renderRainbow(out, now_ms, 5000); }

static void _patWhiteChaser(CRGB out[6], uint32_t now_ms) {
    // Single bright "head" racing the ring at ~600 ms/lap, with a sharp fade
    // either side that briefly overlaps adjacent LEDs for a smooth glide.
    const uint32_t lap = 600;
    uint32_t phase = now_ms % lap;

    // Position in fixed-point fractional-LED units: 0..(6 * 256) - 1.
    int pos = (int)((phase * (6u * 256u)) / lap);

    for (int i = 0; i < 6; i++) {
        int center = i * 256;
        int diff = pos - center;
        // Wrap to nearest direction around the ring.
        if (diff >  3 * 256) diff -= 6 * 256;
        if (diff < -3 * 256) diff += 6 * 256;
        int dist = diff < 0 ? -diff : diff;

        // Sharp falloff: peak (220) at dist 0, zero by dist 200.
        uint8_t b = 0;
        if (dist < 200) {
            b = (uint8_t)(220 * (200 - dist) / 200);
        }
        out[i] = CRGB(b, b, b);
    }
}

// ---------------------------------------------------------------------------

static void _renderPattern(LedPatternId p, uint32_t now_ms, CRGB out[6]) {
    switch (p) {
        case LED_PATTERN_OFF:             _patOff(out, now_ms);           break;
        case LED_PATTERN_CHARGING:        _patCharging(out, now_ms);      break;
        case LED_PATTERN_FULLY_CHARGED:   _patFullyCharged(out, now_ms);  break;
        case LED_PATTERN_SERIAL:          _patSerial(out, now_ms);        break;
        case LED_PATTERN_LOW_BATTERY:     _patLowBattery(out, now_ms);    break;
        case LED_PATTERN_ALERT_DEFAULT:   _patAlertDefault(out, now_ms);  break;
        case LED_PATTERN_RED_BLUE_CHASER: _patRedBlueChaser(out, now_ms); break;
        case LED_PATTERN_RAINBOW_FAST:    _patRainbowFast(out, now_ms);   break;
        case LED_PATTERN_RAINBOW_SLOW:    _patRainbowSlow(out, now_ms);   break;
        case LED_PATTERN_WHITE_CHASER:    _patWhiteChaser(out, now_ms);   break;
        default:                          _patOff(out, now_ms);           break;
    }
}

static void _scaleFrame(CRGB frame[6], uint8_t alpha) {
    if (alpha == 255) return;
    for (int i = 0; i < 6; i++) frame[i].nscale8(alpha);
}

// ---------------------------------------------------------------------------
// LedService
// ---------------------------------------------------------------------------

void LedService::begin(EventBus& bus) {
    _bus = &bus;

    bus.subscribe(EV_BATTERY_UPDATED, this);
    bus.subscribe(EV_TICK_1S,         this);
    bus.subscribe(EV_ALERT_RAISED,    this);
    bus.subscribe(EV_ALERT_CLEARED,   this);
}

void LedService::tick() {
    uint32_t now = millis();
    if (now - _last_render_ms < FRAME_PERIOD_MS) return;
    _last_render_ms = now;

    // ---- Layer composition ----
    // Alert preempts ambient. When an alert's duration expires (or someone
    // clears it via EV_ALERT_CLEARED), we fade-out over ALERT_FADE_MS by
    // scaling the alert frame's brightness, then drop the alert layer entirely
    // so the ambient pattern resumes on the next frame.
    LedPatternId effective = _ambient;
    uint8_t      alpha     = 255;

    if (_alert != LED_PATTERN_OFF) {
        // Trigger fade if the alert's duration has elapsed.
        if (_alert_until_ms != 0 && now >= _alert_until_ms && _alert_fade_start_ms == 0) {
            _alert_fade_start_ms = now;
        }

        if (_alert_fade_start_ms != 0) {
            uint32_t elapsed = now - _alert_fade_start_ms;
            if (elapsed >= ALERT_FADE_MS) {
                // Fade complete — drop the layer.
                _alert               = LED_PATTERN_OFF;
                _alert_until_ms      = 0;
                _alert_fade_start_ms = 0;
            } else {
                effective = _alert;
                alpha     = (uint8_t)(255u - (elapsed * 255u / ALERT_FADE_MS));
            }
        } else {
            effective = _alert;
        }
    }

    // ---- Render and push ----
    CRGB frame[6];
    _renderPattern(effective, now, frame);
    _scaleFrame(frame, alpha);
    g_hal.writeLEDFrame(frame);
}

void LedService::onEvent(const Event& e) {
    switch (e.id) {
        case EV_BATTERY_UPDATED: {
            uint8_t pct = e.data.battery.pct;
            _is_charging = (pct == BATT_PCT_CHARGING);
            _is_charged  = (pct == BATT_PCT_CHARGED);
            // Low only when on battery, with a real percentage reading (sentinels are >100).
            _is_low_batt = (pct <= 100) && (pct < LOW_BATTERY_PCT) && !_is_charging;
            _recomputeAmbient();
            break;
        }

        case EV_TICK_1S: {
            // Re-evaluate serial state once per second so the comet kicks in/out
            // when a terminal opens/closes.
            bool serial_now = (bool)Serial;
            if (serial_now != _last_serial) {
                _last_serial = serial_now;
                _recomputeAmbient();
            }
            break;
        }

        case EV_ALERT_RAISED:
            // Use the default pattern + duration for now. Once AlertPayload
            // carries a pattern_id (from the rules engine), read it from
            // e.data.alert and pass it to playAlertPattern instead.
            if (g_settings.getBool(SKEY_ALERT_LED)) {
                playAlertPattern(LED_PATTERN_ALERT_DEFAULT, ALERT_DEFAULT_MS);
            }
            break;

        case EV_ALERT_CLEARED:
            // Start fade-out immediately if an alert is currently playing.
            if (_alert != LED_PATTERN_OFF && _alert_fade_start_ms == 0) {
                _alert_fade_start_ms = millis();
            }
            break;

        default:
            break;
    }
}

void LedService::playAlertPattern(LedPatternId pattern, uint32_t duration_ms) {
    _alert               = pattern;
    _alert_until_ms      = (duration_ms > 0) ? (millis() + duration_ms) : 0;
    _alert_fade_start_ms = 0;
}

void LedService::_recomputeAmbient() {
    // Priority order: serial > charging > charged > low-battery > off.
    //
    // (bool)Serial is only true when a host has actively opened the CDC port
    // (developer present), so it's a stronger signal of intent than passive
    // charging. Showing the comet while a terminal is open also confirms the
    // device is responsive to that connection.
    LedPatternId next = LED_PATTERN_OFF;
    if      (_last_serial) next = LED_PATTERN_SERIAL;
    else if (_is_charging) next = LED_PATTERN_CHARGING;
    else if (_is_charged)  next = LED_PATTERN_FULLY_CHARGED;
    else if (_is_low_batt) next = LED_PATTERN_LOW_BATTERY;
    _ambient = next;
}
