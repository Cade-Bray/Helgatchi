#include <Arduino.h>
#include "core/Core.h"

Core core_app;

void setup() {
  Serial.begin(115200);

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
  core_app.loop();
}