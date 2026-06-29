#pragma once
#include "event_bus.h"

class DisplayService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

private:
    void _drawAll();
    void _drawBattery();
    void _drawPowerState();
    void _drawCountdown();
    void _drawRightColumn();

    EventBus* _bus         = nullptr;
    uint16_t  _batt_mv     = 0;
    uint8_t   _batt_pct    = 0;
    uint8_t   _pwr_state   = 0;
    uint16_t  _countdown_s = 0;

    bool _usb_attached          = false;
    bool _serial_connected      = false;
    bool _sleep_while_usb       = false;  // mirrors SKEY_SLEEP_WHILE_USB (true = allow)
    bool _no_sleep_while_serial = true;
};

extern DisplayService g_display;
