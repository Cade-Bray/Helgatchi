#pragma once
#include "event_bus.h"

class SerialConsole {
public:
    void begin(EventBus& bus);
    void tick();   // call every loop()

private:
    static constexpr uint8_t BUF_LEN = 128;

    void _dispatch(char* line);
    void _cmdHelp();
    void _cmdSetting(char* args);   // singular: set one setting
    void _cmdSettings(char* args);  // plural: list / save / reset
    void _cmdBus(char* args);
    void _cmdStats();
    void _cmdLed(char* args);       // singular: play one pattern
    void _cmdLeds(char* args);      // plural: list / off / brightness
    void _cmdVibe(char* args);
    void _cmdBattery();
    void _cmdSelftest();
    void _cmdAlert(char* args);    // singular: create one alert
    void _cmdAlerts(char* args);   // plural: list/ack/clear existing alerts
    void _cmdScan(char* args);     // list seen devices, inject test results, clear
    void _cmdVendor(char* args);   // OUI / mfg-id lookup against IEEE + BT SIG tables
    void _cmdRule(char* args);     // singular: create one rule / add criteria
    void _cmdRules(char* args);    // plural: list / show / enable / disable / stats

    EventBus* _bus           = nullptr;
    char      _buf[BUF_LEN];
    uint8_t   _pos           = 0;
    bool      _was_connected = false;
};

extern SerialConsole g_console;
