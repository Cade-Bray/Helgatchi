#include <Arduino.h>
#include "core/Core.h"

Core core_app;

static uint32_t g_lastHeartbeatMs = 0;

void setup() {
  Serial.begin(115200);

  // Give the USB CDC serial monitor time to attach after reset.
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
    delay(10);
  }

  Serial.println();
  Serial.println("[main] boot");

  delay(200);
  core_app.setup();
}

void loop() {
  // If setup messages were missed, this ensures *something* shows in the monitor.
  core_app.loop();
}