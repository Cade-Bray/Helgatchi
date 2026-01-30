
#pragma once

#include <Arduino.h>

#include "../core/CoreState.h"
#include "../core/EventBus.h"

#include "WiFiScanner.h"
#include "BleScanner.h"

class ScanCoordinator {
public:
  void setDebug(bool enabled) { debug_ = enabled; }

  void begin() {
	wifi_.begin();
	ble_.begin();
  }

  void poll(EventBus& bus) {
	wifi_.poll(bus);
	ble_.poll(bus);
  }

  void setModeInteractive(const Settings&) {
    burstActive_ = false;
	if (debug_) Serial.println("[scan] mode=interactive");
	wifi_.start();
	ble_.start();
  }

  void setModeIdle(const Settings&) {
    burstActive_ = false;
  if (debug_) Serial.println("[scan] mode=idle");
	// TODO: implement reduced duty cycling; for now stop scanning.
	wifi_.stop();
	ble_.stop();
  }

  void startBurst(uint32_t ms, const Settings&) {
    burstActive_ = true;
    burstEndMs_ = millis() + ms;
  if (debug_) {
    Serial.print("[scan] burst start ms=");
    Serial.println((unsigned long)ms);
  }
	wifi_.start();
	ble_.start();
  }

  bool burstComplete() const {
    return burstActive_ && (millis() >= burstEndMs_);
  }

  void stopAll() {
    burstActive_ = false;
	if (debug_) Serial.println("[scan] stopAll");
	wifi_.stop();
	ble_.stop();
  }

private:
	WiFiScanner wifi_{};
	BleScanner ble_{};
  bool debug_ = false;
  bool burstActive_ = false;
  uint32_t burstEndMs_ = 0;
};
