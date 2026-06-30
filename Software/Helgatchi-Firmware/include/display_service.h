#pragma once
#include "event_bus.h"

class DisplayService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

    // Rebuilds the status-bar status_icons string (BT / WiFi / Bell). Called
    // from AlertsScreen when alerts come and go, since the bell glyph
    // depends on g_alerts.count(). DisplayService also calls it internally
    // on relevant settings changes.
    void refreshStatusIcons();

private:
    EventBus* _bus          = nullptr;
    uint16_t  _last_batt_mv  = 0;
    uint8_t   _last_batt_pct = 0xFF;
};

extern DisplayService g_display;
