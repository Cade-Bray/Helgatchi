#pragma once
#include <stdint.h>

enum class SystemMode : uint8_t {
  Boot,
  Interactive,
  IdleAwake,
  ScanBurst,
  Alerting,
  PreSleep
};

enum class WakeReason : uint8_t {
  ColdBoot,
  Timer,
  Button,
  Alert
};

struct Settings {
  uint32_t sleepTimeoutMs = 45000;
  uint32_t wakeIntervalMs = 30000;
  uint32_t scanBurstMs    = 4000;

  bool alertScreen = true;
  bool alertLed    = true;
  bool alertVibe   = true;

  // Debug output level: 0=Off, 1=Low (stateful info), 2=High (verbose/noisy), 3=Performance (timing)
  uint8_t debugLevel = 1;

  // Backlight brightness level: 0=Min(10%), 1=Low, 2=Medium, 3=High
  uint8_t backlightLevel = 3;

  // Alert tracking duration: keep scanning aggressively for this long after alert (ms)
  uint32_t alertTrackingMs = 30000;

  // Burst-based tracking: repeat bursts every N ms for M ms after seeing device
  uint32_t trackingBurstIntervalMs = 5000; // 5 seconds between bursts
  uint32_t trackingDurationMs = 60000;     // 60 seconds total tracking time
};

// Summary of detections that occurred during the device's "sleeping" behavior
// (deep sleeping, waking on a timer, scanning briefly, then sleeping again).
// This is persisted in NVS so it can be shown after you wake interactively.
struct SleepScanStats {
  uint32_t bursts = 0;        // how many timer-wake scan bursts have run
  uint32_t bleHits = 0;       // number of BLE sightings observed during those bursts
  uint32_t wifiHits = 0;      // number of WiFi sightings observed during those bursts

  uint32_t lastBleOui24 = 0;
  int8_t lastBleRssi = 0;
  char lastBleName[20] = {0};

  uint32_t lastWifiOui24 = 0;
  int8_t lastWifiRssi = 0;
};

struct CoreState {
  // System lifecycle
  SystemMode mode = SystemMode::Boot;
  WakeReason wakeReason = WakeReason::ColdBoot;

  uint32_t bootCount = 0;
  uint32_t lastActivityMs = 0;
  uint32_t buttonWakeMs = 0; // Timestamp when device woke from button press
  uint32_t lastAlertMs = 0;  // Timestamp of last alert, for adaptive scanning

  // Device tracking for burst-based lock-on
  uint32_t trackingOui24 = 0;      // OUI of device we're tracking (0 = not tracking)
  uint64_t trackingAddr48 = 0;     // Full MAC of device we're tracking (0 = match by OUI only)
  uint32_t trackingLastSeenMs = 0; // When we last saw the tracked device
  uint32_t trackingNextBurstMs = 0; // When to start next tracking burst

  // True when USB CDC serial is opened by the host (best-effort).
  bool serialAttached = false;

  // Stats for UI
  uint32_t eventsDropped = 0;
  uint32_t wifiSightings = 0;
  uint32_t bleSightings  = 0;
  // Approximate all-time unique BLE devices seen (persisted across reboots).
  uint32_t bleUniqueAllTime = 0;
  uint32_t matches       = 0;
  uint32_t alertsFired   = 0;

  uint16_t ruleCount = 0;

  // Rule breakdown (enabled rules only). Used for richer UI summaries.
  uint16_t ruleCountOui = 0;
  uint16_t ruleCountCompanyId = 0;
  uint16_t ruleCountNameContains = 0;
  uint16_t ruleCountMac = 0;
  uint16_t ruleCountServiceUuid = 0;

  // Compact rule list (enabled rules only) for UI browsing.
  enum class RuleKind : uint8_t {
    Oui,
    CompanyId,
    NameContains,
    Mac,
    ServiceUuid,
  };
  static constexpr uint8_t kMaxRuleSummaries = 64;
  struct RuleSummary {
    uint32_t id = 0;
    RuleKind kind = RuleKind::Oui;
    bool enabled = true;
    // Pre-formatted short descriptor, e.g.
    // - "OUI 00:25:DF"
    // - "CID 0x034D"
    // - "Name AXON"
    // - "MAC F0F0F0123456"
    // - "Svc 180D..."
    char detail[20] = {0};
    char pack[16] = {0};
    char label[16] = {0};
  };
  uint8_t ruleSummaryCount = 0;
  RuleSummary ruleSummaries[kMaxRuleSummaries]{};

  // Rule pack summaries (from filesystem packs). Used by UI to list/toggle packs.
  static constexpr uint8_t kMaxRulePacks = 12;
  struct RulePackSummary {
    char name[16] = {0};
    bool enabled = true;
  };
  uint8_t rulePackCount = 0;
  RulePackSummary rulePacks[kMaxRulePacks]{};

  // Last alert info (for UI)
  uint32_t lastAlertTsMs = 0;
  uint32_t lastAlertRuleId = 0;
  int8_t lastAlertRssi = 0;
  // Snapshot of the matched rule at the time of the alert (so UI doesn't need RulesManager access).
  char lastAlertPack[16] = {0};
  char lastAlertLabel[16] = {0};
  bool lastAlertRuleEnabled = false;
  uint32_t lastAlertRuleOui24 = 0;
  uint64_t lastAlertRuleAddr48 = 0;
  uint16_t lastAlertRuleCompanyId = 0;
  char lastAlertRuleName[20] = {0};
  // Actual device data that triggered the alert (not the rule pattern).
  uint32_t lastAlertDeviceOui24 = 0;
  uint64_t lastAlertDeviceAddr48 = 0;
  uint16_t lastAlertDeviceCompanyId = 0;
  uint8_t lastAlertDeviceHasCompanyId = 0;
  char lastAlertDeviceName[20] = {0};

  // Last seen radio sightings (for UI/debug)
  uint32_t lastWifiTsMs = 0;
  uint32_t lastWifiOui24 = 0;
  int8_t lastWifiRssi = 0;

  uint32_t lastBleTsMs = 0;
  uint32_t lastBleOui24 = 0;
  int8_t lastBleRssi = 0;
  char lastBleName[20] = {0};
	uint8_t lastBleHasMsdCompanyId = 0;
	uint16_t lastBleMsdCompanyId = 0;
	char lastBleMfgName[20] = {0}; // Pre-resolved company name for status view

  // Devices/OUI summary table for the Devices menu (not persisted).
  static constexpr uint8_t kMaxDevices = 64;
  struct DeviceSummary {
    uint32_t oui24 = 0;
    int8_t rssi = 0;
    uint32_t lastSeenMs = 0;
    uint8_t hasMsdCompanyId = 0;
    uint16_t msdCompanyId = 0;
    char name[20] = {0};
    char companyName[20] = {0};
    bool seenBle = false;
    bool seenWifi = false;
  };
  uint8_t deviceCount = 0;
  DeviceSummary devices[kMaxDevices]{};

  // Persisted summary of detections that occurred while "sleeping".
  SleepScanStats sleepScan{};

  // Debug: counters stored in RTC memory across deep sleep (not NVS).
  // Useful to confirm whether we're timer-waking vs. button-waking vs. rebooting.
  uint32_t rtcTimerWakes = 0;
  uint32_t rtcButtonWakes = 0;
  uint32_t rtcColdBoots = 0;
  uint32_t rtcResetReason = 0; // esp_reset_reason_t as integer

  // Config
  Settings settings{};
};

// Back-compat alias (older code used AppState)
using AppState = CoreState;