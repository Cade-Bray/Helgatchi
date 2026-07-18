#pragma once
#include "event_bus.h"

// Power Menu screen wiring — mirrors SettingsScreen. Routes its three buttons
// to PowerManager commands and reflects the live sleep countdown in the
// on-screen text (same EV_SLEEP_COUNTDOWN_UPDATED source the Settings screen
// uses).
class PowerMenuScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;
};

extern PowerMenuScreen g_power_menu_screen;
