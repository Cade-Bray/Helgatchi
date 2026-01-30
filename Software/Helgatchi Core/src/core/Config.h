
#pragma once

// ============================================================================
// Core compile-time configuration
// ============================================================================

// Event queue capacity for the Core event bus ring buffer.
#ifndef EVENT_QUEUE_CAPACITY
#define EVENT_QUEUE_CAPACITY 64
#endif

// ============================================================================
// Pin configuration (adjust to your wiring)
// ============================================================================

// Buttons
#ifndef PIN_BTN_LEFT
#define PIN_BTN_LEFT 43
#endif

#ifndef PIN_BTN_RIGHT
#define PIN_BTN_RIGHT 5
#endif

#ifndef PIN_BTN_CENTER
#define PIN_BTN_CENTER 6
#endif

// Button electrical configuration
// If your button connects the GPIO to GND when pressed (common handheld wiring), use:
//   BUTTON_ACTIVE_LOW=1 and BUTTON_PULL_MODE=1 (internal pull-up)
// If your button connects the GPIO to 3V3 when pressed, use:
//   BUTTON_ACTIVE_LOW=0 and BUTTON_PULL_MODE=2 (internal pull-down) OR external resistor.
#ifndef BUTTON_ACTIVE_LOW
#define BUTTON_ACTIVE_LOW 1
#endif

// 0=None, 1=PullUp, 2=PullDown
#ifndef BUTTON_PULL_MODE
#define BUTTON_PULL_MODE 1
#endif

// Backlight
#ifndef PIN_BACKLIGHT
#define PIN_BACKLIGHT 3
#endif

// Vibration motor / haptic output
#ifndef PIN_VIB
// Set to a valid GPIO to enable; -1 disables vibration output.
// NOTE: do not share this pin with the backlight.
#define PIN_VIB -1
#endif

// Display (Adafruit ST77xx family)
// If your display uses different pins, override these via build flags.
#ifndef PIN_TFT_CS
#define PIN_TFT_CS 44
#endif

#ifndef PIN_TFT_DC
#define PIN_TFT_DC 1
#endif

#ifndef PIN_TFT_RST
#define PIN_TFT_RST 8
#endif

// SPI pins for the TFT (override to match your wiring)
#ifndef PIN_TFT_MOSI
#define PIN_TFT_MOSI 9
#endif

#ifndef PIN_TFT_SCLK
#define PIN_TFT_SCLK 7
#endif

// Helpful warnings for easy-to-miss pin conflicts
#if (PIN_BTN_LEFT == PIN_TFT_SCLK) || (PIN_BTN_RIGHT == PIN_TFT_SCLK) || (PIN_BTN_CENTER == PIN_TFT_SCLK)
#warning "A button pin overlaps PIN_TFT_SCLK; this will cause unreliable buttons and/or SPI. Change pins."
#endif

#if (PIN_BTN_LEFT == PIN_TFT_MOSI) || (PIN_BTN_RIGHT == PIN_TFT_MOSI) || (PIN_BTN_CENTER == PIN_TFT_MOSI)
#warning "A button pin overlaps PIN_TFT_MOSI; this will cause unreliable buttons and/or SPI. Change pins."
#endif

#if (PIN_BTN_LEFT == PIN_TFT_CS) || (PIN_BTN_RIGHT == PIN_TFT_CS) || (PIN_BTN_CENTER == PIN_TFT_CS)
#warning "A button pin overlaps PIN_TFT_CS; this will cause unreliable buttons and/or SPI. Change pins."
#endif

#if (PIN_BTN_LEFT == PIN_TFT_DC) || (PIN_BTN_RIGHT == PIN_TFT_DC) || (PIN_BTN_CENTER == PIN_TFT_DC)
#warning "A button pin overlaps PIN_TFT_DC; this will cause unreliable buttons and/or SPI. Change pins."
#endif

#if (PIN_BTN_LEFT == PIN_TFT_RST) || (PIN_BTN_RIGHT == PIN_TFT_RST) || (PIN_BTN_CENTER == PIN_TFT_RST)
#warning "A button pin overlaps PIN_TFT_RST; this will cause unreliable buttons and/or SPI. Change pins."
#endif

// If your TFT doesn't use MISO, keep this at -1.
#ifndef PIN_TFT_MISO
#define PIN_TFT_MISO -1
#endif

// Panel settings (ST7789 variants differ; match your panel)
#ifndef TFT_WIDTH
#define TFT_WIDTH 240
#endif

#ifndef TFT_HEIGHT
#define TFT_HEIGHT 280
#endif

#ifndef TFT_ROTATION
#define TFT_ROTATION 1
#endif

#ifndef TFT_SPI_HZ
#define TFT_SPI_HZ 40000000
#endif

// ============================================================================
// Performance Debug Macros
// ============================================================================

// HELGA_PERF_START(label) - Start timing a code block
// HELGA_PERF_END(label) - End timing and log duration
// HELGA_PERF_MARK(label) - Log a timestamp marker
//
// These macros help identify performance bottlenecks. They only output when
// settings.debugLevel == 3 (Performance mode).
//
// Usage:
//   HELGA_PERF_START("ble_scan");
//   // ... code to profile ...
//   HELGA_PERF_END("ble_scan");

#define HELGA_PERF_START(label) uint32_t _perf_t0_##label = micros()

#define HELGA_PERF_END(label) \
  do { \
    uint32_t _perf_dur = micros() - _perf_t0_##label; \
    Serial.printf("[PERF] %s: %lu us\n", #label, (unsigned long)_perf_dur); \
  } while(0)

#define HELGA_PERF_MARK(label) \
  do { \
    Serial.printf("[PERF] %s @ %lu ms\n", label, (unsigned long)millis()); \
  } while(0)

// Conditional versions that check settings.debugLevel == 3
// NOTE: These require a CoreState& or Settings& in scope named 'state' or 'settings'
#define HELGA_PERF_START_IF(state, label) \
  uint32_t _perf_t0_##label = 0; \
  if ((state).settings.debugLevel == 3) { \
    _perf_t0_##label = micros(); \
  }

#define HELGA_PERF_END_IF(state, label) \
  do { \
    if ((state).settings.debugLevel == 3 && _perf_t0_##label > 0) { \
      uint32_t _perf_dur = micros() - _perf_t0_##label; \
      if (_perf_dur >= 50000) { \
        Serial.printf("[PERF] %s: %lu us\n", #label, (unsigned long)_perf_dur); \
      } \
    } \
  } while(0)

#define HELGA_PERF_MARK_IF(state, label) \
  do { \
    if ((state).settings.debugLevel == 3) { \
      Serial.printf("[PERF] %s @ %lu ms\n", label, (unsigned long)millis()); \
    } \
  } while(0)
