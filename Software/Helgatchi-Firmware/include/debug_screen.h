#pragma once
#include "event_bus.h"

// DebugScreen
//
// Read-only diagnostics view (SCREEN_ID_DEBUG_INFO). Fills the four value
// columns EEZ laid out — System & Health, Power, Scanning, Rules & Alerts —
// by polling the live services once a second while the screen is visible.
//
// Layered like SettingsScreen / AlertsScreen: EEZ owns the visuals, C owns the
// data. Each value label is the right-hand child (index 1) of its EEZ
// container: system___health_container / power_container / scanning_container /
// rules___alerts_container.
//
// Must be initialized AFTER g_ui.begin() so the objects.* handles exist.

class DebugScreen {
public:
    void begin(EventBus& bus);
};

extern DebugScreen g_debug_screen;
