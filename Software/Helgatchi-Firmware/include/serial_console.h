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

    // Multi-subcommand verbs. Each prints its own usage when called with no
    // args; routes to subcommands otherwise.
    void _cmdSetting(char* args);   // list / set / save / reset
    void _cmdAlert(char* args);     // list / raise / ack / clear
    void _cmdLed(char* args);       // list / play / off / bright
    void _cmdVibe(char* args);      // list / play / off
    void _cmdRule(char* args);      // list / show / create / add / rm / delete / enable / disable / reload / stats
    void _cmdScan(char* args);      // list / inject / clear
    void _cmdVendor(char* args);    // stats / oui / mfg / search
    void _cmdPower(char* args);     // sleep / sleepscreen / reboot / shipping

    // Singletons (no subcommands).
    void _cmdBus(char* args);
    void _cmdStats();
    void _cmdBattery();
    void _cmdSelftest();

    EventBus* _bus           = nullptr;
    char      _buf[BUF_LEN];
    uint8_t   _pos           = 0;
    bool      _was_connected = false;
};

extern SerialConsole g_console;
