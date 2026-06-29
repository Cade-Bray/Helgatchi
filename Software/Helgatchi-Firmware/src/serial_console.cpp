#include "serial_console.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "event_payload.h"
#include "led_service.h"
#include "vibe_service.h"
#include "hal.h"
#include "power_manager.h"
#include "ui_controller.h"
#include <Arduino.h>
#include <FastLED.h>
#include <lvgl.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Human-readable formatters for stats output. Each writes into a caller-
// supplied buffer so multiple values can appear in the same printf without
// stepping on each other.
// ---------------------------------------------------------------------------

static const char* fmt_bytes(char* out, size_t out_sz, uint32_t bytes) {
    if      (bytes >= (1U << 20)) snprintf(out, out_sz, "%.1f MB", bytes / (double)(1U << 20));
    else if (bytes >= (1U << 10)) snprintf(out, out_sz, "%.1f KB", bytes / (double)(1U << 10));
    else                          snprintf(out, out_sz, "%u B",    (unsigned)bytes);
    return out;
}

static const char* fmt_hz(char* out, size_t out_sz, uint32_t hz) {
    if      (hz >= 1000000) snprintf(out, out_sz, "%u MHz", (unsigned)(hz / 1000000));
    else if (hz >= 1000)    snprintf(out, out_sz, "%u kHz", (unsigned)(hz / 1000));
    else                    snprintf(out, out_sz, "%u Hz",  (unsigned)hz);
    return out;
}

static const char* fmt_uptime(char* out, size_t out_sz, uint32_t ms) {
    uint32_t s = ms / 1000;
    if      (s < 60)   snprintf(out, out_sz, "%u.%03us",       (unsigned)s,               (unsigned)(ms % 1000));
    else if (s < 3600) snprintf(out, out_sz, "%um %02us",      (unsigned)(s/60),          (unsigned)(s%60));
    else               snprintf(out, out_sz, "%uh %02um %02us",(unsigned)(s/3600),
                                              (unsigned)((s/60)%60), (unsigned)(s%60));
    return out;
}

SerialConsole g_console;

// Keep in sync with SettingsKey enum order. The array size is intentionally
// inferred from the initializers — if you add an enum entry but forget a name
// here, the static_assert below fails at compile time (instead of nullptr-deref
// at runtime, which is what happened when SKEY_NO_SLEEP_WHILE_CHARGING was added).
static const char* const s_key_name[] = {
    "SCREEN_BRIGHTNESS",       // 0
    "LED_BRIGHTNESS",          // 1
    "SCAN_MODE",               // 2
    "PERF_MODE",               // 3
    "ALERT_WAKE_SCREEN",       // 4
    "ALERT_VIBRATION",         // 5
    "ALERT_LED",               // 6
    "SLEEP_WHILE_USB",         // 7
    "DEBUG_SERIAL",            // 8
    "DEBUG_LEVEL",             // 9
    "DEBUG_SLEEP_W_SERIAL",    // 10
    "SCREEN_TIMEOUT_S",        // 11
    "INTERACTIVE_TIMEOUT_S",   // 12
    "WAKE_DURATION_S",         // 13
    "SCAN_DURATION_S",         // 14
    "BLE_SCAN_WINDOW_MS",      // 15
    "BLE_SCAN_INTERVAL_MS",    // 16
    "WIFI_DWELL_MS",           // 17
    "WIFI_HOP_INTERVAL_MS",    // 18
    "TUTORIAL_SHOWN",          // 19
};
static_assert(sizeof(s_key_name) / sizeof(s_key_name[0]) == SKEY_COUNT,
              "s_key_name is out of sync with SettingsKey");

// LED pattern names — keep in sync with the LedPatternId enum.
static const char* const s_led_name[] = {
    "off",
    "charging",
    "fully_charged",
    "serial",
    "low_battery",
    "alert",
    "red_blue",
    "rainbow_fast",
    "rainbow_slow",
    "white_chaser",
};
static_assert(sizeof(s_led_name) / sizeof(s_led_name[0]) == LED_PATTERN_COUNT,
              "s_led_name is out of sync with LedPatternId");

// Haptic pattern names — keep in sync with the HapticPatternId enum.
static const char* const s_vibe_name[] = {
    "off",
    "tick_light",
    "tick",
    "bump",
    "double_tap",
    "long_buzz",
};
static_assert(sizeof(s_vibe_name) / sizeof(s_vibe_name[0]) == HAPTIC_PATTERN_COUNT,
              "s_vibe_name is out of sync with HapticPatternId");

// ---------------------------------------------------------------------------

void SerialConsole::begin(EventBus& bus) {
    _bus = &bus;
    _pos = 0;
}

void SerialConsole::tick() {
    bool connected = (bool)Serial;

    if (connected != _was_connected) {
        _pos = 0;
        _was_connected = connected;
    }

    if (!connected) return;

    while (Serial.available()) {
        char c = (char)Serial.read();

        if (c == '\r') continue;

        if (c == '\n') {
            Serial.println();
            _buf[_pos] = '\0';
            if (_pos > 0) {
                _dispatch(_buf);
                Serial.println();
            }
            _pos = 0;

        } else if (c == 0x7F || c == '\b') {
            if (_pos > 0) {
                _pos--;
                Serial.print("\b \b");  // overwrite with space then move back
            }

        } else if (_pos < BUF_LEN - 1) {
            _buf[_pos++] = c;
            Serial.print(c);  // local echo
            _bus->post(EV_UI_ACTIVITY);  // reset interactive sleep timer
        }
    }
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void SerialConsole::_dispatch(char* line) {
    char* verb = strtok(line, " ");
    char* rest = strtok(nullptr, "");
    if (!verb) return;

    if      (strcmp(verb, "help")     == 0) _cmdHelp();
    else if (strcmp(verb, "settings") == 0) _cmdSettings(rest);
    else if (strcmp(verb, "bus")      == 0) _cmdBus(rest);
    else if (strcmp(verb, "stats")    == 0) _cmdStats();
    else if (strcmp(verb, "led")      == 0) _cmdLed(rest);
    else if (strcmp(verb, "vibe")     == 0) _cmdVibe(rest);
    else if (strcmp(verb, "battery")  == 0) _cmdBattery();
    else if (strcmp(verb, "selftest") == 0) _cmdSelftest();
    else if (strcmp(verb, "reboot")   == 0) {
        Serial.println("rebooting...");
        delay(100);
        ESP.restart();
    }
    else Serial.printf("unknown command '%s'  (try 'help')\n", verb);
}

void SerialConsole::_cmdHelp() {
    Serial.println("commands:");
    Serial.println("  settings                    list all settings with current values");
    Serial.println("  settings set <id> <value>   write setting by numeric id (in-memory)");
    Serial.println("  settings save               persist current settings to NVS");
    Serial.println("  settings reset              restore factory defaults");
    Serial.println("  bus post <event_id>         post an event by numeric id");
    Serial.println("  stats                       device / chip / memory / display / bus drop count");
    Serial.println("  led                         list LED patterns");
    Serial.println("  led <name|id> [ms]          play pattern (ms=0 or omitted = until preempted)");
    Serial.println("  led off                     clear the alert layer (returns to ambient)");
    Serial.println("  led bright <0-255>          override FastLED brightness (debug)");
    Serial.println("  vibe                        list haptic patterns");
    Serial.println("  vibe <name|id>              play haptic pattern (gated by ALERT_VIBRATION)");
    Serial.println("  vibe off                    stop motor immediately");
    Serial.println("  battery                     voltage / pct / charging state / curve anchors");
    Serial.println("  selftest                    probe GPIO pins for shorts / unexpected loads");
    Serial.println("  reboot                      restart the device");
}

void SerialConsole::_cmdSettings(char* args) {
    if (!args) {
        Serial.println(" id  name                    value");
        Serial.println("---  ----------------------  -----");
        for (uint8_t k = 0; k < SKEY_COUNT; k++) {
            Serial.printf("%3u  %-22s  %lu\n",
                          k, s_key_name[k],
                          (unsigned long)g_settings.get((SettingsKey)k));
        }
        return;
    }

    char* sub = strtok(args, " ");
    if (!sub) return;

    if (strcmp(sub, "save") == 0) {
        _bus->post(CMD_SETTINGS_SAVE);
        Serial.println("OK: settings saved to NVS");
        return;
    }

    if (strcmp(sub, "reset") == 0) {
        _bus->post(CMD_SETTINGS_RESET_DEFAULTS);
        Serial.println("OK: settings reset to factory defaults");
        return;
    }

    if (strcmp(sub, "set") == 0) {
        char* k_str = strtok(nullptr, " ");
        char* v_str = strtok(nullptr, " ");
        if (!k_str || !v_str) {
            Serial.println("usage: settings set <id> <value>");
            return;
        }
        uint8_t  key = (uint8_t)atoi(k_str);
        uint32_t val = (uint32_t)atol(v_str);
        if (key >= SKEY_COUNT) {
            Serial.printf("bad id %u (valid: 0-%u)\n", key, SKEY_COUNT - 1);
            return;
        }
        EventPayload p{};
        p.settings_set.key   = (SettingsKey)key;
        p.settings_set.value = val;
        _bus->post(CMD_SETTINGS_SET, p);
        Serial.printf("OK: [%u] %s = %lu\n", key, s_key_name[key], (unsigned long)val);
        return;
    }

    Serial.printf("unknown subcommand 'settings %s'  (try 'help')\n", sub);
}

void SerialConsole::_cmdBus(char* args) {
    if (!args) { Serial.println("usage: bus post <event_id>"); return; }
    char* sub = strtok(args, " ");
    if (!sub || strcmp(sub, "post") != 0) {
        Serial.println("usage: bus post <event_id>");
        return;
    }
    char* id_str = strtok(nullptr, " ");
    if (!id_str) { Serial.println("usage: bus post <event_id>"); return; }

    EventId id = (EventId)atoi(id_str);
    _bus->post(id);
    Serial.printf("OK: posted event %u\n", (unsigned)id);
}

void SerialConsole::_cmdStats() {
    char b1[24], b2[24], b3[24], b4[24];

    Serial.printf("uptime:     %s\n", fmt_uptime(b1, sizeof(b1), millis()));

    // Chip
    Serial.printf("chip:       %s rev %u, %u core(s) @ %u MHz, IDF %s\n",
                  ESP.getChipModel(),
                  (unsigned)ESP.getChipRevision(),
                  (unsigned)ESP.getChipCores(),
                  (unsigned)ESP.getCpuFreqMHz(),
                  ESP.getSdkVersion());

    // Flash + sketch occupancy. Sketch has no "min seen" — it's flashed
    // once, the value can't change at runtime.
    Serial.printf("flash:      %s @ %s\n",
                  fmt_bytes(b1, sizeof(b1), ESP.getFlashChipSize()),
                  fmt_hz   (b2, sizeof(b2), ESP.getFlashChipSpeed()));
    Serial.printf("sketch:     %s / %s used (%s unused in app partition)\n",
                  fmt_bytes(b1, sizeof(b1), ESP.getSketchSize()),
                  fmt_bytes(b2, sizeof(b2), ESP.getSketchSize() + ESP.getFreeSketchSpace()),
                  fmt_bytes(b3, sizeof(b3), ESP.getFreeSketchSpace()));

    // Internal SRAM heap. "min seen" = lifetime minimum free = high-water
    // mark for usage (how close we've come to exhaustion).
    {
        const uint32_t total = ESP.getHeapSize();
        const uint32_t free  = ESP.getFreeHeap();
        Serial.printf("heap:       %s / %s used (%s unused, min seen %s)\n",
                      fmt_bytes(b1, sizeof(b1), total - free),
                      fmt_bytes(b2, sizeof(b2), total),
                      fmt_bytes(b3, sizeof(b3), free),
                      fmt_bytes(b4, sizeof(b4), ESP.getMinFreeHeap()));
    }

    // PSRAM (0 if chip variant has none, or PSRAM init failed)
    const size_t psram_total = ESP.getPsramSize();
    if (psram_total) {
        const uint32_t free = ESP.getFreePsram();
        Serial.printf("psram:      %s / %s used (%s unused, min seen %s)\n",
                      fmt_bytes(b1, sizeof(b1), (uint32_t)psram_total - free),
                      fmt_bytes(b2, sizeof(b2), (uint32_t)psram_total),
                      fmt_bytes(b3, sizeof(b3), free),
                      fmt_bytes(b4, sizeof(b4), ESP.getMinFreePsram()));
    } else {
        Serial.println("psram:      absent");
    }

    // LVGL builtin allocator pool (LV_MEM_SIZE). `max_used` is the lifetime
    // peak — "min seen" of free = total - max_used. If that watermark stays
    // well above zero, LV_MEM_SIZE can be shrunk. frag_pct rising means
    // heavy churn (rare for static UIs).
    lv_mem_monitor_t lv_mon;
    lv_mem_monitor(&lv_mon);
    Serial.printf("lv_mem:     %s / %s used (%s unused, min seen %s, frag %u%%)\n",
                  fmt_bytes(b1, sizeof(b1), lv_mon.total_size - lv_mon.free_size),
                  fmt_bytes(b2, sizeof(b2), lv_mon.total_size),
                  fmt_bytes(b3, sizeof(b3), lv_mon.free_size),
                  fmt_bytes(b4, sizeof(b4), lv_mon.total_size - lv_mon.max_used),
                  (unsigned)lv_mon.frag_pct);

    // Display info. Strip count comes from ground-truth flush_cb counter;
    // frame rate is strips/2 in current 120-row PARTIAL setup (1-2 strips
    // per dirty frame). Panel internal scan rate is fixed by ST7789 default
    // register values — see Architecture notes for why this isn't host-
    // controllable on SPI panels.
    uint32_t flushes, elapsed_ms;
    g_ui.getDisplayStats(flushes, elapsed_ms);
    Serial.printf("display:    SPI @ 80 MHz, panel scan ~60 Hz (ST7789 default)\n");
    if (elapsed_ms > 0) {
        const uint32_t strips_per_s = flushes * 1000 / elapsed_ms;
        Serial.printf("flushes:    %u strips in %s (%u strips/s, ~%u fps)\n",
                      (unsigned)flushes,
                      fmt_uptime(b1, sizeof(b1), elapsed_ms),
                      (unsigned)strips_per_s,
                      (unsigned)(strips_per_s / 2));
    } else {
        Serial.println("flushes:    (call stats again after some activity)");
    }

    Serial.printf("bus drops:  %u\n", g_bus.droppedCount());
}

void SerialConsole::_cmdLed(char* args) {
    // No args → list available patterns.
    if (!args) {
        Serial.println(" id  name");
        Serial.println("---  --------------");
        for (uint8_t i = 0; i < LED_PATTERN_COUNT; i++) {
            Serial.printf("%3u  %s\n", i, s_led_name[i]);
        }
        Serial.println();
        Serial.println("usage: led <name|id> [ms]   (ms=0 or omitted = until preempted)");
        return;
    }

    char* arg1 = strtok(args, " ");
    char* arg2 = strtok(nullptr, " ");

    // `led bright <0-255>` — overrides FastLED's global brightness directly,
    // bypassing the LED_BRIGHTNESS setting and HAL_LED_LEVELS scaling. Useful
    // for diagnosing whether LEDs are responding-but-dim vs. not-responding.
    // Reverts on next EV_SETTINGS_CHANGED (or reboot).
    if (strcmp(arg1, "bright") == 0 || strcmp(arg1, "brightness") == 0) {
        if (!arg2) {
            Serial.println("usage: led bright <0-255>");
            return;
        }
        int n = atoi(arg2);
        if (n < 0)   n = 0;
        if (n > 255) n = 255;
        FastLED.setBrightness((uint8_t)n);
        FastLED.show();
        Serial.printf("OK: FastLED brightness = %d (reverts on settings change / reboot)\n", n);
        return;
    }

    // Resolve pattern: try name first, then numeric id.
    LedPatternId pat = LED_PATTERN_COUNT;  // sentinel: "not found"
    for (uint8_t i = 0; i < LED_PATTERN_COUNT; i++) {
        if (strcmp(arg1, s_led_name[i]) == 0) { pat = (LedPatternId)i; break; }
    }
    if (pat == LED_PATTERN_COUNT) {
        char* end = nullptr;
        long n = strtol(arg1, &end, 10);
        if (end != arg1 && *end == '\0' && n >= 0 && n < LED_PATTERN_COUNT) {
            pat = (LedPatternId)n;
        }
    }
    if (pat == LED_PATTERN_COUNT) {
        Serial.printf("unknown pattern '%s'  (try 'led' for the list)\n", arg1);
        return;
    }

    uint32_t duration_ms = arg2 ? (uint32_t)atol(arg2) : 0;

    g_leds.playAlertPattern(pat, duration_ms);
    if (pat == LED_PATTERN_OFF) {
        Serial.println("OK: alert layer cleared");
    } else if (duration_ms > 0) {
        Serial.printf("OK: %s for %lu ms\n", s_led_name[pat], (unsigned long)duration_ms);
    } else {
        Serial.printf("OK: %s (until preempted or 'led off')\n", s_led_name[pat]);
    }
}

void SerialConsole::_cmdVibe(char* args) {
    if (!args) {
        Serial.println(" id  name");
        Serial.println("---  -----------");
        for (uint8_t i = 0; i < HAPTIC_PATTERN_COUNT; i++) {
            Serial.printf("%3u  %s\n", i, s_vibe_name[i]);
        }
        Serial.println();
        Serial.println("usage: vibe <name|id>");
        Serial.println("note:  gated by SKEY_ALERT_VIBRATION (turn off → motor silent)");
        return;
    }

    char* arg1 = strtok(args, " ");

    HapticPatternId pat = HAPTIC_PATTERN_COUNT;
    for (uint8_t i = 0; i < HAPTIC_PATTERN_COUNT; i++) {
        if (strcmp(arg1, s_vibe_name[i]) == 0) { pat = (HapticPatternId)i; break; }
    }
    if (pat == HAPTIC_PATTERN_COUNT) {
        char* end = nullptr;
        long n = strtol(arg1, &end, 10);
        if (end != arg1 && *end == '\0' && n >= 0 && n < HAPTIC_PATTERN_COUNT) {
            pat = (HapticPatternId)n;
        }
    }
    if (pat == HAPTIC_PATTERN_COUNT) {
        Serial.printf("unknown pattern '%s'  (try 'vibe' for the list)\n", arg1);
        return;
    }

    g_vibe.play(pat);
    Serial.printf("OK: %s\n", s_vibe_name[pat]);
}

// ---------------------------------------------------------------------------
// `selftest` — GPIO short-detection scan
//
// For each testable digital pin: briefly reconfigure as INPUT_PULLUP and
// INPUT_PULLDOWN, read the line in each state. Compare:
//   HIGH/LOW  → FLOATING (internal pull dominates — nothing strong external)
//   LOW/LOW   → external load to GND (or hard short)
//   HIGH/HIGH → external load to VCC (or hard short)
//
// For VSENSE: read analog mV directly. 0 mV usually means battery missing or
// divider open; rail-pegged means short to the input side of the divider.
//
// What's NOT tested and why:
//   • SPI_DC, SPI_SCK, SPI_RST, SPI_MOSI, SPI_CS — the display SPI bus runs
//     continuously and we can't restore the IO_MUX→SPI routing reliably from
//     a one-shot Arduino-API call. The display's own init at boot is the
//     implicit test for these (if it failed, you'd see no UI).
//   • LED_DATA — tested, but FastLED's RMT routing isn't cleanly restorable
//     from a pinMode() switch. Recommend reboot after selftest if LEDs
//     don't respond. Or run `led <pattern>` to verify they do.
//
// Don't press buttons during the scan — a held button looks identical to a
// short to GND on the matrix lines.
// ---------------------------------------------------------------------------

static void _selftestDigitalPin(uint8_t gpio, const char* label) {
    pinMode(gpio, INPUT_PULLUP);
    delayMicroseconds(50);
    int hi = digitalRead(gpio);
    pinMode(gpio, INPUT_PULLDOWN);
    delayMicroseconds(50);
    int lo = digitalRead(gpio);

    Serial.printf(" %3u    %-19s  ", gpio, label);
    if      (hi == HIGH && lo == LOW)  Serial.println("FLOATING     OK");
    else if (hi == LOW  && lo == LOW)  Serial.println("PULLED_LOW   external load to GND / SHORT_GND?");
    else if (hi == HIGH && lo == HIGH) Serial.println("PULLED_HIGH  external load to VCC / SHORT_VCC?");
    else                               Serial.println("ANOMALOUS    inverted — pin actively driven?");
}

void SerialConsole::_cmdSelftest() {
    Serial.println("=== GPIO self-test ===");
    Serial.println("Skipped: SPI bus pins (SCK/MOSI/CS/DC/RST). Display init at boot is their implicit test.");
    Serial.println("Don't press buttons during the scan.");
    Serial.println();
    Serial.println(" GPIO   Pin                  Result");
    Serial.println(" ----   -------------------  -------------------------------------------");

    // --- Analog: VSENSE ----------------------------------------------------
    analogSetAttenuation(ADC_11db);
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VSENSE);
    int vmv = sum / 8;
    Serial.printf(" %3u    %-19s  ADC=%-4d mV  ", PIN_VSENSE, "VSENSE   (GPIO5)", vmv);
    if      (vmv < 20)   Serial.println("(0 V — battery missing or divider open)");
    else if (vmv > 3200) Serial.println("(rail — short to VCC / no divider)");
    else                 Serial.printf ("(vbatt~%d mV via /2 divider)\n", vmv * 2);

    // --- Digital: testable pins -------------------------------------------
    struct DigPin { uint8_t gpio; const char* label; };
    static const DigPin pins[] = {
        {PIN_LED_DATA, "LED_DATA (GPIO2)"},
        {PIN_SPI_BL,   "SPI_BL   (GPIO3)"},
        {PIN_VIBRATE,  "VIBRATE  (GPIO4)"},
        {PIN_BTN_1,    "BTN_1    (GPIO6)"},
        {PIN_BTN_2,    "BTN_2    (GPIO43)"},
    };
    for (const auto& p : pins) _selftestDigitalPin(p.gpio, p.label);

    // --- Restore peripheral routing ---------------------------------------
    // Backlight + motor: re-attach LEDC channels (the pinMode() switch above
    // detached the IO_MUX from the LEDC peripheral). After re-attach the
    // channel's last duty re-applies; we still wakeDisplay() to be explicit.
    ledcAttachPin(PIN_SPI_BL,  HAL_BL_LEDC_CH);
    ledcAttachPin(PIN_VIBRATE, HAL_VIBE_LEDC_CH);
    ledcWrite(HAL_VIBE_LEDC_CH, 0);
    g_hal.wakeDisplay();

    // Buttons: HAL::_pollButtons re-uses INPUT_PULLUP every tick anyway, but
    // leave them in a sane state for the in-between reads.
    pinMode(PIN_BTN_1, INPUT_PULLUP);
    pinMode(PIN_BTN_2, INPUT_PULLUP);

    // LED_DATA: best-effort. FastLED's RMT routing is one-shot at addLeds()
    // and a pinMode() switch detaches it. Drive LOW so the data line idles
    // safely; LEDs may not light again until reboot.
    pinMode(PIN_LED_DATA, OUTPUT);
    digitalWrite(PIN_LED_DATA, LOW);

    Serial.println();
    Serial.println("Done. If LEDs are unresponsive after this command, reboot to restore FastLED's RMT routing.");
}

void SerialConsole::_cmdBattery() {
    // Take a fresh ADC reading so we report current voltage, not the cached
    // 30s-interval sample. The fresh raw% (curve, no smoothing) is shown
    // alongside the smoothed value the UI is currently using.
    uint16_t vsense   = g_hal.readVsenseMv();
    uint16_t vbatt_mv = vsense * 2;
    uint8_t  raw_pct  = pmBattPctFromVsenseMv(vsense);
    bool     usb      = g_hal.usbAttached();
    bool     ser      = (bool)Serial;

    Serial.printf("vsense:    %u mV  (raw ADC, post-divider)\n", vsense);
    Serial.printf("vbatt:     %u mV  (= vsense * 2)\n",          vbatt_mv);
    Serial.printf("raw pct:   %u%%   (curve, no smoothing)\n",   raw_pct);

    uint8_t  last_pct = g_power.lastBatteryPct();
    uint16_t last_mv  = g_power.lastBatteryMv();
    Serial.print("last sent: ");
    if      (last_pct == 0xFF)              Serial.println("none yet");
    else if (last_pct == BATT_PCT_CHARGING) Serial.printf("CHARGING       (mv=%u)\n", last_mv);
    else if (last_pct == BATT_PCT_CHARGED)  Serial.printf("CHARGED        (mv=%u)\n", last_mv);
    else if (last_pct == BATT_PCT_MISSING)  Serial.println("MISSING/FAULT");
    else                                    Serial.printf("%u%%            (mv=%u, EMA-smoothed)\n",
                                                          last_pct, last_mv);

    Serial.print("charging:  ");
    if      (last_pct == BATT_PCT_CHARGED)  Serial.println("yes (full)");
    else if (last_pct == BATT_PCT_CHARGING) Serial.println("yes");
    else if (usb)                           Serial.println("usb attached, not yet sampled as charging");
    else                                    Serial.println("no (discharging)");

    Serial.printf("USB:       %s\n", usb ? "attached" : "no");
    Serial.printf("Serial:    %s\n", ser ? "connected" : "no");
    Serial.printf("curve:     100%% @ %u mV vbatt  /  0%% @ %u mV vbatt  (LUT, %u points)\n",
                  PM_VSENSE_FULL_MV * 2, PM_VSENSE_DEAD_MV * 2, PM_BATT_CURVE_N);
}
