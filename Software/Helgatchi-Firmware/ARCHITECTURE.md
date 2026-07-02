# Architecture

This document describes the firmware as it actually exists. The earlier
revision was design-only; this one tracks what's been built, the conventions
in use, and the footguns that have already cost real time.

---

## Big picture

```
                                 +---------------------+
        physical buttons ------> |                     |
        USB SOF / VSENSE  ------>|     HAL (g_hal)     |---> LCD (LovyanGFX)
        backlight / vibe / LEDs <|                     |---> WS2812 LEDs
                                 +---------------------+
                                            |
                                       posts EV_BTN_*, EV_UI_ACTIVITY
                                            v
                              +---------------------------+
                              |   EventBus (g_bus)        |
                              |   bounded queue, sub fan  |
                              +---------------------------+
                                /  |  |  |  |  |  |  |  \
                               v   v  v  v  v  v  v  v   v
                            settings, log, console, power, leds, vibe, ui, display

                          (each service: begin(), tick(), onEvent())
```

Every service has the same shape: a singleton `g_*`, a `begin(EventBus&)` that
subscribes and seeds state, a `tick()` called from `loop()`, and `onEvent()`
implementing `IEventHandler`. Exceptions are HAL (no event subscriptions to
the bus by default — it just exposes hardware) and SettingsService (storage).

---

## Services that exist today

### Implemented and stable
| Service | File | Role |
|---|---|---|
| `g_bus` | `event_bus.{h,cpp}` | Bounded FIFO + fan-out subscriber list. Mutex-protected. Tracks dispatch perf counters used by LogService. |
| `g_settings` | `settings_service.{h,cpp}` | NVS-backed key/value. **Auto-saves on every `CMD_SETTINGS_SET`.** Schema-versioned — bump `SCHEMA_VERSION` whenever a key's storage format or polarity changes; old NVS gets reset to defaults on next boot. |
| `g_hal` | `hal.{h,cpp}` | LCD (LovyanGFX), backlight (LEDC ch0), vibration motor (LEDC ch1, 20 kHz), WS2812 chain (FastLED), button polling, USB SOF detection, VSENSE ADC. |
| `g_logger` | `log_service.{h,cpp}` | Subscribes to all events, filters by `DebugLevel`. Hides/shows LVGL FPS overlay when level crosses RENDERING_PERF threshold. |
| `g_console` | `serial_console.{h,cpp}` | Line-based serial CLI. Commands: `settings`, `bus`, `stats`, `led`, `vibe`, `reboot`. Each character posts `EV_UI_ACTIVITY`. |
| `g_power` | `power_manager.{h,cpp}` | Sleep/wake decisions, battery sampling, scan/idle window timing, display state machine, shipping-mode RTC flag. |
| `g_leds` | `led_service.{h,cpp}` | LED pattern catalog (charging pulse, serial gradient, alert strobes). 30 FPS render with frame skip. Two-layer (ambient + alert) with 500ms fade. |
| `g_vibe` | `vibe_service.{h,cpp}` | Haptic pattern catalog. Step machine of `{intensity, duration_ms}` pairs. PWM via LEDC. Subscribed directly to `EV_BTN_*` so every button press fires a tick. |
| `g_ui` | `ui_controller.{h,cpp}` | Wraps LVGL: display init, keypad indev, focus groups, settings widget binding table. Calls `ui_init()` from SquareLine-generated code. |
| `g_display` | `display_service.{h,cpp}` | Debug status display (renders directly via LovyanGFX, separate from LVGL UI). Mostly historical. |

### Deferred / not yet implemented
- **Scanner Service** — BLE/WiFi scanning. Will subscribe to `CMD_SCAN_*`, post `EV_OBS_BATCH_READY`.
- **Parser / Enrichment / Entity Store** — observation pipeline.
- **Rule Engine** — pattern matching against entities.
- **Alert Manager** — owns alert lifecycle (raise/ack/snooze/clear).
- **Device Bridge (Public Mesh)** — outbound rule-fired broadcasts.
- **AppState** — central reducer for UI rendering. Currently the UI reads from `g_settings` and listens to events directly; AppState was deferred until there are more things to render.

The original architecture doc described all of these; they're stubs at most. Event IDs (`CMD_SCAN_*`, `EV_ENTITY_UPDATED`, `EV_RULE_TRIGGERED_LOCAL`, etc.) are defined in `include/event_ids.h` so the eventual services can be added without renaming.

---

## Event bus rules

- **Commands (`CMD_*`)** — imperative requests. Producers fire-and-forget; consumers may ignore.
- **Events (`EV_*`)** — facts about something that happened. Multiple subscribers OK.
- **Payloads** — fixed-size `EventPayload` union (`include/event_payload.h`). No heap, no pointers to externally-owned memory.
- **Direct service-to-service calls** are allowed only for read-only queries (e.g. `g_settings.getBool(...)`, `g_hal.usbAttached()`) and for hardware control where the bus would add too much latency (e.g. `g_vibe.play()` from UI for tactile feedback, `g_hal.writeLEDFrame()` from LedService).
- **Auto-save on `CMD_SETTINGS_SET`** — SettingsService persists to NVS on every value change. Currently fine because all bound widgets are switches/dropdowns (settle once per user action). If a slider gets bound, add a debounce — `lv_slider`'s `LV_EVENT_VALUE_CHANGED` fires per drag step.

---

## UI: built in SquareLine Studio, wired in C

The screens are designed in **SquareLine Studio** (`Helgatchi-UI` SLS project, version 1.6.0, targeting LVGL 9.3 + Arduino+TFT_eSPI export). Exported files land in `src/UI/`. Things to know:

- **Don't edit `src/UI/*` by hand** — re-export overwrites them. All custom logic lives in `ui_controller.cpp`.
- **`ui_init()`** (in `src/UI/ui.c`) builds all screens and loads the splash. UIController calls it from `begin()`.
- **Lowercase names** — the project is set to "Force exported names to lower case", so widgets are `ui_<screen>_<type>_<name>` all lowercase.
- **Top bar** is a Component (purple in the SLS hierarchy), instanced on Status / Menu / Settings. Three labels per instance: left status, centre title, right battery %. The right label is updated by `_refreshBatteryLabels()` on `EV_BATTERY_UPDATED`.
- **Settings widgets are bound table-driven**: `_bindings[]` in `ui_controller.cpp` maps each SLS widget pointer to a `SettingsKey`. A polymorphic `_readWidget` / `_writeWidget` handles switch / dropdown / slider / arc / roller / checkbox. To add a new setting widget: one line in the table (assuming it maps 1:1 to an SKEY).
- **Custom (non-1:1) bindings**: BLE+WiFi switches combine into `SKEY_SCAN_MODE` via bit-OR, wired directly. Same shape works for any future "two switches → one bit-packed setting" case.
- **Action buttons** use a parallel `_actions[]` table mapping widget → `EventId`. The Reboot button uses a custom handler instead (calls `ESP.restart()` after stopping the motor).

### Keypad input convention
- Physical buttons → `EV_BTN_*` → UIController converts to LVGL keys via a small queue (press/release pair per button event).
- **Navigation mode** (default): `LEFT` / `RIGHT` send `LV_KEY_PREV` / `LV_KEY_NEXT` to walk the focus group. **Don't send `LV_KEY_LEFT/RIGHT` for navigation** — focused switches interpret those as direct toggle and you'll never get past the first one.
- **Edit mode** (after `LV_KEY_ENTER` on a slider/dropdown): `LEFT` / `RIGHT` send `LV_KEY_LEFT/RIGHT` to the focused widget.
- **Dropdown open** (`lv_dropdown_is_open(focused)` returns true): `LEFT` / `RIGHT` send `LV_KEY_UP/DOWN` because dropdowns navigate options that way.
- **CENTER short** = `LV_KEY_ENTER`, except on Status it's a global "open Menu" shortcut.
- **CENTER long** = panic-out to Status from any non-Status screen.
- **Group population** is done via `_populateGroupFromSubtree` — recursively walks the screen and adds anything `lv_obj_check_type` recognises as interactive (switch, checkbox, dropdown, roller, slider, arc, button) to the group in DOM order.

### Settings polarity convention
- All sleep-inhibit settings use **positive form**: `TRUE = allow sleep`. Keeps the SLS switch ON/OFF labels intuitive.
- Examples: `SKEY_DEBUG_SLEEP_WITH_SERIAL` (true = allow sleep with serial open), `SKEY_SLEEP_WHILE_USB` (true = allow sleep when USB attached). Inhibit logic in `_isInhibited()` inverts: `!_sleep_w_serial` and `!_sleep_while_usb`.

### Display state machine
PowerManager owns three display states (`DisplayState::OFF/ON/DIM`):
- `OFF` → backlight off, `g_ui.setRenderEnabled(false)`. Used for silent TIMER-wake scan windows and pre-deep-sleep teardown.
- `ON` → user's stored brightness, render enabled.
- `DIM` → `SCREEN_BRIGHTNESS_MIN` PWM, render enabled. Triggered when ≤5 s remain in the interactive timeout.

`setRenderEnabled(false)` skips `lv_timer_handler()` entirely — saves ~70 % CPU during silent scans (LVGL is the heaviest thing in the loop). On wake-from-sleep transitions to ON/DIM, PowerManager also calls `g_logger.applyPerfMonitor()` to re-sync the LVGL FPS overlay against the current debug level (LVGL auto-shows it at `lv_display_create` and someone has to actively re-hide it if the user isn't at PERF level).

---

## Power model

| Wake cause | `_user_active` | Display | Sleep timer | Boot indicator (LED + haptic) |
|---|---|---|---|---|
| Cold boot (`ESP_RST_POWERON`) | true | ON | 15-20s interactive | yes |
| EXT1 wake (button) from sleep | true | ON | 15-20s interactive | yes |
| TIMER wake (autonomous scan) | false | OFF | 5s scan-then-sleep | no (silent) |
| Software reset (`ESP.restart`) | true | ON | 15-20s interactive | **no** — the user just clicked Reboot, a second indicator on the other side feels like one long buzz |
| Shipping mode wake | requires 2 s CENTER hold to confirm; otherwise re-sleeps | OFF→ON after confirm | normal | no |

Sleep entry calls `prepareForSleep()` which does:
1. `g_hal.clearLEDs()` — must run **before** prepareForSleep takes the LED data pin away from RMT.
2. `_tft.sleep()` — LCD controller into sleep, drops to ~50 µA.
3. Detach LEDC from backlight pin, drive LOW, lock the pad with `gpio_hold_en` (and same for LED data pin).
4. `gpio_deep_sleep_hold_en()` — honour pad locks during deep sleep.

`HAL::begin()` releases those holds at the start so peripherals can reinit. **Don't add a new pad-held GPIO without releasing it in `begin()`** — it'll stay locked low forever after the first sleep cycle.

Deep sleep wakes only via EXT1 on `PIN_BTN_1` (GPIO6 — the only RTC-capable button pin; GPIO43/`PIN_BTN_2` is readable but can't be a wake source). GPIO6 goes low on left OR center, so any of those wakes the chip. On the next boot `PowerManager::checkWakeHoldOrResleep()` (the very first thing in `setup()` after Serial.begin) requires the user to hold CENTER — both `PIN_BTN_1` and `PIN_BTN_2` LOW — for the hold window (`SLEEP_WAKE_HOLD_MS` 1.5 s for regular sleep, `SHIPPING_WAKE_HOLD_MS` 2.5 s for shipping — a more deliberate unbox gesture). If the hold isn't satisfied it re-enters the same sleep it came from: shipping (`_shipping_pending`, EXT1 only) or regular (EXT1 + the scan-cycle timer stashed in `_deep_sleep_timer_us`). Timer wakes and cold boots skip the check. This makes pocket carry reliable — nothing but a deliberate center hold wakes the device.

---

## Footguns & lessons

These all cost real time. Read before re-treading:

### LVGL config + library caching
- **Use both `lv_conf.h` AND `-D` build_flags** for any LVGL feature that's conditionally compiled in a `.c` file (fonts, sysmon, optional widgets). PIO doesn't always notice that a header changed and rebuild the LVGL `.a`; the `-D` flag is what reliably forces it.
- **`LV_CONF_PATH` is required** in `platformio.ini` because LovyanGFX ships an example `lv_conf.h` at `examples/Advanced/LVGL_PlatformIO/src/lv_conf.h` that PlatformIO's recursive include scan finds before ours. The absolute-path `LV_CONF_PATH=\"$PROJECT_DIR/include/lv_conf.h\"` bypasses include-path search entirely.
- **`LV_TICK_CUSTOM` was removed in LVGL 9.x.** Register a tick callback with `lv_tick_set_cb(my_tick_cb)` instead — without it, animations and delays never advance even though `lv_timer_handler` is being called.
- **`LV_USE_PERF_MONITOR` auto-shows the overlay** in `lv_display_create()` ([lv_display.c:185](.pio/libdeps/seeed_xiao_esp32s3/lvgl/src/display/lv_display.c#L185)). Calling `lv_sysmon_hide_performance` after it doesn't invalidate the screen area, so the pixels stay until something else triggers a redraw — call `lv_obj_invalidate(lv_screen_active())` after toggling.

### GPIO / hardware
- **PIN_SPI_BL (GPIO3)** is a strapping pin with internal pull-up enabled by ROM. Without explicit `gpio_hold_en`, the backlight comes back on as soon as LEDC powers down at deep-sleep entry. Keep the pad held LOW through sleep.
- **PIN_LED_DATA (GPIO2)** floats through long sleeps and EMI/noise can be interpreted by WS2812s as data — observed as a single white flash on next wake. Same fix: hold the pad LOW with `gpio_hold_en`.
- **ERM vibration motors won't spin below ~150 PWM duty** (~2 V average across the motor). Anything lower whines audibly without producing tactile feedback. The "minor" feel of `HAPTIC_TICK_LIGHT` comes from a *short* duration at high duty, not a low duty cycle.
- **USB SOF detection window must be ≥100 ms.** Hosts pause SOFs occasionally (heavy traffic, brief suspends). The original 2 ms window flickered `_usb_attached` to false often enough to break sleep inhibition.
- **Boot LED flash must be skipped on TIMER wake AND on software resets.** Otherwise scan windows light up the device, and pressing Reboot gives two haptics back-to-back across the reset gap. Gate on `esp_reset_reason()` + `esp_sleep_get_wakeup_cause()`.

### SquareLine Studio
- **Re-export overwrites `src/UI/` entirely.** Don't edit those files. All bindings/wiring lives in `ui_controller.cpp`.
- **Lowercase names mode** is enabled in this project's SLS settings. Any expected widget pointer is `ui_<screen>_<type>_<name>` lowercase.
- **Dropdown options must be in enum order.** SLS stores them as a `\n`-separated string; the index is what gets stored. We've eaten this bug twice (Debug Level had an extra "Off" once; brightness slider had range 50..255 instead of enum 0..3).
- **The screen object pointers (`ui_screen_*`) become NULL after `screen_destroy`.** Don't hold pointers to widgets across screen destroys; iterate the SLS-exported globals each time.
- **SLS's auto-generated `ui_helpers.c` references many widget APIs** (slider, dropdown, roller, etc.) even if you don't use them. Either enable those `LV_USE_*` widgets in `lv_conf.h` (linker DCE strips unused ones anyway) or accept implicit-declaration warnings.

### Settings / NVS
- **Schema bumps reset NVS** — every `SCHEMA_VERSION` increment loses user-saved settings. Necessary when a key's *meaning* changes (polarity flip, enum size change) but annoying. Try to add new keys without re-purposing existing ones.
- **`s_key_mask` table in `settings_service.cpp` MUST stay in sync with `SettingsKey` enum order.** No static_assert for size, so a missing entry shifts every subsequent mask wrong without compile error. Counter-example: `s_key_name` in `serial_console.cpp` *does* have a static_assert and caught a real bug (NO_SLEEP_WHILE_CHARGING was silently misaligned).

### Debug levels
- `DEBUG_INFORMATIONAL` (0) — only state changes (sleep, scan, settings, alerts). Sleep-countdown logged only when ≤5 s remain.
- `DEBUG_HIGH` (1) — INFO + UI activity, button presses, battery, every countdown tick.
- `DEBUG_RENDERING_PERF` (2) — suppresses event firehose, emits one perf summary line/sec, shows LVGL FPS+CPU overlay.
- `DEBUG_SCANNING_PERF` (3) — placeholder; treated as RENDERING_PERF until scanner work begins.

PERF level "events processed" counter is from `g_bus.perfSnapshotAndReset()` — measures dispatch wall-clock including subscriber `onEvent()` time, so a slow handler shows up here.

---

## Pin map (from `include/hal.h` + `GPIO.md`)

| GPIO | Use | Notes |
|---|---|---|
| 1 | SPI_DC (LCD) | LovyanGFX-managed |
| 2 | LED_DATA (WS2812 ×6) | Held LOW through deep sleep |
| 3 | SPI_BL (backlight PWM) | LEDC ch0, 5 kHz, held LOW through sleep, strapping pin |
| 4 | VIBRATE | LEDC ch1, 20 kHz (above audible). Low-side N-MOS gate, 100 kΩ pull-down. |
| 5 | VSENSE | ADC, voltage divider /2 (VBATT 3.0–4.2 V → 1.5–2.1 V) |
| 6 | BTN_1 (matrix line A) | RTC-capable, used for EXT1 wake |
| 7 | SPI_SCK | LovyanGFX |
| 8 | SPI_RST | LovyanGFX |
| 9 | SPI_MOSI | LovyanGFX |
| 43 | BTN_2 (matrix line B) | NOT RTC-capable |
| 44 | SPI_CS | LovyanGFX |

Buttons are a 2×2 diode matrix: GPIO6 LOW only = LEFT, GPIO43 LOW only = RIGHT, both LOW = CENTER. EXT1 wake is configured for GPIO6 alone (covers all buttons via the diode matrix).
