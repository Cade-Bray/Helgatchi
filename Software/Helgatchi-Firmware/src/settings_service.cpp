#include "settings_service.h"
#include <Preferences.h>

SettingsService g_settings;

// NVS namespace and key format — "k00".."k16", one uint32 per setting.
static constexpr char NVS_NS[]      = "helgatchi";
static constexpr char NVS_VER_KEY[] = "ver";

// Maps each SettingsKey to the SMASK_* bits it affects.
// Must stay in sync with the SettingsKey enum order.
static constexpr uint32_t s_key_mask[SKEY_COUNT] = {
    SMASK_UI,                   // SKEY_SCREEN_BRIGHTNESS
    SMASK_UI,                   // SKEY_LED_BRIGHTNESS
    SMASK_SCAN,                 // SKEY_SCAN_MODE
    SMASK_SCAN | SMASK_POWER,   // SKEY_PERF_MODE
    SMASK_ALERT,                // SKEY_ALERT_WAKE_SCREEN
    SMASK_ALERT,                // SKEY_ALERT_VIBRATION
    SMASK_ALERT,                // SKEY_ALERT_LED
    SMASK_POWER,                // SKEY_SLEEP_WHILE_USB
    SMASK_DEBUG,                // SKEY_DEBUG_SERIAL_ENABLED
    SMASK_DEBUG,                // SKEY_DEBUG_LEVEL
    SMASK_DEBUG | SMASK_POWER,  // SKEY_DEBUG_SLEEP_WITH_SERIAL
    SMASK_POWER | SMASK_UI,     // SKEY_SCREEN_TIMEOUT_S
    SMASK_POWER,                // SKEY_INTERACTIVE_TIMEOUT_S
    SMASK_POWER,                // SKEY_WAKE_DURATION_S
    SMASK_SCAN,                 // SKEY_SCAN_DURATION_S
    SMASK_SCAN,                 // SKEY_BLE_SCAN_WINDOW_MS
    SMASK_SCAN,                 // SKEY_BLE_SCAN_INTERVAL_MS
    SMASK_SCAN,                 // SKEY_WIFI_DWELL_MS
    SMASK_SCAN,                 // SKEY_WIFI_HOP_INTERVAL_MS
};

// ---------------------------------------------------------------------------

void SettingsService::begin(EventBus& bus) {
    _bus = &bus;
    _applyDefaults();
    _load();

    bus.subscribe(CMD_SETTINGS_SET,            this);
    bus.subscribe(CMD_SETTINGS_SAVE,           this);
    bus.subscribe(CMD_SETTINGS_RESET_DEFAULTS, this);
}

uint32_t SettingsService::get(SettingsKey key) const {
    if (key >= SKEY_COUNT) return 0;
    return _values[key];
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void SettingsService::onEvent(const Event& e) {
    switch (e.id) {
        case CMD_SETTINGS_SET: {
            auto key = static_cast<SettingsKey>(e.data.settings_set.key);
            uint32_t mask = 0;
            _set(key, e.data.settings_set.value, mask);
            if (mask) {
                // Persist immediately so changes survive reboot. _set() already
                // skips no-op writes, and Preferences dedupes per-key, so this
                // only touches NVS when something actually changed. If a future
                // slider widget streams VALUE_CHANGED on every drag step, swap
                // this for a debounced save (mark dirty, flush from a tick).
                _save();
                _emitChanged(mask);
            }
            break;
        }
        case CMD_SETTINGS_SAVE:
            _save();
            break;

        case CMD_SETTINGS_RESET_DEFAULTS:
            _applyDefaults();
            _save();
            _emitChanged(SMASK_ALL);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void SettingsService::_applyDefaults() {
    _values[SKEY_SCREEN_BRIGHTNESS]       = DEFAULT_SCREEN_BRIGHTNESS;
    _values[SKEY_LED_BRIGHTNESS]          = DEFAULT_LED_BRIGHTNESS;
    _values[SKEY_SCAN_MODE]               = DEFAULT_SCAN_MODE;
    _values[SKEY_PERF_MODE]               = DEFAULT_PERF_MODE;
    _values[SKEY_ALERT_WAKE_SCREEN]       = DEFAULT_ALERT_WAKE_SCREEN;
    _values[SKEY_ALERT_VIBRATION]         = DEFAULT_ALERT_VIBRATION;
    _values[SKEY_ALERT_LED]               = DEFAULT_ALERT_LED;
    _values[SKEY_SLEEP_WHILE_USB]         = DEFAULT_SLEEP_WHILE_USB;
    _values[SKEY_DEBUG_SERIAL_ENABLED]    = DEFAULT_DEBUG_SERIAL;
    _values[SKEY_DEBUG_LEVEL]             = DEFAULT_DEBUG_LEVEL;
    _values[SKEY_DEBUG_SLEEP_WITH_SERIAL] = DEFAULT_SLEEP_WITH_SERIAL;

    uint32_t dummy = 0;
    _applyPerfPreset(static_cast<PerfMode>(DEFAULT_PERF_MODE), dummy);
}

void SettingsService::_applyPerfPreset(PerfMode mode, uint32_t& mask_out) {
    if (mode == PERF_DYNAMIC) return; // dynamic values are managed at runtime by Power Manager

    const PerfPreset& p = PERF_PRESETS[mode];
    _values[SKEY_BLE_SCAN_WINDOW_MS]      = p.ble_scan_window_ms;
    _values[SKEY_BLE_SCAN_INTERVAL_MS]   = p.ble_scan_interval_ms;
    _values[SKEY_WIFI_DWELL_MS]          = p.wifi_dwell_ms;
    _values[SKEY_WIFI_HOP_INTERVAL_MS]   = p.wifi_hop_interval_ms;
    _values[SKEY_SCAN_DURATION_S]        = p.scan_duration_s;
    _values[SKEY_WAKE_DURATION_S]        = p.wake_duration_s;
    _values[SKEY_SCREEN_TIMEOUT_S]       = p.screen_timeout_s;
    _values[SKEY_INTERACTIVE_TIMEOUT_S]  = p.interactive_timeout_s;

    mask_out |= SMASK_SCAN | SMASK_POWER | SMASK_UI;
}

void SettingsService::_set(SettingsKey key, uint32_t value, uint32_t& mask_out) {
    if (key >= SKEY_COUNT) return;
    if (_values[key] == value) return;

    _values[key] = value;
    mask_out |= s_key_mask[key];

    if (key == SKEY_PERF_MODE) {
        _applyPerfPreset(static_cast<PerfMode>(value), mask_out);
    }
}

void SettingsService::_load() {
    Preferences prefs;
    prefs.begin(NVS_NS, /*readOnly=*/true);

    uint16_t stored_ver = prefs.getUShort(NVS_VER_KEY, 0);
    if (stored_ver != SCHEMA_VERSION) {
        prefs.end();
        // Schema mismatch — defaults already applied in begin(); persist them now.
        _save();
        return;
    }

    char key[5];
    for (uint8_t i = 0; i < SKEY_COUNT; i++) {
        snprintf(key, sizeof(key), "k%02u", i);
        _values[i] = prefs.getUInt(key, _values[i]);
    }
    prefs.end();
}

void SettingsService::_save() {
    Preferences prefs;
    prefs.begin(NVS_NS, /*readOnly=*/false);
    prefs.putUShort(NVS_VER_KEY, SCHEMA_VERSION);

    char key[5];
    for (uint8_t i = 0; i < SKEY_COUNT; i++) {
        snprintf(key, sizeof(key), "k%02u", i);
        prefs.putUInt(key, _values[i]);
    }
    prefs.end();
}

void SettingsService::_emitChanged(uint32_t mask) {
    EventPayload p{};
    p.settings.mask    = mask;
    p.settings.version = ++_change_seq;
    _bus->post(EV_SETTINGS_CHANGED, p);
}
