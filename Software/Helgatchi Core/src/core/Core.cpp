#include "Core.h"
#include <Arduino.h>
#include <algorithm>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <esp_system.h>
#include "Config.h"

// Drivers
#include "../input/Buttons.h"
#include "../drivers/Display.h"
#include "../drivers/Backlight.h"
#include "../drivers/VibrationMotor.h"

// Data/Power
#include "../data/Store.h"
#include "../power/PowerManager.h"

// Core
#include "../scan/ScanCoordinator.h"
#include "../scan/BleScanner.h"
#include "../rules/RulesManager.h"
#include "../services/AlertEngine.h"
#include "../services/Telemetry.h"

// UI
#include "../ui/Ui.h"

// Forward declaration for BT company lookup (defined in Ui.cpp)
extern const char* btCompanyName(uint16_t companyId, bool allowSlowLookup);

static void dbgPrint(CoreState& state, const char* fmt, ...) {
  if (state.settings.debugLevel == 0) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

static void formatOui_(uint32_t oui24, char* out, size_t outLen) {
  if (!out || outLen < 9) return;
  const uint8_t b0 = (uint8_t)((oui24 >> 16) & 0xFFu);
  const uint8_t b1 = (uint8_t)((oui24 >> 8) & 0xFFu);
  const uint8_t b2 = (uint8_t)(oui24 & 0xFFu);
  snprintf(out, outLen, "%02X:%02X:%02X", (unsigned)b0, (unsigned)b1, (unsigned)b2);
}

static void formatMac48_(uint64_t addr48, char* out, size_t outLen) {
  if (!out || outLen < 18) return;
  addr48 &= 0xFFFFFFFFFFFFULL;
  const uint8_t b0 = (uint8_t)((addr48 >> 40) & 0xFFu);
  const uint8_t b1 = (uint8_t)((addr48 >> 32) & 0xFFu);
  const uint8_t b2 = (uint8_t)((addr48 >> 24) & 0xFFu);
  const uint8_t b3 = (uint8_t)((addr48 >> 16) & 0xFFu);
  const uint8_t b4 = (uint8_t)((addr48 >> 8) & 0xFFu);
  const uint8_t b5 = (uint8_t)(addr48 & 0xFFu);
  snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)b0, (unsigned)b1, (unsigned)b2,
           (unsigned)b3, (unsigned)b4, (unsigned)b5);
}

static const char* classifyBleAddr48_(uint64_t addr48) {
  // BLE random addresses encode their type in the two MSBs of the first octet:
  // 00 = non-resolvable private (NRPA)
  // 01 = resolvable private (RPA)
  // 11 = static random
  // 10 = reserved
  // Public addresses do not use this encoding, so if it looks like RPA/etc it strongly suggests privacy.
  addr48 &= 0xFFFFFFFFFFFFULL;
  if (addr48 == 0) return "--";
  const uint8_t first = (uint8_t)((addr48 >> 40) & 0xFFu);
  const uint8_t tag = (uint8_t)(first >> 6);
  switch (tag) {
    case 0:
      return "NRPA?";
    case 1:
      return "RPA?";
    case 3:
      return "STATIC?";
    default:
      return "RESV?";
  }
}

static bool isUsbSerialActive_() {
#if defined(ARDUINO_USB_CDC_ON_BOOT) || defined(ARDUINO_USB_MODE)
	// NOTE: This typically becomes true when the host opens the serial port (DTR).
	// It does not always detect “cable plugged in but port closed”.
	return (bool)Serial;
#else
	return false;
#endif
}

static void upsertDeviceSummary_(CoreState& state,
                 uint32_t oui24,
                 int8_t rssi,
                 uint32_t tsMs,
                 const char* name,
                 bool isBle,
                 uint8_t hasMsdCompanyId,
                 uint16_t msdCompanyId) {
  oui24 &= 0xFFFFFFu;
  if (oui24 == 0) return;

  // Find existing entry.
  for (uint8_t i = 0; i < state.deviceCount && i < CoreState::kMaxDevices; i++) {
    if (state.devices[i].oui24 == oui24) {
      state.devices[i].rssi = rssi;
      state.devices[i].lastSeenMs = tsMs;
      if (isBle) state.devices[i].seenBle = true;
      else state.devices[i].seenWifi = true;
			if (isBle && hasMsdCompanyId && msdCompanyId != 0) {
				state.devices[i].hasMsdCompanyId = 1;
				state.devices[i].msdCompanyId = msdCompanyId;
				// Cache-only lookup; only store if we get a real name (not "--")
				if (state.devices[i].companyName[0] == 0) {
					const char* company = btCompanyName(msdCompanyId, false);
					if (company[0] != '-' || company[1] != '-' || company[2] != 0) {
						strncpy(state.devices[i].companyName, company, sizeof(state.devices[i].companyName) - 1);
						state.devices[i].companyName[sizeof(state.devices[i].companyName) - 1] = 0;
					}
				}
			}
      if (name && name[0]) {
        strncpy(state.devices[i].name, name, sizeof(state.devices[i].name) - 1);
        state.devices[i].name[sizeof(state.devices[i].name) - 1] = 0;
      }
      return;
    }
  }

  // Insert new entry; evict oldest if full.
  uint8_t idx = state.deviceCount;
  if (idx < CoreState::kMaxDevices) {
    state.deviceCount++;
  } else {
    idx = 0;
    uint32_t oldest = state.devices[0].lastSeenMs;
    for (uint8_t i = 1; i < CoreState::kMaxDevices; i++) {
      if (state.devices[i].lastSeenMs <= oldest) {
        oldest = state.devices[i].lastSeenMs;
        idx = i;
      }
    }
  }

  state.devices[idx] = CoreState::DeviceSummary{};
  state.devices[idx].oui24 = oui24;
  state.devices[idx].rssi = rssi;
  state.devices[idx].lastSeenMs = tsMs;
  if (isBle) state.devices[idx].seenBle = true;
  else state.devices[idx].seenWifi = true;
	if (isBle && hasMsdCompanyId && msdCompanyId != 0) {
		state.devices[idx].hasMsdCompanyId = 1;
		state.devices[idx].msdCompanyId = msdCompanyId;
		// Cache-only lookup; only store if we get a real name (not "--")
		const char* company = btCompanyName(msdCompanyId, false);
		if (company[0] != '-' || company[1] != '-' || company[2] != 0) {
			strncpy(state.devices[idx].companyName, company, sizeof(state.devices[idx].companyName) - 1);
			state.devices[idx].companyName[sizeof(state.devices[idx].companyName) - 1] = 0;
		}
	}
  if (name && name[0]) {
    strncpy(state.devices[idx].name, name, sizeof(state.devices[idx].name) - 1);
    state.devices[idx].name[sizeof(state.devices[idx].name) - 1] = 0;
  }
}

static bool isCenterPressedNow_() {
  const int level = digitalRead(PIN_BTN_CENTER);
  #if BUTTON_ACTIVE_LOW
  return level == LOW;
  #else
  return level == HIGH;
  #endif
}

static bool isPressedNow_(int pin) {
  const int level = digitalRead(pin);
  #if BUTTON_ACTIVE_LOW
  return level == LOW;
  #else
  return level == HIGH;
  #endif
}

static bool leftRightHeldFor_(uint32_t holdMs) {
  const uint32_t start = millis();
  while ((millis() - start) < holdMs) {
    if (!isPressedNow_(PIN_BTN_LEFT) || !isPressedNow_(PIN_BTN_RIGHT)) return false;
    delay(10);
  }
  return true;
}

static bool centerHeldFor_(uint32_t holdMs) {
  const uint32_t start = millis();
  while ((millis() - start) < holdMs) {
    if (!isCenterPressedNow_()) return false;
    delay(10);
  }
  return true;
}

static uint64_t mix64_(uint64_t x) {
  // SplitMix64 finalizer-ish (good enough for hashing MACs).
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

Core::Core() : bus_(EVENT_QUEUE_CAPACITY), lastTickMs_(0) {}
Core::~Core() = default;

// RTC memory survives deep sleep resets (but not power loss).
RTC_DATA_ATTR static uint32_t g_rtcTimerWakes = 0;
RTC_DATA_ATTR static uint32_t g_rtcButtonWakes = 0;
RTC_DATA_ATTR static uint32_t g_rtcColdBoots = 0;
RTC_DATA_ATTR static uint32_t g_rtcLastResetReason = 0;

void Core::setup() {
	Serial.println("[core] setup begin");
  HELGA_PERF_START_IF(state_, setup_total);
  
  HELGA_PERF_START_IF(state_, init_subsystems);
  initSubsystems_();
  HELGA_PERF_END_IF(state_, init_subsystems);

  // Developer escape hatch: hold L+R at boot to clear persisted settings
  // (sleep timeout, wake interval, scan burst length, etc.) without wiping rules.
  if (leftRightHeldFor_(900)) {
    Serial.println("[core] reset settings (L+R held)");
    store_->clearSettings();
    store_->clearSleepScanStats();
  }

  state_.lastActivityMs = millis();
  state_.serialAttached = isUsbSerialActive_();

  // Load persisted settings/rules BEFORE deciding mode.
  HELGA_PERF_START_IF(state_, load_settings);
  store_->loadSettings(state_.settings);
  HELGA_PERF_END_IF(state_, load_settings);
  buttons_->setDebug(state_.settings.debugLevel > 0);
  telemetry_->setEnabled(state_.settings.debugLevel > 0);
  scan_->setDebug(state_.settings.debugLevel > 0);
  BleScanner::setDebugPerformance(state_.settings.debugLevel == 3);
  Display::setDebugPerformance(state_.settings.debugLevel == 3);
  RulesManager::setDebugPerformance(state_.settings.debugLevel == 3);
  backlight_->setBrightness(state_.settings.backlightLevel);
  dbgPrint(state_, "[core] settings loaded: debugLevel=%u", (unsigned)state_.settings.debugLevel);

  HELGA_PERF_START_IF(state_, load_rules);
  rules_->loadFromStore(*store_);
  {
    const std::vector<std::string> disabled = store_->loadDisabledPacks();
    rules_->loadPacksFromFs(disabled);
		const std::vector<PackRuleOverride> disabledRules = store_->loadDisabledPackRules();
		rules_->applyDisabledPackRules(disabledRules);
  }
  updateRuleCounts_();
  updateRuleSummaries_();
  updateRulePackSummary_();
  HELGA_PERF_END_IF(state_, load_rules);
  dbgPrint(state_, "[core] rules loaded: user=%u pack=%u total=%u",
           (unsigned)rules_->rules().size(), (unsigned)rules_->packRuleCount(), (unsigned)rules_->ruleCount());

  // Determine wake reason
  state_.wakeReason = power_->getWakeReason();
  state_.bootCount  = store_->incrementBootCount();

  // Record reset reason + wake reason in RTC memory so we can debug "did it timer wake?"
  // even when the screen stays off during sleep scan bursts.
  const uint32_t rr = (uint32_t)esp_reset_reason();
  g_rtcLastResetReason = rr;
  if (state_.wakeReason == WakeReason::Timer) {
    g_rtcTimerWakes++;
  } else if (state_.wakeReason == WakeReason::Button) {
    g_rtcButtonWakes++;
  } else {
    g_rtcColdBoots++;
  }
  state_.rtcTimerWakes = g_rtcTimerWakes;
  state_.rtcButtonWakes = g_rtcButtonWakes;
  state_.rtcColdBoots = g_rtcColdBoots;
  state_.rtcResetReason = g_rtcLastResetReason;

  // Load persisted "sleep scan" summary (detections found while deep sleeping).
  store_->loadSleepScanStats(state_.sleepScan);

  store_->loadBleUniqueSketch(bleHll_, sizeof(bleHll_));
  state_.bleUniqueAllTime = estimateBleUnique_();
  dbgPrint(state_, "[core] ble unique all-time ~= %lu", (unsigned long)state_.bleUniqueAllTime);

	Serial.print("[core] wakeReason=");
	Serial.print((int)state_.wakeReason);
  Serial.print(" resetReason=");
  Serial.print((unsigned long)state_.rtcResetReason);
	Serial.print(" bootCount=");
	Serial.println((unsigned long)state_.bootCount);

  // Deep-sleep wake can only be edge/level. To implement "long-press wake",
  // we wake on the center button and then require it to remain held.
  if (state_.wakeReason == WakeReason::Button) {
    const bool ok = centerHeldFor_(700);
    if (!ok) {
      dbgPrint(state_, "[power] woke on button but not held long; returning to sleep");
      enterPreSleep_();
      return; // unreachable (deep sleep), but keeps flow obvious
    }
    // Record when we woke from button to prevent immediate re-sleep
    state_.buttonWakeMs = millis();
    
    // Give user time to release the wake button, then clear button states.
    // Without this, the first poll() after wake might see the release as a
    // new press event, or the debouncing might interfere with subsequent presses.
    delay(100);
    // Drain any button events that accumulated during the hold check.
    // We poll once to sync button state, then discard all pending events.
    buttons_->poll(bus_);
    Event dummy{};
    while (bus_.pop(dummy)) { /* discard */ }
  }

  postInitialEvents_();

  // Decide startup path
  if (state_.wakeReason == WakeReason::Timer) {
    enterScanBurst_();
  } else {
    enterInteractive_();
  }

  vibe_->playBootPattern();
  HELGA_PERF_END_IF(state_, setup_total);
	Serial.println("[core] setup done");
}

void Core::updateRulePackSummary_() {
  state_.rulePackCount = 0;
  for (uint8_t i = 0; i < CoreState::kMaxRulePacks; i++) {
    state_.rulePacks[i].name[0] = 0;
    state_.rulePacks[i].enabled = true;
  }

  RulesManager::PackInfo packs[CoreState::kMaxRulePacks];
  const size_t n = rules_ ? rules_->getPackInfo(packs, CoreState::kMaxRulePacks) : 0;
  const uint8_t count = (n > CoreState::kMaxRulePacks) ? CoreState::kMaxRulePacks : (uint8_t)n;
  state_.rulePackCount = count;
  for (uint8_t i = 0; i < count; i++) {
    memcpy(state_.rulePacks[i].name, packs[i].name, sizeof(state_.rulePacks[i].name));
    state_.rulePacks[i].name[sizeof(state_.rulePacks[i].name) - 1] = 0;
    state_.rulePacks[i].enabled = packs[i].enabled;
  }
}

void Core::updateRuleCounts_() {
  state_.ruleCount = (uint16_t)(rules_ ? rules_->enabledRuleCount() : 0);

  state_.ruleCountOui = 0;
  state_.ruleCountCompanyId = 0;
  state_.ruleCountNameContains = 0;
  state_.ruleCountMac = 0;
  state_.ruleCountServiceUuid = 0;

  if (!rules_) return;
  const RulesManager::RuleTypeCounts c = rules_->countRuleTypes(true);
  state_.ruleCountOui = c.oui;
  state_.ruleCountCompanyId = c.companyId;
  state_.ruleCountNameContains = c.nameContains;
  state_.ruleCountMac = c.mac;
  state_.ruleCountServiceUuid = c.serviceUuid;
}

void Core::updateRuleSummaries_() {
  state_.ruleSummaryCount = 0;
  for (uint8_t i = 0; i < CoreState::kMaxRuleSummaries; i++) {
    state_.ruleSummaries[i] = CoreState::RuleSummary{};
  }

  if (!rules_) return;

  auto add = [&](const OuiRule& r) {
    if (state_.ruleSummaryCount >= CoreState::kMaxRuleSummaries) return;

    CoreState::RuleSummary s{};
    s.id = r.id;
    s.enabled = r.enabled;

    if (r.addr48 != 0) {
      s.kind = CoreState::RuleKind::Mac;
      const unsigned long long mac = (unsigned long long)(r.addr48 & 0xFFFFFFFFFFFFULL);
      snprintf(s.detail, sizeof(s.detail), "MAC %012llX", mac);
    } else if (r.hasServiceUuid) {
      s.kind = CoreState::RuleKind::ServiceUuid;
      snprintf(s.detail, sizeof(s.detail), "Svc %02X%02X%02X%02X",
               (unsigned)r.serviceUuid128[0],
               (unsigned)r.serviceUuid128[1],
               (unsigned)r.serviceUuid128[2],
               (unsigned)r.serviceUuid128[3]);
    } else if (r.hasCompanyId) {
      s.kind = CoreState::RuleKind::CompanyId;
      snprintf(s.detail, sizeof(s.detail), "CID 0x%04X", (unsigned)r.companyId);
    } else if (r.nameContains[0]) {
      s.kind = CoreState::RuleKind::NameContains;
      snprintf(s.detail, sizeof(s.detail), "Name %.14s", r.nameContains);
    } else if ((r.oui24 & 0xFFFFFFu) != 0) {
      s.kind = CoreState::RuleKind::Oui;
      char oui[10];
      formatOui_((uint32_t)(r.oui24 & 0xFFFFFFu), oui, sizeof(oui));
      snprintf(s.detail, sizeof(s.detail), "OUI %s", oui);
    } else {
      return;
    }

    memcpy(s.pack, r.pack, sizeof(s.pack));
    s.pack[sizeof(s.pack) - 1] = 0;
    memcpy(s.label, r.label, sizeof(s.label));
    s.label[sizeof(s.label) - 1] = 0;

    state_.ruleSummaries[state_.ruleSummaryCount++] = s;
  };

  for (const auto& r : rules_->rules()) add(r);
  for (const auto& r : rules_->packRules()) add(r);
}

void Core::loop() {
  HELGA_PERF_START_IF(state_, loop_total);
  
  const bool wasSerialAttached = state_.serialAttached;
  state_.serialAttached = isUsbSerialActive_();
  if (wasSerialAttached && !state_.serialAttached) {
    // Treat USB detach as "activity" so we don't instantly sleep if the timer
    // expired while USB was connected.
    state_.lastActivityMs = millis();
    dbgPrint(state_, "[power] USB serial detached; reset sleep timer");
  }

  // Poll “producers” (buttons/scanners) that may push events
  buttons_->poll(bus_);
  scan_->poll(bus_);        // will emit sightings when implemented
  vibe_->tick(millis());    // non-blocking vibration pattern engine

  tick_();

  // Drain event bus
  HELGA_PERF_START_IF(state_, event_drain);
  Event e{};
  uint32_t eventCount = 0;
  while (bus_.pop(e)) {
    HELGA_PERF_START_IF(state_, event_handle);
    handleEvent_(e);
    HELGA_PERF_END_IF(state_, event_handle);
    eventCount++;
  }
  HELGA_PERF_END_IF(state_, event_drain);

  // UI render (dirty-based inside Ui)
  HELGA_PERF_START_IF(state_, ui_tick);
  ui_->tick(state_, bus_);
  HELGA_PERF_END_IF(state_, ui_tick);

	// Persist background stats
	maybePersistBleUnique_(millis());

  // Power policy
  maybeSleep_();
  
  HELGA_PERF_END_IF(state_, loop_total);
}

void Core::initSubsystems_() {
  // Drivers
  Buttons::Pull pull = Buttons::Pull::PullUp;
  #if BUTTON_PULL_MODE == 0
  pull = Buttons::Pull::None;
  #elif BUTTON_PULL_MODE == 2
  pull = Buttons::Pull::PullDown;
  #else
  pull = Buttons::Pull::PullUp;
  #endif

  const bool activeLow = (BUTTON_ACTIVE_LOW != 0);
  buttons_   = new Buttons(PIN_BTN_LEFT, PIN_BTN_RIGHT, PIN_BTN_CENTER, pull, activeLow);
  display_   = new Display();
  backlight_ = new Backlight(PIN_BACKLIGHT);
  vibe_      = new VibrationMotor(PIN_VIB);

  // Core
  store_     = new Store();
  power_     = new PowerManager();
  scan_      = new ScanCoordinator();     // owns WiFiScanner + BleScanner
  rules_     = new RulesManager();
  alerts_    = new AlertEngine();
  telemetry_ = new Telemetry();

  // Init order matters
  store_->begin();
  scan_->begin();
  display_->begin();
  backlight_->begin();
  vibe_->begin();
  buttons_->begin();
  telemetry_->begin();

  // UI depends on display/backlight
  ui_ = new Ui(*display_, *backlight_);
  ui_->begin();
}

void Core::postInitialEvents_() {
  // Post wake event so subsystems/apps can react uniformly
  Event e{};
  e.type = EventType::Wake;
  e.wake.reason = (state_.wakeReason == WakeReason::Timer) ? WakeEventReason::Timer :
                 (state_.wakeReason == WakeReason::Button) ? WakeEventReason::Button :
                 (state_.wakeReason == WakeReason::Alert) ? WakeEventReason::Alert :
                                                           WakeEventReason::ColdBoot;
  bus_.push(e);
}

void Core::tick_() {
  const uint32_t now = millis();

  // Check if we need to start a tracking burst
  if (state_.trackingOui24 != 0) {
    // Check if tracking has expired
    if ((now - state_.trackingLastSeenMs) > state_.settings.trackingDurationMs) {
      dbgPrint(state_, "[track] tracking expired, stopping");
      state_.trackingOui24 = 0;
      state_.trackingAddr48 = 0;
      state_.trackingLastSeenMs = 0;
      state_.trackingNextBurstMs = 0;
    }
    // Check if it's time for next burst
    else if (now >= state_.trackingNextBurstMs) {
      // Only start burst if not already in an active scanning mode
      if (state_.mode == SystemMode::IdleAwake) {
        dbgPrint(state_, "[track] starting tracking burst");
        enterScanBurst_();
      }
      // Schedule next burst
      state_.trackingNextBurstMs = now + state_.settings.trackingBurstIntervalMs;
    }
  }

  if (now - lastTickMs_ >= 50) { // 20 Hz internal tick
    lastTickMs_ = now;
    Event e{};
    e.type = EventType::Tick;
    bus_.push(e);
  }
}

void Core::handleEvent_(const Event& e) {
  // Keep global drop count mirrored into state for UI
  state_.eventsDropped = bus_.droppedCount();

  switch (e.type) {
    case EventType::Wake:
      // TODO: record wake time, maybe pulse vibe briefly
      break;

    case EventType::AlertUiDismissed:
      if (state_.mode == SystemMode::Alerting) {
        state_.mode = SystemMode::Interactive;
      }
      break;

    case EventType::Button:
      state_.lastActivityMs = millis();
		dbgPrint(state_, "[evt] button id=%u action=%u", (unsigned)e.button.id, (unsigned)e.button.action);
    // If we were in the low-power "awake but idle" mode, treat any button
    // interaction as a request to return to full interactive scanning.
    if (state_.mode == SystemMode::IdleAwake) {
      enterInteractive_();
    }
      // Hand button to UI (UI will change screen/mode via events/requests)
      ui_->onButton(e.button, state_, bus_);
      break;

    case EventType::WifiSighting:
      {
      state_.wifiSightings++;
      state_.lastWifiTsMs = e.wifi.tsMs;
      state_.lastWifiOui24 = e.wifi.oui24;
      state_.lastWifiRssi = e.wifi.rssi;

		// Check if this is the device we're tracking
		if (state_.trackingOui24 != 0 && e.wifi.oui24 == state_.trackingOui24) {
			state_.trackingLastSeenMs = millis();
			dbgPrint(state_, "[track] WiFi device seen, extending tracking");
		}
    upsertDeviceSummary_(state_, e.wifi.oui24, (int8_t)e.wifi.rssi, e.wifi.tsMs, nullptr, false, 0, 0);

		// If this boot is a timer-wake scan burst, count it toward "asleep" detections.
		if (state_.wakeReason == WakeReason::Timer) {
			state_.sleepScan.wifiHits++;
			state_.sleepScan.lastWifiOui24 = e.wifi.oui24;
			state_.sleepScan.lastWifiRssi = e.wifi.rssi;
		}

    {
      char oui[10];
      formatOui_(e.wifi.oui24, oui, sizeof(oui));
      dbgPrint(state_, "[evt] wifi oui=%s rssi=%d", oui, (int)e.wifi.rssi);
    }
      HELGA_PERF_START_IF(state_, alert_wifi_check);
      alerts_->onWifiSighting(e.wifi, *rules_, state_, bus_);
      HELGA_PERF_END_IF(state_, alert_wifi_check);
      telemetry_->onWifiSighting(e.wifi);
      }
      break;

    case EventType::BleSighting:
      {
      state_.bleSightings++;
		updateBleUnique_(e.ble.addr48);
      state_.lastBleTsMs = e.ble.tsMs;
      state_.lastBleOui24 = e.ble.oui24;
      state_.lastBleRssi = e.ble.rssi;
		state_.lastBleHasMsdCompanyId = (uint8_t)((e.ble.hasMsdCompanyId && e.ble.msdCompanyId != 0) ? 1 : 0);
		state_.lastBleMsdCompanyId = state_.lastBleHasMsdCompanyId ? e.ble.msdCompanyId : 0;
		// Pre-populate company name ONLY if already cached (don't trigger file I/O here!)
		if (state_.lastBleMsdCompanyId != 0) {
			const char* mfgName = btCompanyName(state_.lastBleMsdCompanyId, false); // false = cache-only
			strncpy(state_.lastBleMfgName, mfgName, sizeof(state_.lastBleMfgName) - 1);
			state_.lastBleMfgName[sizeof(state_.lastBleMfgName) - 1] = 0;
		} else {
			state_.lastBleMfgName[0] = 0;
		}
  		memcpy(state_.lastBleName, e.ble.name, sizeof(state_.lastBleName));
  		state_.lastBleName[sizeof(state_.lastBleName) - 1] = 0;

		// Check if this is the device we're tracking
		if (state_.trackingOui24 != 0) {
			const bool matches = (e.ble.oui24 == state_.trackingOui24) &&
			                     (state_.trackingAddr48 == 0 || e.ble.addr48 == state_.trackingAddr48);
			if (matches) {
				state_.trackingLastSeenMs = millis();
				dbgPrint(state_, "[track] device seen, extending tracking");
			}
		}
    upsertDeviceSummary_(state_, e.ble.oui24, (int8_t)e.ble.rssi, e.ble.tsMs, state_.lastBleName, true,
                 e.ble.hasMsdCompanyId, e.ble.msdCompanyId);

    // If this boot is a timer-wake scan burst, count it toward "asleep" detections.
    if (state_.wakeReason == WakeReason::Timer) {
      state_.sleepScan.bleHits++;
      state_.sleepScan.lastBleOui24 = e.ble.oui24;
      state_.sleepScan.lastBleRssi = e.ble.rssi;
      memcpy(state_.sleepScan.lastBleName, e.ble.name, sizeof(state_.sleepScan.lastBleName));
      state_.sleepScan.lastBleName[sizeof(state_.sleepScan.lastBleName) - 1] = 0;
    }

    {
      char oui[10];
      char mac[18];
      formatOui_(e.ble.oui24, oui, sizeof(oui));
      formatMac48_(e.ble.addr48, mac, sizeof(mac));
      dbgPrint(state_, "[evt] ble  mac=%s (%s) oui=%s rssi=%d name=%.16s", mac, classifyBleAddr48_(e.ble.addr48), oui, (int)e.ble.rssi,
               e.ble.name[0] ? e.ble.name : "--");
    }
      HELGA_PERF_START_IF(state_, alert_ble_check);
      alerts_->onBleSighting(e.ble, *rules_, state_, bus_);
      HELGA_PERF_END_IF(state_, alert_ble_check);
      telemetry_->onBleSighting(e.ble);
      }
      break;

    case EventType::AddOuiRule: {
      const uint32_t oui24 = e.addOuiRule.oui24 & 0xFFFFFFu;
      if (oui24 == 0) {
        dbgPrint(state_, "[rules] ignoring add rule for OUI=000000");
        break;
      }

      char label[16];
      const char* prefix = (e.addOuiRule.source == OuiRuleSource::Ble) ? "BLE" : "WIFI";
      char oui[10];
      formatOui_(oui24, oui, sizeof(oui));
      snprintf(label, sizeof(label), "%s %s", prefix, oui);
      const uint32_t id = rules_->addOui(oui24, label, true);
      rules_->saveToStore(*store_);
    updateRuleCounts_();
    updateRuleSummaries_();
  		{
  			char oui2[10];
  			formatOui_(oui24, oui2, sizeof(oui2));
  			dbgPrint(state_, "[rules] added id=%lu oui=%s label=%s", (unsigned long)id, oui2, label);
  		}

      Event changed{};
      changed.type = EventType::RulesChanged;
      bus_.push(changed);
      break;
    }

    case EventType::AddMacRule: {
      const uint64_t addr48 = e.addMacRule.addr48 & 0xFFFFFFFFFFFFULL;
      if (addr48 == 0) {
        dbgPrint(state_, "[rules] ignoring add rule for MAC=000000000000");
        break;
      }

      char label[16];
      if (e.addMacRule.name[0]) {
        snprintf(label, sizeof(label), "%.13s", e.addMacRule.name);
      } else {
        snprintf(label, sizeof(label), "MAC");
      }
      const uint32_t id = rules_->addMac(addr48, label, true);
      rules_->saveToStore(*store_);
      updateRuleCounts_();
      updateRuleSummaries_();
      dbgPrint(state_, "[rules] added MAC id=%lu label=%s", (unsigned long)id, label);

      Event changed{};
      changed.type = EventType::RulesChanged;
      bus_.push(changed);
      break;
    }

    case EventType::AddCompanyRule: {
      const uint16_t companyId = e.addCompanyRule.companyId;
      if (companyId == 0) {
        dbgPrint(state_, "[rules] ignoring add rule for Company=0");
        break;
      }

      char label[16];
      if (e.addCompanyRule.name[0]) {
        snprintf(label, sizeof(label), "%.13s", e.addCompanyRule.name);
      } else {
        snprintf(label, sizeof(label), "Co %04X", companyId);
      }
      const uint32_t id = rules_->addCompany(companyId, label, true);
      rules_->saveToStore(*store_);
      updateRuleCounts_();
      updateRuleSummaries_();
      dbgPrint(state_, "[rules] added Company id=%lu cid=%u label=%s", (unsigned long)id, companyId, label);

      Event changed{};
      changed.type = EventType::RulesChanged;
      bus_.push(changed);
      break;
    }

    case EventType::AddNameRule: {
      if (!e.addNameRule.name[0]) {
        dbgPrint(state_, "[rules] ignoring add rule for empty name");
        break;
      }

      const uint32_t id = rules_->addName(e.addNameRule.name, e.addNameRule.name, true);
      rules_->saveToStore(*store_);
      updateRuleCounts_();
      updateRuleSummaries_();
      dbgPrint(state_, "[rules] added Name id=%lu name=%s", (unsigned long)id, e.addNameRule.name);

      Event changed{};
      changed.type = EventType::RulesChanged;
      bus_.push(changed);
      break;
    }

    case EventType::AlertFired:
      state_.alertsFired++;
		state_.lastAlertTsMs = e.alert.tsMs;
		state_.lastAlertRuleId = e.alert.ruleId;
		state_.lastAlertRssi = e.alert.rssi;
		state_.lastAlertMs = millis();
		// Start tracking the device that alerted
		state_.trackingOui24 = state_.lastBleOui24;
		state_.trackingAddr48 = 0; // Track by OUI for now (BLE addr can be random)
		state_.trackingLastSeenMs = millis();
		state_.trackingNextBurstMs = millis() + state_.settings.trackingBurstIntervalMs;
		dbgPrint(state_, "[track] started tracking OUI %06X", (unsigned)state_.trackingOui24);
      {
        const OuiRule* r = rules_ ? rules_->findById(e.alert.ruleId) : nullptr;
        if (r) {
          memcpy(state_.lastAlertPack, r->pack, sizeof(state_.lastAlertPack));
          state_.lastAlertPack[sizeof(state_.lastAlertPack) - 1] = 0;
          memcpy(state_.lastAlertLabel, r->label, sizeof(state_.lastAlertLabel));
          state_.lastAlertLabel[sizeof(state_.lastAlertLabel) - 1] = 0;
          state_.lastAlertRuleEnabled = r->enabled;
          state_.lastAlertRuleOui24 = r->oui24 & 0xFFFFFFu;
          state_.lastAlertRuleAddr48 = r->addr48 & 0xFFFFFFFFFFFFULL;
				state_.lastAlertRuleCompanyId = r->hasCompanyId ? r->companyId : 0;
          memcpy(state_.lastAlertRuleName, r->nameContains, sizeof(state_.lastAlertRuleName));
          state_.lastAlertRuleName[sizeof(state_.lastAlertRuleName) - 1] = 0;
        } else {
          state_.lastAlertPack[0] = 0;
          state_.lastAlertLabel[0] = 0;
          state_.lastAlertRuleEnabled = false;
          state_.lastAlertRuleOui24 = 0;
          state_.lastAlertRuleAddr48 = 0;
				state_.lastAlertRuleCompanyId = 0;
          state_.lastAlertRuleName[0] = 0;
        }        // Capture actual device data at time of alert
        state_.lastAlertDeviceOui24 = state_.lastBleOui24;
        state_.lastAlertDeviceAddr48 = 0; // BLE addr not stored in state
        state_.lastAlertDeviceCompanyId = state_.lastBleMsdCompanyId;
        state_.lastAlertDeviceHasCompanyId = state_.lastBleHasMsdCompanyId;
        memcpy(state_.lastAlertDeviceName, state_.lastBleName, sizeof(state_.lastAlertDeviceName));
        state_.lastAlertDeviceName[sizeof(state_.lastAlertDeviceName) - 1] = 0;      }
		dbgPrint(state_, "[evt] alert rule=%lu rssi=%d", (unsigned long)e.alert.ruleId, (int)e.alert.rssi);
      // Outputs are policy-driven by settings
      if (state_.settings.alertVibe) vibe_->playAlertPattern();
      if (state_.settings.alertScreen) {
        const bool alertUiActive = ui_->showAlertOverlay(e.alert, state_);
        // Don't disturb ScanBurst, since it drives burst completion behavior.
        if (alertUiActive && state_.mode == SystemMode::Interactive) {
          state_.mode = SystemMode::Alerting;
        }
      }
      telemetry_->onAlertFired(e.alert);
      break;

    case EventType::ToggleRulePack: {
      const uint8_t idx = e.togglePack.index;
      if (idx >= state_.rulePackCount) break;
      const char* pack = state_.rulePacks[idx].name;
      if (!pack || !*pack) break;
      const bool newEnabled = e.togglePack.enabled;
      if (rules_) {
        rules_->setPackEnabled(pack, newEnabled);
      }
      state_.rulePacks[idx].enabled = newEnabled;
		if (newEnabled) {
			// Master toggle ON should enable everything in the pack.
			// Clear persisted per-rule disables so they don't re-apply on next boot.
			store_->clearDisabledPackRulesForPack(pack);
		}
    updateRuleCounts_();
    updateRuleSummaries_();

      // Persist via disabled-pack list
      std::vector<std::string> disabled = store_->loadDisabledPacks();
      auto exists = [&](const std::string& s) {
        for (const auto& d : disabled) {
          if (d == s) return true;
        }
        return false;
      };
      const std::string p(pack);
      if (!newEnabled) {
        if (!exists(p)) disabled.push_back(p);
      } else {
        disabled.erase(std::remove(disabled.begin(), disabled.end(), p), disabled.end());
      }
      store_->saveDisabledPacks(disabled);
      dbgPrint(state_, "[rules] pack %s => %s", pack, newEnabled ? "EN" : "DIS");
      break;
    }

    case EventType::ToggleRule: {
      if (!rules_) break;
      bool isPackRule = false;
      const OuiRule* r = rules_->findById(e.toggleRule.id, &isPackRule);
      if (!r) break;
      const bool newEnabled = e.toggleRule.enabled;

      if (isPackRule) {
        // Pack master toggle rules: if the pack is disabled, don't allow individual enables.
        if (!rules_->isPackEnabled(r->pack)) break;
        const uint64_t sig = RulesManager::packRuleSignature(*r);
        rules_->toggleRule(r->id, newEnabled);
        store_->setPackRuleDisabled(r->pack, sig, !newEnabled);
      } else {
        rules_->toggleRule(r->id, newEnabled);
        rules_->saveToStore(*store_);
      }

      // Update the last alert rule enabled state if this is the current alert rule
      if (state_.lastAlertRuleId == e.toggleRule.id) {
        state_.lastAlertRuleEnabled = newEnabled;
        // Stop tracking if we disabled the alert rule
        if (!newEnabled && state_.trackingOui24 != 0) {
          dbgPrint(state_, "[track] stopping tracking - alert rule disabled");
          state_.trackingOui24 = 0;
          state_.trackingAddr48 = 0;
          state_.trackingLastSeenMs = 0;
          state_.trackingNextBurstMs = 0;
        }
      }

      // Start tracking mode if any rule is enabled
      if (newEnabled) {
        const uint32_t now = millis();
        dbgPrint(state_, "[track] starting tracking mode - rule enabled: id=%u mode=%u", (unsigned)r->id, (unsigned)state_.mode);
        // Set up tracking state to scan for all devices (OUI 0xFFFFFF = wildcard)
        // This will trigger burst scans via tick_() like when an alert fires
        state_.trackingOui24 = 0xFFFFFF; // Wildcard - scan for everything
        state_.trackingAddr48 = 0;
        state_.trackingLastSeenMs = now;
        state_.trackingNextBurstMs = now; // Trigger immediately in tick_()
        // Force transition to IdleAwake so tick_() can start bursts
        if (state_.mode == SystemMode::Interactive) {
          enterIdleAwake_();
        }
        dbgPrint(state_, "[track] tracking setup complete, tick_() will start burst");
      }

      updateRuleCounts_();
      updateRuleSummaries_();
      break;
    }

    case EventType::WipeAction: {
      switch (e.wipe.kind) {
        case WipeActionKind::LocalStats: {
          // Persisted stats
          store_->clearSleepScanStats();
          store_->clearBleUniqueSketch();
          store_->clearBootCount();

          // Runtime stats
          state_.eventsDropped = 0;
          state_.wifiSightings = 0;
          state_.bleSightings = 0;
          state_.matches = 0;
          state_.alertsFired = 0;
			updateRuleCounts_();
      updateRuleSummaries_();

          state_.lastWifiTsMs = 0;
          state_.lastWifiOui24 = 0;
          state_.lastWifiRssi = 0;
          state_.lastBleTsMs = 0;
          state_.lastBleOui24 = 0;
          state_.lastBleRssi = 0;
          state_.lastBleName[0] = 0;
			state_.lastBleHasMsdCompanyId = 0;
			state_.lastBleMsdCompanyId = 0;

			  state_.deviceCount = 0;
			  for (auto& d : state_.devices) d = CoreState::DeviceSummary{};

          state_.sleepScan = SleepScanStats{};
          state_.lastAlertTsMs = 0;
          state_.lastAlertRuleId = 0;
          state_.lastAlertRssi = 0;
          state_.lastAlertPack[0] = 0;
          state_.lastAlertLabel[0] = 0;
          state_.lastAlertRuleEnabled = false;
          state_.lastAlertRuleOui24 = 0;
          state_.lastAlertRuleAddr48 = 0;
          state_.lastAlertRuleName[0] = 0;

          // RTC counters (deep sleep debug)
          g_rtcTimerWakes = 0;
          g_rtcButtonWakes = 0;
          g_rtcColdBoots = 0;
          g_rtcLastResetReason = 0;
          state_.rtcTimerWakes = 0;
          state_.rtcButtonWakes = 0;
          state_.rtcColdBoots = 0;
          state_.rtcResetReason = 0;

          // BLE unique sketch
          memset(bleHll_, 0, sizeof(bleHll_));
          bleHllDirty_ = false;
          state_.bleUniqueAllTime = 0;
          lastBleHllSaveMs_ = 0;

          dbgPrint(state_, "[wipe] local stats cleared");
          break;
        }
        case WipeActionKind::CustomRules: {
          store_->clearRules();
          if (rules_) {
            rules_->loadFromStore(*store_);
            const std::vector<std::string> disabled = store_->loadDisabledPacks();
            rules_->loadPacksFromFs(disabled);
					const std::vector<PackRuleOverride> disabledRules = store_->loadDisabledPackRules();
					rules_->applyDisabledPackRules(disabledRules);
				updateRuleCounts_();
				updateRuleSummaries_();
            updateRulePackSummary_();
          }
          dbgPrint(state_, "[wipe] custom rules cleared");
          break;
        }
        case WipeActionKind::ResetSettings: {
          store_->clearSettings();
          state_.settings = Settings{};
          buttons_->setDebug(state_.settings.debugLevel > 0);
          telemetry_->setEnabled(state_.settings.debugLevel > 0);
          scan_->setDebug(state_.settings.debugLevel > 0);
          BleScanner::setDebugPerformance(state_.settings.debugLevel == 3);
          Display::setDebugPerformance(state_.settings.debugLevel == 3);
          RulesManager::setDebugPerformance(state_.settings.debugLevel == 3);
          dbgPrint(state_, "[wipe] settings reset to defaults");
          break;
        }
        default:
          break;
      }
      break;
    }

    case EventType::SettingsChanged:
      store_->saveSettings(state_.settings);
  		buttons_->setDebug(state_.settings.debugLevel > 0);
    telemetry_->setEnabled(state_.settings.debugLevel > 0);
    scan_->setDebug(state_.settings.debugLevel > 0);
    BleScanner::setDebugPerformance(state_.settings.debugLevel == 3);
    Display::setDebugPerformance(state_.settings.debugLevel == 3);
    RulesManager::setDebugPerformance(state_.settings.debugLevel == 3);
    backlight_->setBrightness(state_.settings.backlightLevel);
    Serial.print("[core] settings saved: debugLevel=");
    Serial.println((unsigned)state_.settings.debugLevel);
      break;

    case EventType::RulesChanged:
      rules_->saveToStore(*store_);
      break;

    case EventType::ForceSleep:
      dbgPrint(state_, "[core] ForceSleep requested");
      // Prevent immediate re-sleep if we just woke from button press
      if (state_.buttonWakeMs != 0 && (millis() - state_.buttonWakeMs) < 1000) {
        dbgPrint(state_, "[core] Ignoring ForceSleep - just woke from button");
        break;
      }
      // Clear sleep scan stats before sleeping
      state_.sleepScan = SleepScanStats{};
      store_->saveSleepScanStats(state_.sleepScan);
      // Directly enter PreSleep, bypassing USB serial check
      enterPreSleep_();
      break;

    case EventType::Tick:
    default:
      break;
  }
}

void Core::updateBleUnique_(uint64_t addr48) {
  addr48 &= 0xFFFFFFFFFFFFULL;
  if (addr48 == 0) return;

  uint64_t h = mix64_(addr48);
  const uint32_t mMask = (uint32_t)kBleHllM_ - 1u;
  const uint32_t idx = (uint32_t)(h & (uint64_t)mMask);
  uint64_t w = h >> kBleHllP_;
  uint8_t rank;
  if (w == 0) {
    rank = (uint8_t)((64 - kBleHllP_) + 1);
  } else {
    rank = (uint8_t)(__builtin_ctzll(w) + 1);
  }
  if (rank > 63) rank = 63;

  if (rank > bleHll_[idx]) {
    bleHll_[idx] = rank;
    bleHllDirty_ = true;
    state_.bleUniqueAllTime = estimateBleUnique_();
  }
}

uint32_t Core::estimateBleUnique_() const {
  const double m = (double)kBleHllM_;
  const double alpha = 0.7213 / (1.0 + (1.079 / m));

  double sum = 0.0;
  uint32_t zeros = 0;
  for (size_t i = 0; i < kBleHllM_; i++) {
    const uint8_t r = bleHll_[i];
    if (r == 0) zeros++;
    sum += ldexp(1.0, -(int)r);
  }

  if (sum <= 0.0) return 0;
  double est = alpha * m * m / sum;

  // Small-range correction.
  if (est <= (2.5 * m) && zeros > 0) {
    est = m * log(m / (double)zeros);
  }

  if (est < 0.0) est = 0.0;
  if (est > 4294967295.0) est = 4294967295.0;
  return (uint32_t)(est + 0.5);
}

void Core::maybePersistBleUnique_(uint32_t nowMs) {
  // Throttle NVS writes.
  if (!bleHllDirty_) return;
  if ((nowMs - lastBleHllSaveMs_) < 15000) return;
  lastBleHllSaveMs_ = nowMs;
  store_->saveBleUniqueSketch(bleHll_, sizeof(bleHll_));
  bleHllDirty_ = false;
}

void Core::enterInteractive_() {
  const SystemMode prevMode = state_.mode;
  state_.mode = SystemMode::Interactive;
	dbgPrint(state_, "[core] enterInteractive");
  backlight_->setOn(true);
  scan_->setModeInteractive(state_.settings); // start higher duty scan
  // If we were in a timer scan burst and an alert fired, Ui::showAlertOverlay()
  // already switched to Alerts + started the red header flash. Preserve that view
  // instead of resetting back to Status.
  if (!(prevMode == SystemMode::ScanBurst && state_.settings.alertScreen && state_.alertsFired > 0)) {
    ui_->setRootScreenMain();
  }
}

void Core::enterIdleAwake_() {
  state_.mode = SystemMode::IdleAwake;
	dbgPrint(state_, "[core] enterIdleAwake");
  backlight_->setDimmed(true);
  scan_->setModeIdle(state_.settings); // reduced duty or stop
}

void Core::enterScanBurst_() {
  state_.mode = SystemMode::ScanBurst;
	dbgPrint(state_, "[core] enterScanBurst burstMs=%lu", (unsigned long)state_.settings.scanBurstMs);
  backlight_->setOn(false);

  // Each timer wake that enters scan burst counts as one "asleep scan" burst.
  if (state_.wakeReason == WakeReason::Timer) {
    state_.sleepScan.bursts++;
    // Persist immediately so you can confirm timer wakes are happening even
    // if the burst sees 0 devices (or we reset before burst completion).
    store_->saveSleepScanStats(state_.sleepScan);
  }

  scan_->startBurst(state_.settings.scanBurstMs, state_.settings);
}

void Core::enterPreSleep_() {
  state_.mode = SystemMode::PreSleep;
	dbgPrint(state_, "[core] enterPreSleep wakeIntervalMs=%lu", (unsigned long)state_.settings.wakeIntervalMs);
  scan_->stopAll();
  store_->flush();
  backlight_->setOn(false);
	// Configure wake sources + sleep interval.
	// Use center button as the only wake button. For "long press wake",
	// Core::setup() requires it to be held for a short time after boot.
	const bool wakeActiveLow = (BUTTON_ACTIVE_LOW != 0);
	power_->sleepForMs(state_.settings.wakeIntervalMs, PIN_BTN_CENTER, wakeActiveLow, (uint8_t)BUTTON_PULL_MODE);
}

void Core::maybeSleep_() {
  const uint32_t now = millis();

  if (state_.mode == SystemMode::Interactive || state_.mode == SystemMode::Alerting) {
    // If we're tracking a device, transition to IdleAwake to allow burst scanning
    if (state_.trackingOui24 != 0) {
      dbgPrint(state_, "[track] transitioning to IdleAwake for burst scanning");
      enterIdleAwake_();
      return;
    }
    // Check if we're in alert tracking window (adaptive scanning)
    const bool tracking = (state_.lastAlertMs != 0) && ((now - state_.lastAlertMs) < state_.settings.alertTrackingMs);
    if (tracking) {
      // Keep scanning during tracking window
      return;
    }
    if (now - state_.lastActivityMs > state_.settings.sleepTimeoutMs) {
      enterIdleAwake_();
    }
  } else if (state_.mode == SystemMode::IdleAwake) {
    // You can use the same timeout or a second timeout for deep sleep
    if (now - state_.lastActivityMs > state_.settings.sleepTimeoutMs) {
			if (isUsbSerialActive_()) {
				dbgPrint(state_, "[power] USB serial active; inhibiting deep sleep");
				state_.lastActivityMs = now;
				return;
			}

      // Starting a new "sleeping" period; clear any old asleep-scan summary.
      state_.sleepScan = SleepScanStats{};
      store_->saveSleepScanStats(state_.sleepScan);

			enterPreSleep_();
    }
  } else if (state_.mode == SystemMode::ScanBurst) {
    if (scan_->burstComplete()) {
      // If we're tracking a device, go to IdleAwake instead of sleeping or going interactive
      // to allow tracking bursts to continue
      if (state_.trackingOui24 != 0) {
        dbgPrint(state_, "[track] burst complete, staying awake for next burst");
        enterIdleAwake_();
        return;
      }

      // TODO: if alerts fired during burst and screen enabled -> interactive else sleep
      if (state_.settings.alertScreen && state_.alertsFired > 0) {
      // Persist the latest asleep-scan summary before going interactive.
      store_->saveSleepScanStats(state_.sleepScan);

        // This boot was a timer wake, but the *screen wake* was effectively due to an alert.
        if (state_.wakeReason == WakeReason::Timer) {
          state_.wakeReason = WakeReason::Alert;
        }
        enterInteractive_();

        // Stay in Alerting mode until the user navigates away from the Alerts section.
        state_.mode = SystemMode::Alerting;
      } else {
				if (isUsbSerialActive_()) {
					dbgPrint(state_, "[power] USB serial active; skipping deep sleep after burst");
					state_.lastActivityMs = now;
					enterIdleAwake_();
					return;
				}

        // Persist asleep-scan summary at the end of each timer-wake scan burst.
        if (state_.wakeReason == WakeReason::Timer) {
          store_->saveSleepScanStats(state_.sleepScan);
        }

				enterPreSleep_();
      }
    }
  }
}
