#pragma once
#include "event_bus.h"

class RulesScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;
};

extern RulesScreen g_rules_screen;
