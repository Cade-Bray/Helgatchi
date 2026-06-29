#pragma once
#include "event_bus.h"

class DisplayService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

private:
    EventBus* _bus          = nullptr;
    uint16_t  _last_batt_mv  = 0;
    uint8_t   _last_batt_pct = 0xFF;
};

extern DisplayService g_display;
