# Screens

This document describes the UI/UX screens, user interactions, and how UI actions map into the event/service architecture. It is intentionally **behavioral**, not visual. LVGL layout and widgets live in generated code; logic lives in `UIController`.

---

## UI Architecture Rules (recap)

* UI is **state-driven**: screens render from `AppState` only.
* UI never calls services directly.
* UI emits **commands (CMD_*)** and **UI events (EV_UI_ACTIVITY)**.
* All LVGL calls happen inside `UIController`.
* Long-press / short-press behavior is normalized before screen logic.

### Button conventions (global)

* **Short C**: confirm / toggle / enter
* **Long C**: back / exit / sleep (context-dependent)
* **Left / Right**: navigate options or screens
* Any button press emits `EV_UI_ACTIVITY` (resets sleep timer).

---

## Screen Index

1. Status (default)
2. Menu (root navigator)
3. Scan
4. Devices
5. Device Detail
6. Rules
7. Alerts
8. Settings
9. About / Info

---

## 1. Status Screen (default)

**Purpose:** At-a-glance system state.

### Displays (from AppState)

* Scan state (idle / scanning / lock-on)
* Devices seen count
* Active alerts count
* Power state / battery
* Sleep countdown (if applicable)

### Actions

* **Short C** → enter Menu (`CMD_UI_CONFIRM`)
* **Long C** → request sleep (`CMD_POWER_SLEEP`)

### Architecture hooks

* Renders on: `EV_SCAN_STATE_CHANGED`, `EV_ENTITY_UPDATED`, `EV_ALERT_*`, `EV_POWER_*`
* No direct service calls

---

## 2. Menu Screen

**Purpose:** Top-level navigation hub.

### Menu items

* Status
* Scan
* Devices
* Rules
* Alerts
* Settings
* About

### Actions

* **Left / Right** → change selection
* **Short C** → enter selected screen
* **Long C** → return to Status

### Architecture hooks

* Pure UI state (menu index in AppState or UIController-local)

---

## 3. Scan Screen

**Purpose:** Control scanning behavior.

### Displays

* Current scan mode
* Lock-on state (if enabled)

### Actions

* **Short C** → toggle scan start/stop

  * Emits `CMD_SCAN_START` or `CMD_SCAN_STOP`
* **Secondary option (if present)** → toggle lock-on

  * Emits `CMD_SCAN_LOCKON_START` / `CMD_SCAN_LOCKON_STOP`
* **Long C** → back to Menu

### Architecture hooks

* Scan state confirmed via `EV_SCAN_STATE_CHANGED`
* UI does not assume success until event arrives

---

## 4. Devices Screen

**Purpose:** Browse detected entities.

### Displays

* Paged list of entities from Entity Store (via AppState summary)
* Basic stats (last seen, count, strongest RSSI)

### Actions

* **Left / Right** → scroll list
* **Short C** → open Device Detail
* **Long C** → back to Menu

### Architecture hooks

* Entity list derived from Core State snapshot
* No direct registry iteration in UI thread

---

## 5. Device Detail Screen

**Purpose:** Inspect a single entity.

### Displays

* Vendor / company (if known)
* Seen count / first seen / last seen
* Services / attributes (summary only)

### Actions

* **Short C** → add rule based on this device (optional v1)

  * Emits `CMD_RULE_ADD_CUSTOM`
* **Long C** → back to Devices

### Architecture hooks

* Uses selected `entity_id` stored in AppState

---

## 6. Rules Screen

**Purpose:** Manage rule packs and custom rules.

### Displays

* Enabled rule packs
* Custom rules

### Actions

* **Short C** → toggle enable/disable pack

  * Emits `CMD_RULE_PACK_ENABLE` / `CMD_RULE_PACK_DISABLE`
* **Secondary option** → wipe custom rules

  * Emits `CMD_RULE_WIPE_CUSTOM`
* **Long C** → back to Menu

### Architecture hooks

* Changes confirmed via `EV_RULES_CHANGED`
* UI does not assume immediate application

---

## 7. Alerts Screen

**Purpose:** View and manage alerts.

### Displays

* Active alerts
* Alert history (bounded)

### Actions

* **Short C** → acknowledge selected alert

  * Emits `CMD_ALERT_ACK`
* **Secondary option** → snooze

  * Emits `CMD_ALERT_SNOOZE`
* **Long C** → back to Menu

### Architecture hooks

* Renders from AppState alert snapshot
* Alert lifecycle driven by Alert Manager events

---

## 8. Settings Screen

**Purpose:** Configure system behavior.

### Displays

* Public mesh enabled/disabled
* Sleep settings
* Scan defaults
* Debug/log settings

### Actions

* **Short C** → toggle or edit value

  * Emits `CMD_SETTINGS_SET`
* **Save option** → persist

  * Emits `CMD_SETTINGS_SAVE`
* **Long C** → back to Menu

### Architecture hooks

* Settings changes reflected via `EV_SETTINGS_CHANGED`
* UI never writes settings directly

---

## 9. About / Info Screen

**Purpose:** Informational only.

### Displays

* Firmware version
* Ruleset version
* Build info

### Actions

* **Long C** → back to Menu

---

## Cross-cutting UX behaviors

### Sleep behavior

* Any button press emits `EV_UI_ACTIVITY`
* Power Manager resets sleep timer on UI activity
* Alerts may force temporary awake-hold

### Error / busy states

* UI shows last-known state until confirmed events arrive
* No spinners that block input

### Public mesh indication

* Public alerts shown with distinct visual indicator
* Public-origin alerts never auto-modify rules/settings

---

## Non-goals (for clarity)

* UI does not perform parsing, lookup, or rule evaluation
* UI does not assume commands succeed
* UI does not manage timers directly

---

This screen model is intentionally conservative: it keeps UI simple, deterministic, and fully driven by the event/service architecture so future features (mesh, fleet control, new radios) do not require UI rewrites.
