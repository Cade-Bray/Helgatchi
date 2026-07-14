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

    // Party mode: tint every top-bar glyph (left icons + right battery/prefix)
    // a single colour instead of their status colours, and repaint. Party drives
    // setIconTint() on a hue cycle each frame; clearIconTint() restores normal
    // status colouring. The tint is also honoured by the internal event-driven
    // refreshes so a battery/scan update mid-party doesn't flash back to normal.
    void setIconTint(uint32_t rgb);
    void clearIconTint();

private:
    EventBus* _bus           = nullptr;
    uint16_t  _last_batt_mv  = 0;
    uint8_t   _last_batt_pct = 0xFF;
    bool      _ble_scanning  = false;   // BT icon blue while true  (EV_SCAN_STATE_CHANGED, SCAN_BLE)
    bool      _wifi_scanning = false;   // WiFi icon blue while true (EV_SCAN_STATE_CHANGED, SCAN_WIFI)
};

extern DisplayService g_display;
