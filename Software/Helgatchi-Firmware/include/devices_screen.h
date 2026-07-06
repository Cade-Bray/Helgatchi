#pragma once
#include "event_bus.h"

// DevicesScreen
//
// UI side of the scanned-device list. Renders one card per unique device in
// ScanService's dedup'd seen-map into objects.devices_container, refreshed on
// EV_SCAN_COMPLETE (posted by PowerManager once a scan window's backlog has
// fully drained, so the seen-map is complete before we read it).
//
// Cards are diffed by (domain, MAC) across refreshes: matched cards update
// text in place, new devices get a fresh card, evicted ones are deleted, and
// the list is reordered RSSI-strongest-first. Keeping card objects alive
// across refreshes preserves keypad focus + scroll position while browsing.
// A 1 s timer live-ticks only the "Ns ago" age; RSSI/MFG stay frozen between
// scan windows.
//
// Layered like AlertsScreen: ScanService is the LVGL-free data store; this is
// the presentation layer. Initialize AFTER g_ui (objects.* must exist) and
// AFTER g_scan.

class DevicesScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;
};

extern DevicesScreen g_devices_screen;
