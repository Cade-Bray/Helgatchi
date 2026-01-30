
#pragma once

#include <stdint.h>

enum class EventType : uint8_t {
  Tick,
  Button,
  Wake,
  WifiSighting,
  BleSighting,
  AddOuiRule,
  AddMacRule,
  AddCompanyRule,
  AddNameRule,
  AlertFired,
  AlertUiDismissed,
  ToggleRulePack,
	ToggleRule,
  WipeAction,
  SettingsChanged,
  RulesChanged,
  ForceSleep,
};

enum class WipeActionKind : uint8_t {
	LocalStats = 1,
	CustomRules = 2,
	ResetSettings = 3,
};

struct WipeActionEvent {
	WipeActionKind kind;
};

enum class ButtonId : uint8_t { Left, Right, Center };
enum class ButtonAction : uint8_t { Press, LongPress };

struct ButtonEvent {
  ButtonId id;
  ButtonAction action;
};

enum class WakeEventReason : uint8_t { ColdBoot, Timer, Button, Alert };

struct WakeEvent {
  WakeEventReason reason;
};

struct WifiSightingEvent {
  uint32_t tsMs;
  uint32_t oui24;
  int8_t rssi;
};

struct BleSightingEvent {
  uint32_t tsMs;
  // Lower 48 bits contain the BLE address (MAC). 0 means unknown.
  uint64_t addr48;
  uint32_t oui24;
  int8_t rssi;
  // Best-effort advertised device name (truncated). Empty string if none.
  char name[20];

	// Manufacturer Specific Data (AD type 0xFF) Company Identifier (Bluetooth SIG).
	// If present, the first two bytes of MSD are a little-endian company ID.
	uint8_t hasMsdCompanyId;
	uint16_t msdCompanyId;

  // Best-effort advertised service UUIDs (canonicalized to 128-bit).
  // Small fixed cap to keep the event size bounded.
  static constexpr uint8_t kMaxServiceUuids = 2;
  uint8_t serviceUuidCount;
  uint8_t serviceUuids[kMaxServiceUuids][16];
};

enum class OuiRuleSource : uint8_t { Wifi = 1, Ble = 2 };

struct AddOuiRuleEvent {
  uint32_t oui24;
  OuiRuleSource source;
};

struct AddMacRuleEvent {
  uint64_t addr48;
  char name[20];
};

struct AddCompanyRuleEvent {
  uint16_t companyId;
  char name[20];
};

struct AddNameRuleEvent {
  char name[20];
};

struct AlertFiredEvent {
  uint32_t tsMs;
  uint32_t ruleId;
  int8_t rssi;
};

struct ToggleRulePackEvent {
	// Index into CoreState.rulePacks[]
	uint8_t index;
  // Desired enabled state
  bool enabled;
};

struct ToggleRuleEvent {
  uint32_t id;
  bool enabled;
};

struct Event {
  EventType type;
  union {
    ButtonEvent button;
    WakeEvent wake;
    WifiSightingEvent wifi;
    BleSightingEvent ble;
    AddOuiRuleEvent addOuiRule;
    AddMacRuleEvent addMacRule;
    AddCompanyRuleEvent addCompanyRule;
    AddNameRuleEvent addNameRule;
    AlertFiredEvent alert;
		ToggleRulePackEvent togglePack;
		ToggleRuleEvent toggleRule;
    WipeActionEvent wipe;
  };
};