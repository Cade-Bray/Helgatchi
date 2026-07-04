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

    // --- Improv Serial (esp-web-tools device identify) --------------------
    // esp-web-tools sends binary "IMPROV" frames on connect to read the
    // firmware name. When that name matches the flasher manifest's, it treats
    // the flash as an update and skips its erase prompt. We sniff those frames
    // off the same CDC stream; any byte that isn't part of a valid frame falls
    // through to the text console via _consoleByte.
    void _consoleByte(char c);              // per-byte line editor
    bool _improvFeed(uint8_t b);            // true = byte handled as Improv
    void _improvReplayAndReset(uint8_t current);
    void _improvHandleFrame();
    void _improvSend(uint8_t type, const uint8_t* data, uint8_t len);
    void _improvSendCurrentState();
    void _improvSendDeviceInfo();
    void _cmdImprov();                      // TEMP: dump improv debug counters

    // Max frame: 6 magic + version + type + len + 255 data + checksum = 264.
    static constexpr uint16_t IMPROV_MAX = 272;

    EventBus* _bus                       = nullptr;
    char      _buf[BUF_LEN];
    uint8_t   _pos                       = 0;
    bool      _was_connected             = false;
    uint32_t  _last_seen_connected_ms    = 0;   // hysteresis against CDC `bool Serial` blips

    uint8_t   _improv_buf[IMPROV_MAX];
    uint16_t  _improv_len                = 0;   // bytes buffered in the current frame
    bool      _improv_swallow_nl         = false;  // eat the '\n' trailing a frame

    // TEMP debug counters, dumped by the `improv` console command.
    uint32_t  _improv_frames             = 0;
    uint8_t   _improv_last_type          = 0xFF;
    uint8_t   _improv_last_cmd           = 0xFF;
    int16_t   _improv_last_tx            = -1;   // bytes Serial.write returned
    uint8_t   _improv_last_txn           = 0;    // bytes we asked it to write
    bool      _improv_last_conn          = false; // (bool)Serial AT reply time
    uint32_t  _improv_last_ms            = 0;     // millis() when we replied
};

extern SerialConsole g_console;
