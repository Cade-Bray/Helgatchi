# Screens

This document describes the actual UI screens, their SquareLine Studio
implementation, and the wiring between SLS-exported widgets and firmware
behavior. The earlier revision was a design intent — this one tracks what
exists, what doesn't, and the conventions in use.

---

## Source of truth

- **SLS project** (external): `Helgatchi-UI`, SLS 1.6.0, targeting LVGL 9.3, "Arduino with TFT_eSPI" board template, "Force exported names to lower case" enabled, "Custom variable prefix" `uic`.
- **Exported files**: `src/UI/*.{c,h}` — *do not edit by hand, re-export overwrites.*
- **Firmware wiring**: `src/ui_controller.cpp`. All buttons / settings / battery / dim / haptic hooks live here.

Whenever the SLS project is re-exported, widget pointer names may change. The compiler will catch most of these as undeclared identifiers in `_bindings[]` / `_actions[]` / batteryLabels[] in `ui_controller.cpp`.

---

## Screens that exist today

| # | Screen | SLS object | Status |
|---|---|---|---|
| 1 | Splash | `ui_screen_splash_screen` | ✅ Implemented |
| 2 | Status | `ui_screen_status_screen` | ✅ Renders top bar, no body content yet |
| 3 | Menu | `ui_screen_menu_screen` | ✅ Horizontal carousel, only Settings panel exists |
| 4 | Settings | `ui_screen_settings_screen` | ✅ Most-developed screen — actual settings widgets bound to NVS |

Screens deferred to later milestones: **Scan, Devices, Device Detail, Rules, Alerts, About**. Each will follow the same pattern as Settings (top bar component + content panel + binding table).

---

## Top bar component

Built once in SLS as a Component, instanced on Status / Menu / Settings (purple in the Hierarchy panel — they share the same Component definition and update together if edited).

Three labels per instance, named consistently across screens:
- `top_bar_left_text` — left-side status indicator (currently shows "zzz" placeholder on Menu, free text per screen)
- `top_bar_center_text` — screen title
- `top_bar_right_text` — battery percentage

The right-text labels are tracked in `_batteryLabels[]` in `ui_controller.cpp`. On `EV_BATTERY_UPDATED` they all get rewritten with the formatted percentage / "CHRG" / "FULL" / "??". A cached `_last_batt_*` is also re-applied when a screen loads, so a freshly-loaded screen paints immediately instead of waiting up to 30 s for the next battery sample.

To add the top bar to a new screen: drop the Component instance in SLS → re-export → add the new screen's `top_bar_right_text_*` pointer to `_batteryLabels[]`.

---

## Splash → Status

The splash → status transition is **defined in SLS itself**, not in C:
- Trigger: `LV_EVENT_SCREEN_LOADED` on splash
- Action: `Change Screen` to Status with `FADE_OUT`, 500 ms, **3000 ms delay**

The C side just calls `ui_init()` (loads splash) and lets LVGL handle the rest. This is the canonical SLS pattern — when something can be a CLICKED-event-driven screen change, prefer doing it in SLS.

The 3 second delay needs `lv_tick_set_cb(_tick_cb)` to be registered (LVGL 9.x removed `LV_TICK_CUSTOM`, so without this the delay never elapses). `UIController::begin()` does this before `lv_display_create`.

---

## Menu screen

Horizontal scroll-snap carousel (`ui_menu_screen_container_container5`). Each child panel is one menu entry. Currently only `panel_settings_pannel` (sic — SLS-generated typo) exists, with `lv_image_settings_image`, label, and a `Clicked → Change Screen → Settings` event defined in SLS.

Keypad navigation: `_grp_menu` is populated by iterating the carousel container's children, so adding a new menu panel in SLS (Devices, Rules, Alerts, etc.) auto-shows up in the focus cycle without code changes. LVGL's group focus auto-scrolls the snap container.

Adding a new menu entry:
1. In SLS: copy the Settings panel inside `container_container5`, rename, give it its own `Clicked → Change Screen → <target>` event
2. Re-export
3. (No C change needed for the menu screen itself)
4. Build / register the target screen separately

---

## Settings screen

The most-developed screen — every other future screen will follow this pattern.

### Layout
A top bar + a vertical scrollable panel (`panel_panel2`) of section labels and option containers. Each option container has a label and one widget (switch/dropdown/slider/button).

### Bound settings (`_bindings[]` in `ui_controller.cpp`)

Each entry is `{ &ui_<widget>, SKEY_<X> }` — the polymorphic `_readWidget` / `_writeWidget` handles the widget type at runtime, so a switch, dropdown, slider, etc. all work via the same path.

Currently bound:
| Widget | SLS pointer | SettingsKey |
|---|---|---|
| Brightness dropdown | `ui_settings_screen_dropdown_brightness` | `SKEY_SCREEN_BRIGHTNESS` |
| Performance Mode dropdown | `ui_settings_screen_dropdown_performance_mode` | `SKEY_PERF_MODE` |
| Debug over Serial switch | `ui_settings_screen_switch_debug_over_serial` | `SKEY_DEBUG_SERIAL_ENABLED` |
| Debug Level dropdown | `ui_settings_screen_dropdown_debug_level` | `SKEY_DEBUG_LEVEL` |
| Sleep with Serial switch | `ui_settings_screen_switch_sleep_while_serial` | `SKEY_DEBUG_SLEEP_WITH_SERIAL` |
| Sleep with USB switch | `ui_settings_screen_switch_sleep_while_usb` | `SKEY_SLEEP_WHILE_USB` |

### Custom (non-1:1) bindings

Some widgets don't map directly to a single SKEY:
- **BLE / WiFi scanning switches** combine into `SKEY_SCAN_MODE` via bit-OR (BLE = bit 0, WiFi = bit 1). Custom `_onScanBitsChanged` rebuilds the combined value from both switch states on any change. `_refreshScanBits()` in the other direction.
- **Reboot button** (`reboot_button`): `_on_reboot_button` posts `CMD_POWER_REBOOT`; PowerManager tears down peripherals (`g_vibe.stop()` + `HAL::prepareForReboot()`) before `ESP.restart()`. (Was a bare `ESP.restart()` that skipped teardown and could leave the motor buzzing through the boot window.)

### Action buttons (`_actions[]`)

Buttons that fire one-shot commands. Currently:
| Button | Command |
|---|---|
| `button_shipping_mode` | `CMD_POWER_SHIPPING_SLEEP` |
| `button_sleep` | `CMD_POWER_SLEEP` (bypasses inhibit by design — Sleep Now button) |

### Sleep countdown text

`ui_settings_screen_label_sleep_text` displays the live sleep countdown. Updated on `EV_SLEEP_COUNTDOWN_UPDATED`:
- `0xFFFF` (sentinel from PowerManager when inhibited) → `"Will not sleep"`
- otherwise → `"Sleep in Ns"`

### Dropdown options must match the enum
SLS stores dropdown options as a `\n`-separated string and saves the index. **The index is the value stored in NVS.** If your dropdown options aren't in enum order, you'll silently store wrong values — we've eaten this bug twice (extra "Off" leading the Debug Level dropdown, slider range 50..255 vs enum 0..3 for brightness).

Current required option strings:
- Brightness: `Min\nLow\nMedium\nHigh\nMax` (matches `ScreenBrightness` enum)
- Performance Mode: `Performance\nBalanced\nPower Saver` (matches `PerfMode` enum, missing DYNAMIC by intent)
- Debug Level: `Informational\nHigh\nRender perf\nScan perf` (matches `DebugLevel` enum)

---

## Keypad input contract

UIController converts physical button events into LVGL keys with these conventions:

| Action | Default (nav mode) | In edit mode | Dropdown open |
|---|---|---|---|
| LEFT | `LV_KEY_PREV` (prev focused widget) | `LV_KEY_LEFT` (decrement value) | `LV_KEY_UP` (prev option) |
| RIGHT | `LV_KEY_NEXT` (next focused widget) | `LV_KEY_RIGHT` (increment value) | `LV_KEY_DOWN` (next option) |
| CENTER short on Status | open Menu (direct screen change, bypasses LVGL) | — | — |
| CENTER short elsewhere | `LV_KEY_ENTER` | `LV_KEY_ENTER` (commit edit) | `LV_KEY_ENTER` (select option) |
| CENTER long | back to Status (panic-out) | — | — |

**Common mistakes**:
- Sending `LV_KEY_LEFT/RIGHT` for navigation. Switches interpret those as direct toggle and you'll never get past the first switch in the focus group.
- Forgetting that `lv_dropdown_is_open()` is widget-local state, not group editing state. `lv_group_get_editing()` returns false even with a dropdown list visible. Both checks needed.

### Group population
`_populateGroupFromSubtree()` recursively walks a screen's widget tree and adds anything `lv_obj_check_type` recognises as interactive (switch, checkbox, dropdown, roller, slider, arc, button) to the group **in DOM order**. So the focus cycle matches the order children appear in SLS's Hierarchy panel — drag widgets there to reorder navigation.

---

## Screen lifecycle hooks

`_scr_evt_cb` (registered for all four screens via `_registerScreenLogging`) handles three things on every screen transition:

1. **Logging** — `[t] SCREEN_LOAD_START / LOADED / UNLOAD_START / UNLOADED  <name>` to serial when debug serial is on.
2. **Group switching** — on `LV_EVENT_SCREEN_LOAD_START`, set the keypad indev's group to the screen's group (or NULL for splash/status).
3. **Settings refresh** — on Settings screen load, repaint widget values from `g_settings`.

Plus battery label re-paint (`_refreshBatteryLabels`) on every screen-load so the right-text shows the latest reading immediately.

---

## What's not implemented

- **Scan screen** — needs to surface scan state, lock-on, scan mode toggle. Future commands: `CMD_SCAN_START`, `CMD_SCAN_STOP`, `CMD_SCAN_LOCKON_START/STOP`. Will subscribe to `EV_SCAN_STATE_CHANGED` for the toggle status.
- **Devices screen** — paged list. Needs Entity Store + AppState before this is meaningful. Pagination + per-row rendering will be the main UI work.
- **Device Detail screen** — view one entity, optionally `CMD_RULE_ADD_CUSTOM` from it.
- **Rules screen** — toggle rule packs (`CMD_RULE_PACK_ENABLE/DISABLE`), wipe customs.
- **Alerts screen** — list active + history, ack (`CMD_ALERT_ACK`) / snooze (`CMD_ALERT_SNOOZE`).
- **About screen** — firmware version, build info, ruleset version.

The pattern for each is identical to Settings: SLS top-bar instance + content layout + add the appropriate widgets to either `_bindings[]` (state) or `_actions[]` (commands). Screen-load hook for any data that needs paint-on-entry.

---

## Cross-cutting

### `EV_UI_ACTIVITY` is used as the activity heartbeat
Every button press posts it. Every character typed in the serial console posts it. PowerManager updates `_last_activity_ms` and forces display ON. If a future input source (touch, gesture) is added, fire `EV_UI_ACTIVITY` from there too.

### Render is gated by display state
`UIController::tick()` is a no-op when `_render_enabled` is false. PowerManager flips it from `_setDisplay`: `OFF → false`, `ON/DIM → true`. This saves ~70 % CPU during silent TIMER-wake scan windows.

### LVGL FPS overlay
Compiled in via `LV_USE_PERF_MONITOR=1`, but visibility is runtime-toggled by `LogService::applyPerfMonitor()` based on `DebugLevel`. Reapplied on:
- After `g_ui.begin()` (display-create auto-shows the overlay; we have to actively re-hide if level < PERF)
- On every `EV_SETTINGS_CHANGED` that crosses the PERF threshold
- On `_setDisplay(ON/DIM)` (covers wake-from-silent-scan transitions)
