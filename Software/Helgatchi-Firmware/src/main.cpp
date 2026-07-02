#include <Arduino.h>
#include "event_bus.h"
#include "settings_service.h"
#include "hal.h"
#include "log_service.h"
#include "serial_console.h"
#include "power_manager.h"
#include "display_service.h"
#include "settings_screen.h"
#include "alerts_screen.h"
#include "ui_controller.h"
#include "led_service.h"
#include "vibe_service.h"
#include "alerts_service.h"
#include "scan_service.h"
#include "scan_engine.h"
#include "rules_service.h"
#include <LittleFS.h>
#include <esp_sleep.h>
#include <esp_system.h>

static void _printBootInfo() {
    Serial.printf("[boot] chip:  %s rev%u  cores:%u\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("[boot] mac:   %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
                  (uint8_t)(mac >> 16), (uint8_t)(mac >>  8), (uint8_t)(mac));
    Serial.printf("[boot] heap:  %lu B free\n",  (unsigned long)ESP.getFreeHeap());
    Serial.printf("[boot] flash: %lu KB\n",       (unsigned long)(ESP.getFlashChipSize() / 1024));
    Serial.printf("[boot] scan:  mode=%u  perf=%u  scan_s=%u  sleep_s=%u\n",
                  g_settings.get(SKEY_SCAN_MODE),   g_settings.get(SKEY_PERF_MODE),
                  g_settings.get(SKEY_SCAN_DURATION_S), g_settings.get(SKEY_SLEEP_DURATION_S));
    Serial.printf("[boot] vsense: %u mV\n", g_hal.readVsenseMv());
    Serial.printf("[boot] debug: level=%u  sleep_w_serial=%u\n",
                  g_settings.get(SKEY_DEBUG_LEVEL),
                  g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL));
}

void setup() {
    Serial.begin(115200);

    // EARLIEST: on a button wake from deep sleep (regular or shipping), verify
    // the user is holding CENTER long enough — otherwise re-enter the same
    // sleep without spinning anything else up. May not return. Timer wakes and
    // cold boots pass straight through.
    PowerManager::checkWakeHoldOrResleep();

    const uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }
    delay(200);

    g_bus.begin();
    g_settings.begin(g_bus);
    g_hal.begin(g_bus);
    g_logger.begin(g_bus);
    g_console.begin(g_bus);
    g_power.begin(g_bus);
    g_alerts.begin(g_bus); // must precede led/vibe so they can find() records when EV_ALERT_RAISED fires
    g_scan.begin(g_bus);          // ring buffer + seen-devices map
    g_scan_engine.begin(g_bus);   // NimBLE driver — publishes into g_scan
    // LittleFS must be mounted before RulesService reads /rules/factory and
    // /rules/user. formatOnFail=true so a fresh device with no FS image
    // still boots (it'll just find an empty filesystem).
    if (!LittleFS.begin(true /* formatOnFail */)) {
        Serial.println("[fs] FATAL: LittleFS mount failed — rules subsystem disabled");
    }
    g_rules.begin(g_bus);  // must follow LittleFS mount + g_scan + g_alerts
    g_leds.begin(g_bus);   // depends on HAL (LED chain) + bus events from PowerManager
    g_vibe.begin(g_bus);   // haptic patterns; subscribes to button + alert events
    g_ui.begin(g_bus);     // creates the LVGL display — auto-shows perf overlay
    g_display.begin(g_bus); // top-bar indicators — must follow g_ui (objects.* must exist)
    g_settings_screen.begin(g_bus); // settings widget wiring — must follow g_ui
    g_alerts_screen.begin(g_bus);   // alert cards UI — must follow g_ui + g_display + g_alerts
    g_logger.applyPerfMonitor();   // re-hide unless level >= RENDERING_PERF

    if (g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED)) {
        _printBootInfo();
    }

    // Boot indicator: white LED flash + short haptic. Only fires for boots
    // the user *initiated* — fresh power-on, or button wake from deep sleep.
    // Software resets (Reboot button calling ESP.restart, panic, watchdog)
    // get NO indicator: the user just produced a haptic clicking the button
    // that triggered the reset, and a second haptic on the other side feels
    // like one long buzz. TIMER wakes (autonomous scan) also stay silent.
    {
        esp_sleep_wakeup_cause_t cause  = esp_sleep_get_wakeup_cause();
        esp_reset_reason_t       reset  = esp_reset_reason();
        bool show_indicator =
            (reset == ESP_RST_POWERON) ||
            (reset == ESP_RST_DEEPSLEEP && cause == ESP_SLEEP_WAKEUP_EXT1);

        if (show_indicator) {
            g_hal.setAllLEDs(30, 30, 30);
            if (g_settings.getBool(SKEY_ALERT_VIBRATION)) {
                g_hal.setVibrate(220);
                delay(60);
                g_hal.stopVibrate();
                delay(140);
            } else {
                delay(200);
            }
            g_hal.clearLEDs();
        }
    }

    Serial.println("[Helgatchi] boot OK");
}

void loop() {
    g_hal.tick();       // button polling + USB SOF detection + buzz timer
    g_bus.dispatch();   // drain event queue and call all handlers
    g_console.tick();   // process any pending serial input
    g_power.tick();     // scan/sleep cycle + battery sampling
    g_scan_engine.tick(); // drain NimBLE callback queue + publish to g_scan
    g_rules.tick();     // drain scan ring + match against loaded rules
    g_leds.tick();      // ~30 FPS LED pattern render (frame-skips internally)
    g_vibe.tick();      // advance haptic pattern step machine
    g_ui.tick();        // lv_timer_handler — drives LVGL rendering
}
