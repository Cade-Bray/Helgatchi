#pragma once

#include <Arduino.h>

#include "../core/Events.h"

// Minimal telemetry sink.
// Today: just logs to Serial so we can debug event flow.
// Later: can be replaced with BLE/WiFi upload, SD logging, etc.
class Telemetry {
public:
  void setEnabled(bool enabled) { enabled_ = enabled; }

  void begin() {
    // no-op
  }

  void onWifiSighting(const WifiSightingEvent& e) {
    if (!enabled_) return;
    Serial.print("[telemetry] wifi oui=");
    Serial.print(e.oui24, HEX);
    Serial.print(" rssi=");
    Serial.println((int)e.rssi);
  }

  void onBleSighting(const BleSightingEvent& e) {
    if (!enabled_) return;
    Serial.print("[telemetry] ble oui=");
    Serial.print(e.oui24, HEX);
    Serial.print(" rssi=");
    Serial.println((int)e.rssi);
  }

  void onAlertFired(const AlertFiredEvent& e) {
    if (!enabled_) return;
    Serial.print("[telemetry] alert rule=");
    Serial.print(e.ruleId);
    Serial.print(" rssi=");
    Serial.println((int)e.rssi);
  }

private:
  bool enabled_ = false;
};
