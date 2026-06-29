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
    void _cmdSettings(char* args);
    void _cmdBus(char* args);
    void _cmdStats();
    void _cmdLed(char* args);
    void _cmdVibe(char* args);
    void _cmdBattery();
    void _cmdSelftest();

    EventBus* _bus           = nullptr;
    char      _buf[BUF_LEN];
    uint8_t   _pos           = 0;
    bool      _was_connected = false;
};

extern SerialConsole g_console;
