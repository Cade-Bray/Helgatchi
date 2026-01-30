
#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_err.h>
#include "esp_sleep.h"

#include "../core/CoreState.h"

class PowerManager {
public:
  WakeReason getWakeReason() {
    const auto cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
      case ESP_SLEEP_WAKEUP_TIMER:
        return WakeReason::Timer;
      case ESP_SLEEP_WAKEUP_EXT0:
      case ESP_SLEEP_WAKEUP_EXT1:
      case ESP_SLEEP_WAKEUP_GPIO:
        return WakeReason::Button;
      default:
        return WakeReason::ColdBoot;
    }
  }

  // Enter deep sleep for `ms`, optionally also enabling wake on a single GPIO.
  // NOTE: "Long-press wake" can't be detected during deep sleep; you can only
  // wake on a level/edge. If you want long-press semantics, wake on the button
  // and then require the button to stay held after boot.
  [[noreturn]] void sleepForMs(uint32_t ms, int wakeGpio = -1, bool wakeActiveLow = true, uint8_t pullMode = 1) {
    // Clear wake sources so we control policy from one place.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);

    if (wakeGpio >= 0) {
      const gpio_num_t gpio = (gpio_num_t)wakeGpio;

      // Prefer EXT0 for a single wake button. It's the most reliable deep-sleep
      // wake source, but requires an RTC-capable GPIO.
      // wakeActiveLow=true means wake on level 0.
      rtc_gpio_init(gpio);
      rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_INPUT_ONLY);
      if (pullMode == 1) {
        rtc_gpio_pullup_en(gpio);
        rtc_gpio_pulldown_dis(gpio);
      } else if (pullMode == 2) {
        rtc_gpio_pulldown_en(gpio);
        rtc_gpio_pullup_dis(gpio);
      } else {
        rtc_gpio_pullup_dis(gpio);
        rtc_gpio_pulldown_dis(gpio);
      }

      esp_err_t err = esp_sleep_enable_ext0_wakeup(gpio, wakeActiveLow ? 0 : 1);
      if (err != ESP_OK) {
        // If EXT0 fails (non-RTC pin), fall back to EXT1 with a single-bit mask.
        // If that fails too, we skip button wake rather than risk a wake storm.
        const uint64_t mask = 1ULL << (uint8_t)wakeGpio;
        err = esp_sleep_enable_ext1_wakeup(mask,
                          wakeActiveLow ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH);

        if (err != ESP_OK) {
          // Don't attempt GPIO wake fallback here; it can be unreliable for deep sleep
          // and can cause immediate re-wakes if the pin floats.
        }
      }
    }

    Serial.flush();
    esp_deep_sleep_start();
    while (true) {
      delay(1000);
    }
  }
};
