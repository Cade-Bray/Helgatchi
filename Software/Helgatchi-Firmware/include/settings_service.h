#pragma once
#include "event_bus.h"
#include "settings_keys.h"

class SettingsService : public IEventHandler {
public:
    // Incremented any time the NVS schema layout changes (forces defaults on next boot).
    static constexpr uint16_t SCHEMA_VERSION = 6;  // bumped: NO_SLEEP_WHILE_CHARGING → SLEEP_WHILE_USB (polarity flipped)

    // Load NVS, apply defaults if schema mismatch, subscribe to commands.
    void begin(EventBus& bus);

    // Read any setting by key. Thread-safe (values are word-aligned atomics on ESP32).
    uint32_t get(SettingsKey key) const;

    // Convenience casts
    bool     getBool(SettingsKey key) const { return get(key) != 0; }
    uint16_t getU16(SettingsKey key)  const { return static_cast<uint16_t>(get(key)); }

    // IEventHandler — handles CMD_SETTINGS_SET / SAVE / RESET_DEFAULTS
    void onEvent(const Event& e) override;

private:
    void _applyDefaults();
    void _applyPerfPreset(PerfMode mode, uint32_t& mask_out);
    void _set(SettingsKey key, uint32_t value, uint32_t& mask_out);
    void _load();
    void _save();
    void _emitChanged(uint32_t mask);

    uint32_t  _values[SKEY_COUNT] = {};
    EventBus* _bus                = nullptr;
    uint16_t  _change_seq         = 0;     // increments each EV_SETTINGS_CHANGED emission
};

extern SettingsService g_settings;
