#pragma once
#include "event_bus.h"

class SettingsScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;
};

extern SettingsScreen g_settings_screen;
