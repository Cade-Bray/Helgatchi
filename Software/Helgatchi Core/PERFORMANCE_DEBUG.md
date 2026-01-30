# Performance Debug Mode

## Overview

The Helgatchi Core firmware now includes a dedicated **Performance Debug Mode** to help identify performance bottlenecks and slow points in the codebase.

## Enabling Performance Debug Mode

Performance debug mode is controlled by the `debugPerformance` setting in the `Settings` structure:

```cpp
// In CoreState.h
struct Settings {
  // ... other settings ...
  
  // Performance debug mode: 0=Off, 1=On (timing/profiling info)
  uint8_t debugPerformance = 0;
};
```

### To Enable:

1. **Via Settings UI**: Navigate to Main Menu → Debug Options → Debug level → set to "Performance"
2. **Via Code**: Set `state.settings.debugLevel = 3` and trigger a `SettingsChanged` event
3. **Default at Boot**: Change the default value in `CoreState.h`

Performance mode is the 4th debug level:
- `debugLevel = 0`: Off (no debug output)
- `debugLevel = 1`: Low (basic events)
- `debugLevel = 2`: High (verbose logging)
- `debugLevel = 3`: Performance (timing/profiling data)

## What Gets Logged

When performance debug mode is enabled, timing information is logged over serial for:

### Core System Operations
- **Setup timing**: Subsystem initialization, settings/rules loading
- **Loop timing**: Total loop time, individual subsystem polls
- **Event processing**: Event bus draining and individual event handling
- **UI rendering**: Display update timing

### BLE Scanner Operations
- **Callback processing**: Total time in BLE advertisement callbacks
- **Address parsing**: MAC/OUI extraction
- **Name parsing**: Device name extraction
- **Manufacturer data**: Company ID parsing
- **Service UUIDs**: Service UUID extraction

### Display Operations
- **Display initialization**: Screen setup time
- **Clear screen**: Full screen clear operations
- **Rectangle fills**: `fillRect()` timing
- **Text rendering**: `drawText()` and `drawTextColored()` timing

### Rules Matching
- **OUI matching**: Time to match WiFi OUI rules
- **BLE matching**: Comprehensive BLE rule matching including:
  - MAC address matching
  - Service UUID matching
  - Company ID matching
  - Name substring matching
  - OUI fallback matching

## Performance Log Format

All performance logs are prefixed with `[PERF]` and show timing in microseconds (µs):

```
[PERF] setup_total: 1234567 us
[PERF] loop_total: 8234 us
[PERF] ble_callback_total: 456 us
[PERF] display_drawText(10,20,sz=2): 1234 us
[PERF] matchBle(mac_pack): 234 us
```

## Using the Macros

The system provides several performance macros in [Config.h](src/core/Config.h):

### Basic Macros (Always Active)
```cpp
PERF_START(label);
// ... code to profile ...
PERF_END(label);

PERF_MARK("checkpoint_name");  // Timestamp marker
```

### Conditional Macros (Require CoreState)
```cpp
PERF_START_IF(state, label);
// ... code to profile ...
PERF_END_IF(state, label);

PERF_MARK_IF(state, "checkpoint");
```

### Module-Specific Control

Each major module has its own static performance flag:

```cpp
BleScanner::setDebugPerformance(bool enabled);
Display::setDebugPerformance(bool enabled);
RulesManager::setDebugPerformance(bool enabled);
```

These are automatically synced with `state.settings.debugPerformance` in Core.cpp.

## Typical Workflow

1. **Enable performance mode** via settings
2. **Monitor serial output** at 115200 baud
3. **Look for high timing values** in critical paths:
   - Loop times > 50ms indicate main loop blocking
   - BLE callbacks > 10ms indicate parsing issues
   - Rule matching > 5ms indicates too many rules or inefficient matching
   - Display operations > 20ms indicate SPI/display issues
4. **Identify bottlenecks** and optimize accordingly
5. **Disable performance mode** in production to reduce serial spam

## Performance Tips

- **BLE parsing**: Service UUID parsing is often slow - limit rules with UUIDs
- **Display rendering**: Text rendering can be slow at large sizes - minimize redraws
- **Rule matching**: Large rule lists (>100 rules) can slow matching - consider optimization
- **Event processing**: Process events in batches to avoid overhead

## Notes

- Performance logging adds ~10-50µs overhead per measurement
- Serial output itself can impact timing (slow UART)
- Measurements are in microseconds (1000µs = 1ms)
- Use this mode during development/debugging, not in production
- Disable to reduce serial noise and improve battery life
