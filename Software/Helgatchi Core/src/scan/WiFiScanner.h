#pragma once

#include <stdint.h>

#include "../core/EventBus.h"
#include "../core/Events.h"

// Producer-only: performs Wi-Fi scanning and emits WifiSighting events.
// This is currently a stub (no real scanning yet).
class WiFiScanner {
public:
	void begin() {}
	void start() { running_ = true; }
	void stop() { running_ = false; }
	bool running() const { return running_; }

	void poll(EventBus& bus) {
		(void)bus;
		// TODO: implement promiscuous scan or active scan and enqueue sightings.
	}

private:
	bool running_ = false;
};
