#pragma once

#include <stdint.h>

#include "../core/EventBus.h"
#include "../core/Events.h"

// Producer-only: performs BLE scanning and emits BleSighting events.
// Implementation is in BleScanner.cpp and uses a thread-safe internal queue;
// events are only pushed to EventBus from poll() (main loop context).
class BleScanner {
public:
	void begin();
	void start();
	void stop();
	bool running() const { return running_; }

	void poll(EventBus& bus);
	
	static void setDebugPerformance(bool enabled);
	static bool debugPerformance();

private:
	bool running_ = false;
};
