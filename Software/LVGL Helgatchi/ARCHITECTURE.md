# Architecture

This document describes the event/service architecture for the device firmware, including core services, public mesh behavior, and UI integration. This version fills in the missing contracts so the system can scale without hidden coupling.

---

## Core Concepts

### Event Bus

The Event Bus is the single messaging backbone between services.

It supports:

* **Commands (CMD_*)** – requests to perform an action
* **Events (EV_*)** – facts about something that already happened
* Fan-out subscriptions (multiple handlers per event)
* Bounded queue with drop/coalesce behavior

**Rule:** Services must not directly call other services to cause actions. Actions are requested via commands or implied by events. Direct calls are allowed only for pure helpers or read-only queries.

---

### Messaging Model

#### Commands vs Events

* **CMD_***: imperative requests ("start scan", "ack alert")
* **EV_***: immutable facts ("scan started", "alert raised")

Commands may be ignored or rejected. Events are never rejected.

#### Payload lifetime

* Low-rate events use fixed-size inline payloads (struct/union).
* High-rate scan data uses a bounded ring buffer or pool.
* Event bus messages never own heap memory.
* High-rate events carry IDs or summaries, not raw payloads.

#### Backpressure policy

* Scanner writes raw observations into a bounded buffer.
* Scanner emits `EV_OBS_BATCH_READY` periodically.
* Parser drains up to N observations per tick.
* Entity store coalesces duplicates (same entity within a short window updates counters but does not re-emit).

---

### Scheduler

Responsible for all time-based behavior:

* Scan cadence and lock-on windows
* UI tick cadence
* Power idle timers and sleep countdowns
* Periodic persistence flushes

Scheduler emits tick events (e.g. `EV_TICK_1S`) or directly invokes service `tick()` functions.

---

### Core State (AppState)

Single global truth snapshot used by the UI and high-level behavior.

Rules:

* Only the Core State reducer mutates Core State.
* UI renders exclusively from Core State.
* Services do not directly manipulate UI objects.

Core State subscribes to:

* `EV_SCAN_STATE_CHANGED`
* `EV_ENTITY_UPDATED`
* `EV_RULES_CHANGED`
* `EV_ALERT_*`
* `EV_POWER_*`
* `EV_SETTINGS_CHANGED`

---

## Services

### Settings Service

Owns configuration and persistence.

Commands:

* `CMD_SETTINGS_SET`
* `CMD_SETTINGS_SAVE`
* `CMD_SETTINGS_RESET_DEFAULTS`

Events:

* `EV_SETTINGS_CHANGED (mask, version)`

---

### Scanner Service

Performs RF scanning (BLE/WiFi).

States:

* IDLE
* SCANNING
* LOCKON

Commands:

* `CMD_SCAN_START`
* `CMD_SCAN_STOP`
* `CMD_SCAN_LOCKON_START`
* `CMD_SCAN_LOCKON_STOP`

Events:

* `EV_SCAN_STATE_CHANGED`
* `EV_OBS_BATCH_READY`

Subscribes:

* Scan commands
* `EV_SETTINGS_CHANGED`
* `EV_POWER_STATE_CHANGED`

---

### Parser (Normalizer)

Converts raw observations into canonical form.

Responsibilities:

* Frame parsing
* MAC normalization
* Manufacturer / service extraction

Subscribes:

* `EV_OBS_BATCH_READY`

Events:

* `EV_OBS_CANONICAL`

---

### Lookup / Enrichment

Maps IDs to human-meaningful data (OUI, Company ID, service names).

Service calls (read-only):

* lookup_oui_vendor()
* lookup_company_id()
* lookup_service_uuid()

Subscribes:

* `EV_OBS_CANONICAL`

Events:

* `EV_OBS_ENRICHED`

---

### Entity Store (Registry)

Maintains bounded device/entity state and statistics.

Service calls:

* entity_upsert(obs) -> entity_id
* entity_get(entity_id)
* entity_iter(filter/page)
* entity_wipe_stats()

Subscribes:

* `EV_OBS_ENRICHED`

Events:

* `EV_ENTITY_UPDATED`

Notes:

* Store is bounded with eviction (LRU or ring).

---

### Rule Engine

Evaluates rules against canonical/enriched entity updates.

Subscribes:

* `EV_ENTITY_UPDATED`
* `EV_SETTINGS_CHANGED`
* `EV_MESH_RULE_FIRED_RX`

Commands:

* `CMD_RULE_PACK_ENABLE`
* `CMD_RULE_PACK_DISABLE`
* `CMD_RULE_ADD_CUSTOM`
* `CMD_RULE_WIPE_CUSTOM`

Events:

* `EV_RULES_CHANGED`
* `EV_RULE_TRIGGERED_LOCAL`

---

### Alert Manager

Tracks alert lifecycle and user interaction.

States:

* NONE
* ALERTING
* ACKED
* SNOOZED
* DISMISSED

Subscribes:

* `EV_RULE_TRIGGERED_LOCAL`
* `CMD_ALERT_ACK`
* `CMD_ALERT_SNOOZE`
* `EV_SETTINGS_CHANGED`

Events:

* `EV_ALERT_RAISED`
* `EV_ALERT_UPDATED`
* `EV_ALERT_CLEARED`
* `EV_ALERT_SNOOZED`

---

### Power Manager

Controls sleep/wake behavior.

States:

* DEEP_SLEEP
* SLEEP
* IDLE
* INTERACTIVE
* NO_SLEEP

Subscribes:

* `EV_SETTINGS_CHANGED`
* `EV_ALERT_RAISED` (wake + awake-hold)
* `EV_UI_ACTIVITY`
* Power commands

Events:

* `EV_POWER_STATE_CHANGED`
* `EV_BATTERY_UPDATED`
* `EV_SLEEP_COUNTDOWN_UPDATED`

---

### Logging Service

Debug and diagnostic logging.

Subscribes:

* Selected `EV_*` based on debug level

Notes:

* Logging is rate-limited and sampled to avoid scan spam.

---

### Device Bridge (Public Mesh – v1)

Implements the public mesh channel only.

Outbound:

* Subscribes to `EV_RULE_TRIGGERED_LOCAL`
* Publishes `PUB_RULE_FIRED` messages

Inbound:

* Receives mesh messages
* Emits `EV_MESH_RULE_FIRED_RX` (untrusted hint)

Public mesh rules:

* No configuration sync
* No acknowledgments
* TTL required
* `(origin_id, seq)` deduplication
* Rate-limited, jittered repeats
* Inbound messages never directly change configuration

---

## UI Layer

### UI Controller

Bridges physical buttons to commands and renders UI from Core State.

Publishes:

* `EV_UI_ACTIVITY`
* `CMD_UI_NAV_NEXT`
* `CMD_UI_NAV_BACK`
* `CMD_UI_CONFIRM`
* Feature commands (scan, settings, alert ack)

Subscribes:

* `EV_SCAN_STATE_CHANGED`
* `EV_ENTITY_UPDATED`
* `EV_ALERT_*`
* `EV_POWER_*`
* `EV_SETTINGS_CHANGED`

Rules:

* All LVGL calls occur only inside UIController
* Generated LVGL code is not modified directly

---

## Notes

* Public mesh messages are treated as hints, not truth.
* Reliability is achieved via TTL + repetition, not acknowledgments.
* Future control/group channels can be layered on top of this model without changing core services.
