#pragma once
#include "event_bus.h"

// AlertsScreen
//
// Owns the UI side of alerts:
//   * Dynamic alert card list rendered into objects.alert_container
//   * "No active alerts" placeholder label visibility
//   * Dismiss-button fade-out animation mirroring the EEZ "Fade and Hide
//     Alert" user action
//   * Keypad nav group population for dismiss buttons on screen load
//   * Triggers the status-bar bell refresh in DisplayService
//
// Layered like SettingsScreen: AlertsService is the data store (LVGL-free);
// this is the presentation layer for one screen. Must be initialized AFTER
// g_ui (so EEZ objects.* exist) and AFTER g_alerts (so the restored alerts
// list from RTC memory is available for initial rendering).

class AlertsScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;
};

extern AlertsScreen g_alerts_screen;
