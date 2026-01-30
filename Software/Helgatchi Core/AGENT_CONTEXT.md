# Helgatchi Core — Agent Context

This file is a fast on-ramp for automated agents and new contributors.

## What This Project Is
Helgatchi Core is an ESP32-S3 firmware (PlatformIO/Arduino) that:
- scans WiFi + BLE
- matches sightings against rules
- fires alerts (screen overlay, vibe/LED)
- manages sleep/scan-burst behavior to save power

Target board/env (PlatformIO): `seeed_xiao_esp32s3`

## Workspace Gotchas
This VS Code workspace contains multiple firmware projects/folders:
- `Software/Helgatchi Core` (this folder): primary/active “Core” firmware
- `PlatformIO/Projects/Helgatchi` (another project copy)
- `PlatformIO/Projects/LCD display test` (separate test project)

When editing, confirm you’re changing the intended project copy.

## High-Level Architecture (Mental Model)

### Runtime Loop
1. `Core` owns global state (`CoreState`), devices (Display/Backlight/Vibe), scanners, rule manager, and UI.
2. Scanners emit events (BLE/WiFi sightings).
3. `AlertEngine` evaluates sightings vs enabled rules and emits `AlertFired`.
4. UI renders views; may switch into Alerts UI on alert.
5. `Core::maybeSleep_()` drives mode transitions (Interactive → IdleAwake → PreSleep, and timer wake → ScanBurst → sleep or interactive).

### State + Events
- State is held in `CoreState` (mode, wakeReason, settings, counters, last sightings, last alert details).
- Communication is event-driven through `EventBus` and `Events.h` structs.

#### Key Event Types (Events.h)
- `Wake`: posted once at boot from `Core::postInitialEvents_()` so subsystems can react uniformly.
- `WifiSighting` / `BleSighting`: produced by scanners; consumed by `AlertEngine` + telemetry.
- `AlertFired`: produced by `AlertEngine` when a rule matches.
- `AlertUiDismissed`: produced by UI when the user navigates away from the Alerts section (used to revert `Alerting → Interactive`).

#### Where Events Are Dispatched
- `Core::handleEvent_()` is the central dispatcher.
- UI interactions are processed in `Ui::onButton()` and typically push events back to Core via `EventBus`.

## Modes & Wake Reasons

### Modes (CoreState.mode)
Defined in `src/core/CoreState.h`:
- `Boot`
- `Interactive` (screen on, active scanning)
- `Alerting` (a temporary “interactive-but-alert-focused” state; reverts when leaving Alerts section)
- `IdleAwake` (dimmed, reduced duty)
- `ScanBurst` (timer wake, screen off, short scan)
- `PreSleep` (stop scanning, configure wake sources, enter deep sleep)

Important relationships:
- `ScanBurst` is a special mode that drives burst completion logic in `Core::maybeSleep_()`.
- `Alerting` is intentionally *not* entered while in `ScanBurst` (to avoid breaking burst completion logic).

### Wake Reasons (CoreState.wakeReason)
Defined in `src/core/CoreState.h`:
- `ColdBoot`
- `Timer`
- `Button`
- `Alert` (used when a timer scan burst ended due to an alert waking the screen/UI)

`WakeReason` is also mapped to `WakeEventReason` in `Core::postInitialEvents_()`.

## Where Things Live

### Entrypoint
- `src/main.cpp` — creates `Core`, calls setup/loop.

### Core State Machine
- `src/core/Core.cpp`
  - `enterInteractive_()` / `enterIdleAwake_()` / `enterScanBurst_()` / `enterPreSleep_()`
  - `maybeSleep_()` — scan-burst completion logic & sleep transitions
  - `handleEvent_()` — main event dispatcher
- `src/core/CoreState.h` — `CoreState`, `SystemMode`, `WakeReason`, `Settings`.

### UI
- `src/ui/Ui.h`, `src/ui/Ui.cpp`
  - View state is internal (`Ui::View`)
  - `Ui::showAlertOverlay()` switches to Alerts view in specific contexts and flashes the header
  - `Ui::onButton()` handles navigation and emits events back to Core

### Alerting
- `src/services/AlertEngine.*` — decides when an alert fires
- `src/core/Events.h` — `AlertFiredEvent`

### Rules
- `src/rules/RulesManager.*` — rule storage, enabling/disabling, pack integration

### Scanning
- `src/scan/ScanCoordinator.*` — orchestrates BLE + WiFi scanner operation
- `src/scan/BleScanner.*`, `src/scan/WiFiPromiscScanner.*`

### Persistence
- `src/data/Store.*` — NVS and/or filesystem persistence (settings, stats, rules)

### Power
- `src/power/PowerManager.h` — wake reason detection and deep sleep configuration

## Key Flows (Trace Guide)

### Boot
- `Core::setup()` populates `state_.wakeReason = power_->getWakeReason()`
- `postInitialEvents_()` posts `EventType::Wake` with a `WakeEventReason`

### Timer Sleep Scan Burst
- Timer wake → `wakeReason == Timer`
- Core enters `ScanBurst` and runs scanners briefly with backlight off
- At burst completion:
  - if `settings.alertScreen && alertsFired > 0` → go `Interactive` (and keep Alerts view)
  - else → deep sleep again

### Alert On Timer Burst
- `AlertFired` occurs during `ScanBurst`
- UI overlay will switch to Alerts view + flash header
- When burst completes and we go interactive, `wakeReason` is updated to `Alert` and mode becomes `Alerting`
- Leaving the Alerts section emits `EventType::AlertUiDismissed` to revert `Alerting → Interactive`

## UI Navigation Model
The UI has an internal `Ui::View` state. The “Alerts section” is considered any of:
- `Alerts`
- `AlertsConfig`
- `AlertsRuleList`
- `AlertPacks`

When an alert fires, `Ui::showAlertOverlay()` may switch the current view to `Alerts` (and start a flashing header).

## Session Changelog (Important Recent Behavior)
This workspace/session has recently implemented or refined these behaviors:

1) **Alerts page reorganization**
- Header now persists "Alert: [name]" when an alert has fired, not just during flash
- Alert display shows rule label in large text (size 2)
- Below label: pack name, match type ("Matched: OUI/Company/MAC/BLE Name/Service UUID"), and actual device data
- Added fields to `CoreState` to capture actual device data at alert time:
  - `lastAlertDeviceOui24`, `lastAlertDeviceAddr48`, `lastAlertDeviceCompanyId`, `lastAlertDeviceHasCompanyId`, `lastAlertDeviceName`
- Alert display now shows the full device name, actual OUI, actual company name, etc. that triggered the alert (not just the rule pattern)

2) **Manual sleep from Status view**
- Long-press Center button (1200ms) from Status view triggers immediate sleep
- Added `EventType::ForceSleep` event to bypass USB serial check
- Added `CoreState.buttonWakeMs` timestamp to prevent immediate re-sleep after button wake
- Protection: ForceSleep is ignored for 1 second after waking from button press
- Footer hint shows "Hold C: Sleep" on Status view

3) **Long press duration increased**
- `kLongPressMs` increased from 600ms to 1200ms in `Buttons.h`
- Applies to all long-press actions (back navigation, sleep trigger, etc.)

4) Alerts during sleep scan bursts wake to Alerts page
- Root issue: after `Ui::showAlertOverlay()` switched the UI to Alerts, the scan-burst completion path transitioned to interactive and reset the UI back to Status.
- Fix: `Core::enterInteractive_()` preserves the Alerts view when transitioning from `ScanBurst` and `alertsFired > 0` with `alertScreen` enabled.

5) Alert-specific wake semantics
- Added `WakeReason::Alert`.
- When a `ScanBurst` ends due to an alert, Core updates `wakeReason` from `Timer` to `Alert` before entering interactive.

6) Temporary Alerting mode
- Uses existing `SystemMode::Alerting`.
- When an alert fires while already `Interactive`, Core transitions to `Alerting` if the UI is actually showing the Alerts section.
- UI emits `EventType::AlertUiDismissed` when the user leaves the Alerts section; Core reverts `Alerting → Interactive`.

## Invariants / “Don’t Break These”
- Do not change `CoreState.mode` to `Alerting` while in `ScanBurst` (burst completion logic depends on mode staying `ScanBurst`).
- Avoid calling `Ui::setRootScreenMain()` on transitions that should preserve an alert-driven navigation state.
- If you add new `WakeReason` or `SystemMode` enum values, update:
  - `modeToStr()` / `wakeToStr()` in `Ui.cpp`
  - the mapping in `Core::postInitialEvents_()`

## Build / Flash / Monitor
PlatformIO environment: `seeed_xiao_esp32s3`

Typical commands (PowerShell):
- Build: `platformio run -e seeed_xiao_esp32s3`
- Upload + monitor: `platformio run -t upload -t monitor -e seeed_xiao_esp32s3`

In VS Code, use the provided PIO tasks when available.

**CRITICAL AGENT REQUIREMENT**: 
- **ALWAYS run a build** (`platformio run -e seeed_xiao_esp32s3`) **before completing any task** that involves code changes.
- This verifies that all code modifications compile successfully and prevents pushing broken code.
- If the build fails, review errors, fix the issues, and rebuild until successful.
- For deep debugging sessions involving serial output validation, additionally use the upload and monitor task to verify changes on hardware.

## Agent Tips (When Making Changes)
- Prefer small, mode-specific changes in `Core::maybeSleep_()` and `Core::enterInteractive_()`; mode transitions are easy to accidentally regress.
- UI is stateful; changing `Ui::setRootScreenMain()` can override alert-driven view changes.
- Any new enum value usually requires updating:
  - string helpers in `Ui.cpp` (`modeToStr`, `wakeToStr`)
  - mapping in `Core::postInitialEvents_()` (WakeReason → WakeEventReason)

## Quick Pointers (Common “Where is …?”)
- “Alert overlay / red flash header”: `Ui::showAlertOverlay()`, `startHeaderFlash_()`, `tickHeaderFlash_()`
- “Scan burst timing”: `CoreState.settings.scanBurstMs`, `ScanCoordinator::startBurst()`
- “Why did it go to sleep?”: `Core::maybeSleep_()`
- “Rules/pack toggles”: `RulesManager`, events `ToggleRulePack`, `ToggleRule`
