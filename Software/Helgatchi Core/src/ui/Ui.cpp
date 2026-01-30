#include "Ui.h"

#include "../core/Config.h"
#include "../drivers/Display.h"
#include "../drivers/Backlight.h"

#include <algorithm>
#include <vector>

#include <LittleFS.h>

static void formatOui_(uint32_t oui24, char* out, size_t outLen) {
  if (!out || outLen < 9) return;
  const uint8_t b0 = (uint8_t)((oui24 >> 16) & 0xFFu);
  const uint8_t b1 = (uint8_t)((oui24 >> 8) & 0xFFu);
  const uint8_t b2 = (uint8_t)(oui24 & 0xFFu);
  snprintf(out, outLen, "%02X:%02X:%02X", (unsigned)b0, (unsigned)b1, (unsigned)b2);
}

static bool readLine_(File& f, char* out, size_t outLen) {
  if (!out || outLen < 2) return false;
  out[0] = 0;
  if (!f) return false;
  if (!f.available()) return false;
  const size_t n = f.readBytesUntil('\n', out, outLen - 1);
  out[n] = 0;
  return true;
}

static bool parseJsonStringValue_(const char* line, const char* key, char* out, size_t outLen) {
  if (!line || !key || !out || outLen < 2) return false;
  out[0] = 0;
  const char* k = strstr(line, key);
  if (!k) return false;
  const char* colon = strchr(k, ':');
  if (!colon) return false;
  const char* q1 = strchr(colon, '"');
  if (!q1) return false;
  const char* q2 = strchr(q1 + 1, '"');
  if (!q2) return false;
  const size_t n = (size_t)(q2 - (q1 + 1));
  if (n == 0) return false;
  const size_t copyN = std::min(n, outLen - 1);
  memcpy(out, q1 + 1, copyN);
  out[copyN] = 0;
  return true;
}

// Made non-static so Core.cpp can use it during device tracking
const char* btCompanyName(uint16_t companyId, bool allowSlowLookup) {
  struct CacheEntry {
    uint16_t id;
    char name[32];
  };
  static CacheEntry cache[64]{};
  static uint8_t cacheCount = 0;

  if (companyId == 0) return "--";
  for (uint8_t i = 0; i < cacheCount; i++) {
    if (cache[i].id == companyId) return cache[i].name;
  }

  // If not cached and slow lookup not allowed, return placeholder
  if (!allowSlowLookup) {
    return "--";
  }

  CacheEntry e{};
  e.id = companyId;
  strncpy(e.name, "Unknown", sizeof(e.name) - 1);
  e.name[sizeof(e.name) - 1] = 0;

  File f = LittleFS.open("/bt_company_ids.json", "r");
  if (f) {
    char needle[24];
    snprintf(needle, sizeof(needle), "\"value\": \"0x%04X\"", (unsigned)companyId);
    char line[128];
    while (readLine_(f, line, sizeof(line))) {
      if (strstr(line, needle) == nullptr) continue;
      // The next few lines should include the "name" field in our pretty-printed JSON.
      for (int i = 0; i < 6 && readLine_(f, line, sizeof(line)); i++) {
        if (strstr(line, "\"name\"") == nullptr) continue;
        char tmp[64];
        if (parseJsonStringValue_(line, "\"name\"", tmp, sizeof(tmp))) {
          strncpy(e.name, tmp, sizeof(e.name) - 1);
          e.name[sizeof(e.name) - 1] = 0;
        }
        break;
      }
      break;
    }
    f.close();
  }

  if (cacheCount < (uint8_t)(sizeof(cache) / sizeof(cache[0]))) {
    cache[cacheCount++] = e;
    return cache[cacheCount - 1].name;
  }
  // Simple eviction: overwrite slot 0.
  cache[0] = e;
  return cache[0].name;
}

// Fixed safe zone padding (pixels). Keeps UI away from rounded corners.
// Requested: bump safe zone to 20px.
static constexpr int kSafePadX = 20;
static constexpr int kSafePadY = 20;

static int textW_(const std::string& s, int size) {
  // Default Adafruit_GFX built-in font is 6x8 pixels per character.
  return (int)s.size() * 6 * size;
}

static void renderHeader_(Display& d, const std::string& title, std::string& lastKey, uint16_t textRgb565 = 0xFFFF, bool skipClear = false) {
  const int size = 2;
  const int lineH = 8 * size;
  const std::string key = title + "\n" + std::to_string(size) + "\n" + std::to_string((unsigned)textRgb565);
  if (key == lastKey) return;

  if (!skipClear) {
    d.fillRect(0, 0, d.width(), lineH, 0x0000);
  }
  const int tw = textW_(title, size);
  const int x = std::max(0, (d.width() - tw) / 2);
  d.drawTextColored(x, 0, title, size, textRgb565, 0x0000);
  lastKey = key;
}

void Ui::startHeaderFlash_(View v) {
  headerFlashActive_ = true;
  headerFlashView_ = v;
  headerFlashRed_ = true;
  // 6 toggles => red/white alternating, 3 red flashes.
  headerFlashTogglesLeft_ = 6;
  headerFlashNextMs_ = millis() + kHeaderFlashIntervalMs;
  dirty_ = true;
}

void Ui::tickHeaderFlash_(uint32_t nowMs) {
  if (!headerFlashActive_) return;
  if (view_ != headerFlashView_) {
    headerFlashActive_ = false;
    headerFlashRed_ = false;
    headerFlashTogglesLeft_ = 0;
    return;
  }
  if (headerFlashTogglesLeft_ == 0) {
    headerFlashActive_ = false;
    headerFlashRed_ = false;
    return;
  }
  if (nowMs < headerFlashNextMs_) return;
  headerFlashRed_ = !headerFlashRed_;
  headerFlashTogglesLeft_--;
  headerFlashNextMs_ = nowMs + kHeaderFlashIntervalMs;
  dirty_ = true;
}

static void renderLine(Display& d, int x, int y, int size, const std::string& text, std::string& last, bool skipClear = false) {
  if (text == last) return;
  // Default GFX font is 6x8 per char; height is 8*size.
  const int lineH = 8 * size;

  const int drawX = kSafePadX + x;
  const int drawY = kSafePadY + y;
  int safeW = d.width() - (kSafePadX * 2);
  if (safeW < 0) safeW = d.width();

  if (!skipClear) {
    d.fillRect(kSafePadX, drawY, safeW, lineH, 0x0000);
  }
  d.drawText(drawX, drawY, text, size);
  last = text;
}

static void renderLineAbs_(Display& d, int x, int y, int size, const std::string& text, std::string& last, bool skipClear = false) {
  if (text == last) return;
  const int lineH = 8 * size;
  const int drawX = x;
  const int drawY = y;

  if (!skipClear) {
    d.fillRect(0, drawY, d.width(), lineH, 0x0000);
  }
  d.drawText(drawX, drawY, text, size);
  last = text;
}

static void renderFooterRow3_(Display& d,
                             int y,
                             int size,
                             const std::string& left,
                             const std::string& center,
                             const std::string& right,
                             std::string& lastKey) {
  const std::string key = left + "\n" + center + "\n" + right + "\n" + std::to_string(size);
  if (key == lastKey) return;

  const int lineH = 8 * size;
  const int drawY = y;
  const int w = d.width();

  // Footer is drawn outside the main safe zone: clear and render full-width.
  d.fillRect(0, drawY, w, lineH, 0x0000);

  // Rounded corners mainly affect text near edges; keep X padding for left/right.
  if (!left.empty()) {
    d.drawText(kSafePadX, drawY, left, size);
  }
  if (!center.empty()) {
    const int tw = textW_(center, size);
    const int x = std::max(0, (w - tw) / 2);
    d.drawText(x, drawY, center, size);
  }
  if (!right.empty()) {
    const int tw = textW_(right, size);
    const int x = std::max(0, w - kSafePadX - tw);
    d.drawText(x, drawY, right, size);
  }

  lastKey = key;
}

static void renderFooterCenter_(Display& d, int y, int size, const std::string& center, std::string& lastKey) {
  const std::string key = center + "\n" + std::to_string(size);
  if (key == lastKey) return;

  const int lineH = 8 * size;
  const int drawY = y;
  const int w = d.width();

  d.fillRect(0, drawY, w, lineH, 0x0000);

  if (!center.empty()) {
    const int tw = textW_(center, size);
    const int x = std::max(0, (w - tw) / 2);
    d.drawText(x, drawY, center, size);
  }

  lastKey = key;
}

static int clampInt_(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static const char* modeToStr(SystemMode m) {
  switch (m) {
    case SystemMode::Boot:
      return "Boot";
    case SystemMode::Interactive:
      return "Interactive";
    case SystemMode::IdleAwake:
      return "Idle";
    case SystemMode::ScanBurst:
      return "ScanBurst";
    case SystemMode::Alerting:
      return "Alerting";
    case SystemMode::PreSleep:
      return "PreSleep";
    default:
      return "?";
  }
}

static const char* wakeToStr(WakeReason r) {
  switch (r) {
    case WakeReason::ColdBoot:
      return "Cold";
    case WakeReason::Timer:
      return "Timer";
    case WakeReason::Button:
      return "Button";
    case WakeReason::Alert:
      return "Alert";
    default:
      return "?";
  }
}

static bool applySettingsDelta_(CoreState& state, uint8_t idx, int dir) {
  // Returns true if any value changed.
  // idx mapping:
  // 0 sleepTimeout
  // 1 wakeInterval
  // 2 scanBurst
  // 3 alertScreen
  // 4 alertVibe
  // 5 debugSerial
  switch (idx) {
    case 0: {
      // 5s steps, clamp 5s..10m
      const int step = 5000;
      const int lo = 5000;
      const int hi = 600000;
      const int cur = (int)state.settings.sleepTimeoutMs;
      const int next = clampInt_(cur + (dir * step), lo, hi);
      if ((uint32_t)next == state.settings.sleepTimeoutMs) return false;
      state.settings.sleepTimeoutMs = (uint32_t)next;
      return true;
    }
    case 1: {
      // 1s steps, clamp 3s..10m
      const int step = 1000;
      const int lo = 3000;
      const int hi = 600000;
      const int cur = (int)state.settings.wakeIntervalMs;
      const int next = clampInt_(cur + (dir * step), lo, hi);
      if ((uint32_t)next == state.settings.wakeIntervalMs) return false;
      state.settings.wakeIntervalMs = (uint32_t)next;
      return true;
    }
    case 2: {
      // 500ms steps, clamp 500ms..20s
      const int step = 500;
      const int lo = 500;
      const int hi = 20000;
      const int cur = (int)state.settings.scanBurstMs;
      const int next = clampInt_(cur + (dir * step), lo, hi);
      if ((uint32_t)next == state.settings.scanBurstMs) return false;
      state.settings.scanBurstMs = (uint32_t)next;
      return true;
    }
    case 3: {
      if (dir == 0) return false;
      state.settings.alertScreen = !state.settings.alertScreen;
      return true;
    }
    case 4: {
      if (dir == 0) return false;
      state.settings.alertVibe = !state.settings.alertVibe;
      return true;
    }
    default:
      return false;
  }
}

static bool applySettingsDeltaTo_(uint32_t& v, int dir, int step, int lo, int hi) {
  const int cur = (int)v;
  const int next = clampInt_(cur + (dir * step), lo, hi);
  if (next == cur) return false;
  v = (uint32_t)next;
  return true;
}

void Ui::begin() {
  dirty_ = true;
  needsClear_ = true;
	view_ = View::Status;
	menuIndex_ = 0;
	settingsIndex_ = 0;
  packIndex_ = 0;
  devicesIndex_ = 0;
  selectedDeviceIndex_ = 0;
  deviceActionIndex_ = 0;
  alertsConfigIndex_ = 0;
  alertsRuleIndex_ = 0;
  alertsRuleKind_ = CoreState::RuleKind::Oui;
  wipeIndex_ = 0;
  editKind_ = EditKind::None;
  editIndex_ = 0;
  confirmYes_ = false;
  confirmAction_ = ConfirmAction::None;
	for (auto& s : lastLines_) s.clear();
}

void Ui::setRootScreenMain() {
  dirty_ = true;
  needsClear_ = true;
	view_ = View::Status;
  editKind_ = EditKind::None;
  confirmAction_ = ConfirmAction::None;
}

bool Ui::showAlertOverlay(const AlertFiredEvent&, CoreState& state) {
  // Check if this is a new alert (different rule ID)
  const bool newAlert = (state.lastAlertRuleId != lastAlertRuleId_);

  // Only switch to Alerts view if:
  // 1. It's a new alert that hasn't been shown yet, OR
  // 2. We're on Status screen or waking from sleep
  if (!alertShown_ || view_ == View::Status || state.wakeReason == WakeReason::Timer) {
    stopAutoScroll_();
    editKind_ = EditKind::None;
    confirmAction_ = ConfirmAction::None;
    view_ = View::Alerts;
    alertsIndex_ = 0;
    needsClear_ = true;
    for (auto& s : lastLines_) s.clear();
    startHeaderFlash_(View::Alerts);
    alertShown_ = true;
  } else {
    // Alert already shown - just flash the current view's header
    // The header flash will show the alert name in red
    startHeaderFlash_(view_);
  }
  dirty_ = true;
  return (view_ == View::Alerts || view_ == View::AlertsConfig || view_ == View::AlertsRuleList || view_ == View::AlertPacks);
}

void Ui::stopAutoScroll_() {
  autoScrollActive_ = false;
  autoScrollDir_ = 0;
  autoScrollView_ = View::Status;
  autoScrollNextMs_ = 0;
}

bool Ui::canAutoScrollInView_(const CoreState& state) const {
  if (editKind_ != EditKind::None) return false;
  // Views where L/R scroll a selection.
  switch (view_) {
    case View::MainMenu:
    case View::Devices:
    case View::DeviceAction:
    case View::AlertsConfig:
    case View::AlertsRuleList:
    case View::AlertPacks:
    case View::Settings:
    case View::WipeOptions:
      return true;
    default:
      break;
  }
  (void)state;
  return false;
}

void Ui::stepScroll_(int8_t dir, const CoreState& state) {
  if (dir == 0) return;
  const bool forward = (dir > 0);

  if (view_ == View::MainMenu) {
    const uint8_t itemCount = 4;
    menuIndex_ = (uint8_t)((menuIndex_ + (forward ? 1 : (itemCount - 1))) % itemCount);
    return;
  }

  if (view_ == View::Devices) {
    const uint8_t count = state.deviceCount;
    if (count == 0) return;
    devicesIndex_ = (uint8_t)((devicesIndex_ + (forward ? 1 : (count - 1))) % count);
    return;
  }

  if (view_ == View::DeviceAction) {
    const uint8_t itemCount = 4; // OUI, MAC, Company, Name
    deviceActionIndex_ = (uint8_t)((deviceActionIndex_ + (forward ? 1 : (itemCount - 1))) % itemCount);
    return;
  }

  if (view_ == View::AlertsConfig) {
    const uint8_t itemCount = 6;
    alertsConfigIndex_ = (uint8_t)((alertsConfigIndex_ + (forward ? 1 : (itemCount - 1))) % itemCount);
    return;
  }

  if (view_ == View::AlertsRuleList) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < state.ruleSummaryCount; i++) {
      if (state.ruleSummaries[i].kind == alertsRuleKind_) count++;
    }
    if (count == 0) return;
    alertsRuleIndex_ = (uint8_t)((alertsRuleIndex_ + (forward ? 1 : (count - 1))) % count);
    return;
  }

  if (view_ == View::AlertPacks) {
    const uint8_t count = state.rulePackCount;
    if (count == 0) return;
    packIndex_ = (uint8_t)((packIndex_ + (forward ? 1 : (count - 1))) % count);
    return;
  }

  if (view_ == View::Settings) {
    const uint8_t itemCount = 8;
    settingsIndex_ = (uint8_t)((settingsIndex_ + (forward ? 1 : (itemCount - 1))) % itemCount);
    return;
  }

  if (view_ == View::WipeOptions) {
    const uint8_t itemCount = 3;
    wipeIndex_ = (uint8_t)((wipeIndex_ + (forward ? 1 : (itemCount - 1))) % itemCount);
    return;
  }

  if (view_ == View::DebugOptions) {
    const uint8_t itemCount = 2;
    debugIndex_ = (uint8_t)((debugIndex_ + (forward ? 1 : (itemCount - 1))) % itemCount);
    return;
  }
}

void Ui::tickAutoScroll_(const CoreState& state, uint32_t nowMs) {
  if (!autoScrollActive_) return;
  if (view_ != autoScrollView_) {
    stopAutoScroll_();
    return;
  }
  if (!canAutoScrollInView_(state)) {
    stopAutoScroll_();
    return;
  }
  if (nowMs < autoScrollNextMs_) return;
  stepScroll_(autoScrollDir_, state);
  autoScrollNextMs_ = nowMs + kAutoScrollRepeatMs;
  dirty_ = true;
}

void Ui::onButton(const ButtonEvent& b, CoreState& state, EventBus& bus) {
  const View prevView = view_;
  const EditKind prevEdit = editKind_;
  const auto inAlertsMenu = [](View v) {
    return (v == View::Alerts || v == View::AlertsConfig || v == View::AlertsRuleList || v == View::AlertPacks);
  };

  // Any short press cancels auto-scroll (treat as "release" / user intervention).
  if (autoScrollActive_ && b.action == ButtonAction::Press) {
    stopAutoScroll_();
  }

  // Center long-press is reserved for navigation/back.
  if (b.id == ButtonId::Center && b.action == ButtonAction::LongPress) {
		stopAutoScroll_();
    // Always cancel edit first.
    if (editKind_ != EditKind::None) {
      editKind_ = EditKind::None;
      confirmAction_ = ConfirmAction::None;
      dirty_ = true;
      return;
    }

    if (view_ == View::AlertPacks) {
      view_ = View::AlertsConfig;
    } else if (view_ == View::AlertsRuleList) {
      view_ = View::AlertsConfig;
    } else if (view_ == View::AlertsConfig) {
      view_ = View::Alerts;
    } else if (view_ == View::WipeOptions) {
      view_ = View::Settings;
    } else if (view_ == View::DebugOptions) {
      view_ = View::Settings;
    } else if (view_ == View::DeviceAction) {
      view_ = View::Devices;
    } else if (view_ == View::Devices) {
      view_ = View::MainMenu;
    } else if (view_ == View::Settings || view_ == View::Alerts) {
		  view_ = View::MainMenu;
	  } else if (view_ == View::MainMenu) {
		  view_ = View::Status;
	  } else if (view_ == View::Status) {
		  // Status: holding C triggers immediate sleep
		  Event e{};
		  e.type = EventType::ForceSleep;
		  bus.push(e);
	  }
    if (view_ != prevView) {
      needsClear_ = true;
      for (auto& s : lastLines_) s.clear();
    }
    // If we were in Alerting mode, revert once the user leaves the Alerts section.
    if (state.mode == SystemMode::Alerting && inAlertsMenu(prevView) && !inAlertsMenu(view_)) {
      Event e{};
      e.type = EventType::AlertUiDismissed;
      bus.push(e);
    }
    dirty_ = true;
	  return;
  }

  // Quick rule capture (debug-only):
  // - Long-press Left: add rule for last BLE OUI
  // - Long-press Right: add rule for last WiFi OUI
  if (state.settings.debugLevel > 0 && b.action == ButtonAction::LongPress) {
    if (b.id == ButtonId::Left && state.lastBleOui24 != 0) {
      Event e{};
      e.type = EventType::AddOuiRule;
      e.addOuiRule.oui24 = state.lastBleOui24;
      e.addOuiRule.source = OuiRuleSource::Ble;
      bus.push(e);
      Serial.print("[ui] add BLE rule oui=");
      Serial.println(state.lastBleOui24, HEX);
    } else if (b.id == ButtonId::Right && state.lastWifiOui24 != 0) {
      Event e{};
      e.type = EventType::AddOuiRule;
      e.addOuiRule.oui24 = state.lastWifiOui24;
      e.addOuiRule.source = OuiRuleSource::Wifi;
      bus.push(e);
      Serial.print("[ui] add WiFi rule oui=");
      Serial.println(state.lastWifiOui24, HEX);
    }
  }

  // Continuous scroll: long-press L/R in scrollable views.
  if (state.settings.debugLevel == 0 && b.action == ButtonAction::LongPress && (b.id == ButtonId::Left || b.id == ButtonId::Right)) {
    if (canAutoScrollInView_(state)) {
      const uint32_t nowMs = millis();
      const int8_t dir = (b.id == ButtonId::Right) ? (int8_t)+1 : (int8_t)-1;
      const bool same = autoScrollActive_ && (autoScrollView_ == view_) && (autoScrollDir_ == dir);
      if (same) {
        stopAutoScroll_();
      } else {
        autoScrollActive_ = true;
        autoScrollDir_ = dir;
        autoScrollView_ = view_;
        autoScrollNextMs_ = nowMs + kAutoScrollInitialDelayMs;
        // Immediate step so it feels responsive.
        stepScroll_(dir, state);
        dirty_ = true;
      }
      return;
    }
  }

  // Menu navigation (short press)
  if (b.action == ButtonAction::Press) {
    if (view_ == View::Status) {
      if (b.id == ButtonId::Center) {
        view_ = View::MainMenu;
      }
    } else if (view_ == View::MainMenu) {
      const uint8_t itemCount = 4; // Status, Settings, Alerts, Devices
      if (b.id == ButtonId::Left) {
        menuIndex_ = (menuIndex_ + itemCount - 1) % itemCount;
      } else if (b.id == ButtonId::Right) {
        menuIndex_ = (menuIndex_ + 1) % itemCount;
      } else if (b.id == ButtonId::Center) {
        if (menuIndex_ == 0) {
          view_ = View::Status;
        } else if (menuIndex_ == 1) {
          view_ = View::Settings;
			} else if (menuIndex_ == 2) {
				view_ = View::Alerts;
				alertsIndex_ = 0;
			} else if (menuIndex_ == 3) {
				view_ = View::Devices;
				devicesIndex_ = 0;
        }
      }
		} else if (view_ == View::Devices) {
			const uint8_t count = state.deviceCount;
			if (count > 0) {
				if (b.id == ButtonId::Left) {
					devicesIndex_ = (uint8_t)((devicesIndex_ + count - 1) % count);
				} else if (b.id == ButtonId::Right) {
					devicesIndex_ = (uint8_t)((devicesIndex_ + 1) % count);
				} else if (b.id == ButtonId::Center) {
					// Calculate actual device array index from sorted position
					std::vector<uint8_t> idx;
					idx.reserve(count);
					for (uint8_t i = 0; i < count; i++) idx.push_back(i);
					std::sort(idx.begin(), idx.end(), [&](uint8_t a, uint8_t b) {
						const auto& da = state.devices[a];
						const auto& db = state.devices[b];
						if (da.rssi != db.rssi) return da.rssi > db.rssi;
						return da.lastSeenMs > db.lastSeenMs;
					});
					selectedDeviceIndex_ = (devicesIndex_ < idx.size()) ? idx[devicesIndex_] : 0;
					// Open device action menu
					view_ = View::DeviceAction;
					deviceActionIndex_ = 0;
				}
			}
    } else if (view_ == View::DeviceAction) {
      const uint8_t itemCount = 4; // OUI, MAC, Company, Name
      if (b.id == ButtonId::Left) {
        deviceActionIndex_ = (uint8_t)((deviceActionIndex_ + itemCount - 1) % itemCount);
      } else if (b.id == ButtonId::Right) {
        deviceActionIndex_ = (uint8_t)((deviceActionIndex_ + 1) % itemCount);
      } else if (b.id == ButtonId::Center) {
        // Add rule based on selection using actual device index
        if (selectedDeviceIndex_ < state.deviceCount) {
          const auto& d = state.devices[selectedDeviceIndex_];
          Event e{};
          
          switch (deviceActionIndex_) {
            case 0: // Add OUI rule
              e.type = EventType::AddOuiRule;
              e.addOuiRule.oui24 = d.oui24;
              e.addOuiRule.source = OuiRuleSource::Ble;
              bus.push(e);
              break;
            case 1: // Add MAC rule (not available, use device addr)
              // MAC address not stored in DeviceSummary, skip
              break;
            case 2: // Add Company rule
              if (d.hasMsdCompanyId && d.msdCompanyId != 0) {
                e.type = EventType::AddCompanyRule;
                e.addCompanyRule.companyId = d.msdCompanyId;
                memcpy(e.addCompanyRule.name, d.name, sizeof(e.addCompanyRule.name));
                bus.push(e);
              }
              break;
            case 3: // Add Name rule
              if (d.name[0]) {
                e.type = EventType::AddNameRule;
                memcpy(e.addNameRule.name, d.name, sizeof(e.addNameRule.name));
                bus.push(e);
              }
              break;
          }
        }
        view_ = View::Devices; // Go back to devices
      }
    } else if (view_ == View::Alerts) {
      if (state.lastAlertTsMs != 0) {
        // Alert has fired - show menu with options
        const uint8_t itemCount = 2; // Disable Alert, Config
        if (b.id == ButtonId::Left) {
          alertsIndex_ = (uint8_t)((alertsIndex_ + itemCount - 1) % itemCount);
        } else if (b.id == ButtonId::Right) {
          alertsIndex_ = (uint8_t)((alertsIndex_ + 1) % itemCount);
        } else if (b.id == ButtonId::Center) {
          if (alertsIndex_ == 0) {
            // Toggle the alert rule enabled state
            Event e{};
            e.type = EventType::ToggleRule;
            e.toggleRule.id = state.lastAlertRuleId;
            e.toggleRule.enabled = !state.lastAlertRuleEnabled; // Toggle
            bus.push(e);
            alertsIndex_ = 0; // Reset menu
          } else if (alertsIndex_ == 1) {
            // Go to alerts config
            view_ = View::AlertsConfig;
            alertsConfigIndex_ = 0;
          }
        }
      } else {
        // No alert yet - just go to config on Center
        if (b.id == ButtonId::Center) {
          view_ = View::AlertsConfig;
          alertsConfigIndex_ = 0;
        }
      }
    } else if (view_ == View::AlertsConfig) {
      const uint8_t itemCount = 6; // Packs + 5 rule categories
      if (b.id == ButtonId::Left) {
        alertsConfigIndex_ = (uint8_t)((alertsConfigIndex_ + itemCount - 1) % itemCount);
      } else if (b.id == ButtonId::Right) {
        alertsConfigIndex_ = (uint8_t)((alertsConfigIndex_ + 1) % itemCount);
      } else if (b.id == ButtonId::Center) {
        if (alertsConfigIndex_ == 0) {
          view_ = View::AlertPacks;
          packIndex_ = 0;
        } else {
          view_ = View::AlertsRuleList;
          alertsRuleIndex_ = 0;
          switch (alertsConfigIndex_) {
            case 1:
              alertsRuleKind_ = CoreState::RuleKind::ServiceUuid;
              break;
            case 2:
              alertsRuleKind_ = CoreState::RuleKind::CompanyId;
              break;
            case 3:
              alertsRuleKind_ = CoreState::RuleKind::NameContains;
              break;
            case 4:
              alertsRuleKind_ = CoreState::RuleKind::Oui;
              break;
            case 5:
              alertsRuleKind_ = CoreState::RuleKind::Mac;
              break;
            default:
              alertsRuleKind_ = CoreState::RuleKind::Oui;
              break;
          }
        }
      }
    } else if (view_ == View::AlertsRuleList) {
      uint8_t count = 0;
      for (uint8_t i = 0; i < state.ruleSummaryCount; i++) {
        if (state.ruleSummaries[i].kind == alertsRuleKind_) count++;
      }
      if (count > 0) {
        if (b.id == ButtonId::Left) {
          alertsRuleIndex_ = (uint8_t)((alertsRuleIndex_ + count - 1) % count);
        } else if (b.id == ButtonId::Right) {
          alertsRuleIndex_ = (uint8_t)((alertsRuleIndex_ + 1) % count);
        } else if (b.id == ButtonId::Center) {
          if (alertsRuleIndex_ >= count) alertsRuleIndex_ = 0;
          uint8_t picked = 0xFF;
          uint8_t seen = 0;
          for (uint8_t i = 0; i < state.ruleSummaryCount; i++) {
            if (state.ruleSummaries[i].kind != alertsRuleKind_) continue;
            if (seen == alertsRuleIndex_) {
              picked = i;
              break;
            }
            seen++;
          }
          if (picked != 0xFF) {
            const auto& r = state.ruleSummaries[picked];
            // If this is a pack rule and the pack is disabled, don't allow individual enables.
            bool packEnabled = true;
            if (r.pack[0] && strncmp(r.pack, "USER", 4) != 0) {
              for (uint8_t p = 0; p < state.rulePackCount; p++) {
                if (strncmp(state.rulePacks[p].name, r.pack, sizeof(state.rulePacks[p].name)) == 0) {
                  packEnabled = state.rulePacks[p].enabled;
                  break;
                }
              }
            }
            if (packEnabled) {
              Event e{};
              e.type = EventType::ToggleRule;
              e.toggleRule.id = r.id;
              e.toggleRule.enabled = !r.enabled;
              bus.push(e);
            }
          }
        }
      }
    } else if (view_ == View::Settings) {
      const uint8_t itemCount = 8; // + Wipe options...
      if (editKind_ == EditKind::Setting) {
        // In edit mode: L/R adjusts, C confirms.
        if (b.id == ButtonId::Center) {
          bool changed = false;
          switch (editIndex_) {
            case 0:
              changed = (state.settings.sleepTimeoutMs != editSleepTimeoutMs_);
              state.settings.sleepTimeoutMs = editSleepTimeoutMs_;
              break;
            case 1:
              changed = (state.settings.wakeIntervalMs != editWakeIntervalMs_);
              state.settings.wakeIntervalMs = editWakeIntervalMs_;
              break;
            case 2:
              changed = (state.settings.scanBurstMs != editScanBurstMs_);
              state.settings.scanBurstMs = editScanBurstMs_;
              break;
            case 3:
              changed = (state.settings.alertScreen != editAlertScreen_);
              state.settings.alertScreen = editAlertScreen_;
              break;
            case 4:
              changed = (state.settings.alertVibe != editAlertVibe_);
              state.settings.alertVibe = editAlertVibe_;
              break;
            case 6:
              changed = (state.settings.backlightLevel != editBacklightLevel_);
              state.settings.backlightLevel = editBacklightLevel_;
              break;
            default:
              break;
          }
          if (changed) {
            Event e{};
            e.type = EventType::SettingsChanged;
            bus.push(e);
          }
          editKind_ = EditKind::None;
        } else if (b.id == ButtonId::Left || b.id == ButtonId::Right) {
          const int dir = (b.id == ButtonId::Right) ? +1 : -1;
          switch (editIndex_) {
            case 0:
              applySettingsDeltaTo_(editSleepTimeoutMs_, dir, 5000, 5000, 600000);
              break;
            case 1:
              applySettingsDeltaTo_(editWakeIntervalMs_, dir, 1000, 3000, 600000);
              break;
            case 2:
              applySettingsDeltaTo_(editScanBurstMs_, dir, 500, 500, 20000);
              break;
            case 3:
              editAlertScreen_ = !editAlertScreen_;
              break;
            case 4:
              editAlertVibe_ = !editAlertVibe_;
              break;
            case 6:
              editBacklightLevel_ = (editBacklightLevel_ + (dir > 0 ? 1 : 3)) % 4;
              backlight_.setBrightness(editBacklightLevel_); // Apply immediately
              break;
            default:
              break;
          }
        }
      } else {
        // Browse mode: L/R scroll selection, C enters edit/submenu.
        if (b.id == ButtonId::Left) {
          settingsIndex_ = (settingsIndex_ + itemCount - 1) % itemCount;
        } else if (b.id == ButtonId::Right) {
          settingsIndex_ = (settingsIndex_ + 1) % itemCount;
        } else if (b.id == ButtonId::Center) {
          if (settingsIndex_ == 5) {
            view_ = View::DebugOptions;
            debugIndex_ = 0;
          } else if (settingsIndex_ == 7) {
            view_ = View::WipeOptions;
            wipeIndex_ = 0;
          } else if (settingsIndex_ == 3 || settingsIndex_ == 4) {
            // Booleans: toggle immediately (no edit mode).
            bool changed = false;
            switch (settingsIndex_) {
              case 3:
                state.settings.alertScreen = !state.settings.alertScreen;
                changed = true;
                break;
              case 4:
                state.settings.alertVibe = !state.settings.alertVibe;
                changed = true;
                break;
              default:
                break;
            }
            if (changed) {
              Event e{};
              e.type = EventType::SettingsChanged;
              bus.push(e);
            }
          } else {
            // Enter edit mode for selected setting.
            editKind_ = EditKind::Setting;
            editIndex_ = settingsIndex_;
            editSleepTimeoutMs_ = state.settings.sleepTimeoutMs;
            editWakeIntervalMs_ = state.settings.wakeIntervalMs;
            editScanBurstMs_ = state.settings.scanBurstMs;
            editAlertScreen_ = state.settings.alertScreen;
            editAlertVibe_ = state.settings.alertVibe;
            editBacklightLevel_ = state.settings.backlightLevel;
          }
        }
      }
    } else if (view_ == View::WipeOptions) {
      const uint8_t itemCount = 3;
      if (editKind_ == EditKind::Confirm) {
        if (b.id == ButtonId::Left || b.id == ButtonId::Right) {
          confirmYes_ = !confirmYes_;
        } else if (b.id == ButtonId::Center) {
          if (confirmYes_) {
            Event e{};
            e.type = EventType::WipeAction;
            switch (confirmAction_) {
              case ConfirmAction::WipeLocalStats:
                e.wipe.kind = WipeActionKind::LocalStats;
                break;
              case ConfirmAction::WipeCustomRules:
                e.wipe.kind = WipeActionKind::CustomRules;
                break;
              case ConfirmAction::ResetSettings:
                e.wipe.kind = WipeActionKind::ResetSettings;
                break;
              default:
                e.wipe.kind = WipeActionKind::LocalStats;
                break;
            }
            bus.push(e);
          }
          editKind_ = EditKind::None;
          confirmAction_ = ConfirmAction::None;
          confirmYes_ = false;
        }
      } else {
        // Browse mode
        if (b.id == ButtonId::Left) {
          wipeIndex_ = (wipeIndex_ + itemCount - 1) % itemCount;
        } else if (b.id == ButtonId::Right) {
          wipeIndex_ = (wipeIndex_ + 1) % itemCount;
        } else if (b.id == ButtonId::Center) {
          editKind_ = EditKind::Confirm;
          confirmYes_ = false; // default to NO
          switch (wipeIndex_) {
            case 0:
              confirmAction_ = ConfirmAction::WipeLocalStats;
              break;
            case 1:
              confirmAction_ = ConfirmAction::WipeCustomRules;
              break;
            case 2:
              confirmAction_ = ConfirmAction::ResetSettings;
              break;
            default:
              confirmAction_ = ConfirmAction::WipeLocalStats;
              break;
          }
        }
      }
    } else if (view_ == View::DebugOptions) {
      const uint8_t itemCount = 2;
      if (editKind_ == EditKind::Setting) {
        // In edit mode: L/R adjusts, C confirms.
        if (b.id == ButtonId::Center) {
          bool changed = false;
          if (editIndex_ == 0) {
            changed = (state.settings.debugLevel != editDebugLevel_);
            state.settings.debugLevel = editDebugLevel_;
          }
          if (changed) {
            Event e{};
            e.type = EventType::SettingsChanged;
            bus.push(e);
          }
          editKind_ = EditKind::None;
        } else if (b.id == ButtonId::Left || b.id == ButtonId::Right) {
          const int dir = (b.id == ButtonId::Right) ? +1 : -1;
          if (editIndex_ == 0) {
            editDebugLevel_ = (editDebugLevel_ + (dir > 0 ? 1 : 3)) % 4;
          }
        }
      } else {
        // Browse mode: L/R scroll selection, C enters edit.
        if (b.id == ButtonId::Left) {
          debugIndex_ = (debugIndex_ + itemCount - 1) % itemCount;
        } else if (b.id == ButtonId::Right) {
          debugIndex_ = (debugIndex_ + 1) % itemCount;
        } else if (b.id == ButtonId::Center) {
          if (debugIndex_ == 0) {
            // Edit debug level
            editKind_ = EditKind::Setting;
            editIndex_ = 0;
            editDebugLevel_ = state.settings.debugLevel;
          }
          // Index 1 (Serial output) is read-only, derived from debugLevel
        }
      }
    } else if (view_ == View::AlertPacks) {
      const uint8_t count = state.rulePackCount;
    if (count == 0) {
      // no-op
    } else {
      // Browse mode
      if (b.id == ButtonId::Left) {
        packIndex_ = (uint8_t)((packIndex_ + count - 1) % count);
      } else if (b.id == ButtonId::Right) {
        packIndex_ = (uint8_t)((packIndex_ + 1) % count);
      } else if (b.id == ButtonId::Center) {
        // Toggle this pack immediately.
        const bool newEnabled = !state.rulePacks[packIndex_].enabled;
        Event e{};
        e.type = EventType::ToggleRulePack;
        e.togglePack.index = packIndex_;
        e.togglePack.enabled = newEnabled;
        bus.push(e);
      }
    }
    }
  }

  if (view_ != prevView) {
		needsClear_ = true;
		for (auto& s : lastLines_) s.clear();
	}
  if (editKind_ != prevEdit) {
    // Force redraw hints/values immediately.
    dirty_ = true;
  }

  // If we were in Alerting mode, revert once the user leaves the Alerts section.
  if (state.mode == SystemMode::Alerting && inAlertsMenu(prevView) && !inAlertsMenu(view_)) {
	  Event e{};
	  e.type = EventType::AlertUiDismissed;
	  bus.push(e);
  }

  dirty_ = true;
}

void Ui::tick(CoreState& state, EventBus&) {
  const uint32_t now = millis();
	tickAutoScroll_(state, now);
	tickHeaderFlash_(now);

  // Track when alert changes to force full redraw and reset shown flag
  if (state.lastAlertRuleId != lastAlertRuleId_) {
    lastAlertRuleId_ = state.lastAlertRuleId;
    alertShown_ = false; // New alert, needs to be shown
    if (view_ == View::Alerts) {
      needsClear_ = true;
      for (auto& s : lastLines_) s.clear();
      dirty_ = true;
    }
  }

  // Refresh periodically for time-based lines like sleep countdown.
  // Devices view is noisy (RSSI/age changes), so refresh it less to reduce flicker.
  const uint32_t refreshMs = (view_ == View::Devices) ? 1000 : 250;
  if (!dirty_ && (now - lastRenderMs_) < refreshMs) return;

  lastRenderMs_ = now;
  dirty_ = false;

  if (needsClear_) {
    display_.clear();
    needsClear_ = false;
    justCleared_ = true;
    for (auto& s : lastLines_) s.clear();
  } else {
    justCleared_ = false;
  }

  // Render helpers
  auto line = [&](size_t idx, int y, int size, const std::string& text) {
    if (idx >= lastLines_.size()) return;
    renderLine(display_, 0, y, size, text, lastLines_[idx], justCleared_);
  };

  auto lineAbs = [&](size_t idx, int x, int y, int size, const std::string& text) {
    if (idx >= lastLines_.size()) return;
    renderLineAbs_(display_, x, y, size, text, lastLines_[idx], justCleared_);
  };

  // Helper to render header with alert flash support
  auto header = [&](const std::string& title) {
    const bool flashing = (headerFlashActive_ && headerFlashView_ == view_);
    
    // If flashing in non-Alerts view, show alert name blinking red/white
    if (flashing && view_ != View::Alerts && state.lastAlertLabel[0]) {
      // Show alert name only (without "Alert: " prefix)
      // Blink red to white
      const uint16_t hdrColor = headerFlashRed_ ? 0xF800 : 0xFFFF;
      renderHeader_(display_, state.lastAlertLabel, lastLines_[0], hdrColor, justCleared_);
    } else {
      // Normal header rendering (Alerts view uses its own logic)
      const uint16_t hdrColor = (flashing && headerFlashRed_) ? 0xF800 : 0xFFFF;
      renderHeader_(display_, title, lastLines_[0], hdrColor, justCleared_);
    }
  };

  // Footer hints (fixed layout)
  std::string hintL;
  std::string hintC;
  std::string hintCHold;
  std::string hintR;

  // STATUS VIEW
  if (view_ == View::Status) {
    char buf[64];
    header("Status");
    const bool scanning = (state.mode == SystemMode::Interactive) || (state.mode == SystemMode::Alerting) || (state.mode == SystemMode::ScanBurst);
    snprintf(buf, sizeof(buf), "Mode:%s Scan:%s", modeToStr(state.mode), scanning ? "ON" : "OFF");
    line(1, 22, 1, buf);

    snprintf(buf, sizeof(buf), "WiFi:%lu BLE:%lu", (unsigned long)state.wifiSightings,
             (unsigned long)state.bleSightings);
    line(2, 34, 1, buf);

    if (state.bleSightings > 0) {
      char oui[10];
      formatOui_(state.lastBleOui24, oui, sizeof(oui));
      snprintf(buf, sizeof(buf), "BLE %ddBm %s", (int)state.lastBleRssi, oui);
    } else {
      snprintf(buf, sizeof(buf), "BLE --dBm --:--:--");
    }
    line(3, 46, 1, buf);

    const char* n = state.lastBleName[0] ? state.lastBleName : "--";
    snprintf(buf, sizeof(buf), "Name: %.18s", n);
    line(4, 58, 1, buf);

    const uint16_t cid = state.lastBleHasMsdCompanyId ? state.lastBleMsdCompanyId : 0;
    const char* mfg = (cid != 0) ? btCompanyName(cid, true) : "--";
    snprintf(buf, sizeof(buf), "Mfg: %.18s", mfg);
    line(5, 70, 1, buf);

    snprintf(buf, sizeof(buf), "Rules:%u A:%lu SB:%lu SW:%lu",
         (unsigned)state.ruleCount,
         (unsigned long)state.alertsFired,
         (unsigned long)state.sleepScan.bleHits,
         (unsigned long)state.sleepScan.wifiHits);
    line(6, 82, 1, buf);

    if (state.serialAttached) {
      line(7, 94, 1, "Sleep: DISABLED (USB)");
    } else {
      const uint32_t elapsed = (now >= state.lastActivityMs) ? (now - state.lastActivityMs) : 0;
      const uint32_t remMs = (elapsed >= state.settings.sleepTimeoutMs) ? 0 : (state.settings.sleepTimeoutMs - elapsed);
      snprintf(buf, sizeof(buf), "Sleep in: %lus", (unsigned long)(remMs / 1000UL));
      line(7, 94, 1, buf);
    }

    if (state.settings.debugLevel > 0) {
      snprintf(buf, sizeof(buf), "Wake:%s Boot:%lu T:%lu", wakeToStr(state.wakeReason),
               (unsigned long)state.bootCount, (unsigned long)state.rtcTimerWakes);
      line(8, 106, 1, buf);
    } else {
      line(8, 106, 1, "");
    }

    hintL = "--";
    hintR = "--";
    hintC = "C: Menu";
    hintCHold = "Hold C: Sleep";
  }

  // MAIN MENU VIEW
  else if (view_ == View::MainMenu) {
    header("Menu");
    const char* items[4] = {"Status", "Settings", "Alerts", "Devices"};
    for (uint8_t i = 0; i < 4; i++) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%c %s", (i == menuIndex_) ? '>' : ' ', items[i]);
      line((size_t)(1 + i), 22 + (int)i * 12, 1, buf);
    }
    hintL = "L: Scroll";
    hintR = "R: Scroll";
    hintC = "C: Select";
    hintCHold = "Hold C: Back";
  }

  // DEVICES VIEW
  else if (view_ == View::Devices) {
    // Devices list uses more vertical space than the main safe zone.
    // Rounded corners primarily affect left/right edges, so keep X padding but use raw Y.
    {
      char hdr[32];
      const uint8_t count = state.deviceCount;
      if (count == 0) {
        snprintf(hdr, sizeof(hdr), "Devices");
      } else {
        const uint8_t sel = (devicesIndex_ < count) ? (uint8_t)(devicesIndex_ + 1) : 1;
        snprintf(hdr, sizeof(hdr), "Devices %u/%u", (unsigned)sel, (unsigned)count);
      }
      header(hdr);
    }
    const uint8_t count = state.deviceCount;
    if (count == 0) {
      lineAbs(1, 0, 16, 1, "No devices seen yet");
      lineAbs(2, 0, 24, 1, "Start scanning...");
    } else {
      // Sort indices by RSSI (desc), then newest lastSeen (desc)
      std::vector<uint8_t> idx;
      idx.reserve(count);
      for (uint8_t i = 0; i < count; i++) idx.push_back(i);
      std::sort(idx.begin(), idx.end(), [&](uint8_t a, uint8_t b) {
        const auto& da = state.devices[a];
        const auto& db = state.devices[b];
        if (da.rssi != db.rssi) return da.rssi > db.rssi;
        return da.lastSeenMs > db.lastSeenMs;
      });

      // Scroll selection references sorted list.
      if (devicesIndex_ >= count) devicesIndex_ = 0;
      const int headerH = 16; // size=2 line height
      const int rowH = 8;     // size=1 line height
      const int footerH = 16; // two footer rows at size=1
      const int h = display_.height();
      const int footerTop = std::max(0, h - footerH);
      const int listStartY = headerH;
      const int availPx = std::max(0, footerTop - listStartY);

      uint8_t visible = (uint8_t)(availPx / rowH);
      // Show fewer rows so we can keep each row flush-left and readable.
      if (visible > 2) {
        visible = (uint8_t)(visible - 2);
      } else {
        visible = 1;
      }
      // Limit by our line cache (line 0 is header).
      const uint8_t maxByCache = (lastLines_.size() > 1) ? (uint8_t)(lastLines_.size() - 1) : 1;
      if (visible > maxByCache) visible = maxByCache;

      uint8_t start = 0;
      if (count > visible) {
        // Keep selection in view while scrolling one item at a time.
        const uint8_t maxStart = (uint8_t)(count - visible);
        const uint8_t pad = (visible > 2) ? (uint8_t)(visible / 2) : 1;
        const uint8_t desired = (devicesIndex_ > pad) ? (uint8_t)(devicesIndex_ - pad) : 0;
        start = (desired > maxStart) ? maxStart : desired;
      }
      for (uint8_t row = 0; row < visible; row++) {
        const uint8_t sortedPos = (uint8_t)(start + row);
        if (sortedPos >= count) {
          lineAbs((size_t)(1 + row), 0, listStartY + (int)row * rowH, 1, "");
          continue;
        }
        const uint8_t i = idx[sortedPos];
        const auto& d = state.devices[i];

        const bool hover = (sortedPos == devicesIndex_);
        const char prefix = hover ? '>' : ' ';
        const char* name = d.name[0] ? d.name : "--";

        char buf[96];
        // Use pre-populated company name from device summary, or lookup if needed
        const char* company = "--";
        if (d.companyName[0]) {
          company = d.companyName;
        } else if (d.hasMsdCompanyId && d.msdCompanyId != 0) {
          company = btCompanyName(d.msdCompanyId, false); // Don't allow slow lookup during render
        }
        // Single-line entry, flush-left. Show RSSI - company - BLE name.
        // Keep both fields visible by bounding each chunk.
        snprintf(buf, sizeof(buf), "%c%4ddB - %.16s - %.18s", prefix, (int)d.rssi, company, name);
        lineAbs((size_t)(1 + row), 0, listStartY + (int)row * rowH, 1, buf);
      }

		// Clear any remaining cached rows from previous frames (prevents stale text when visible shrinks).
		for (size_t i = 1 + visible; i < lastLines_.size(); i++) {
        lineAbs(i, 0, listStartY + (int)(i - 1) * rowH, 1, "");
		}
    }

    hintL = "L: Scroll";
    hintR = "R: Scroll";
    hintC = "C: Actions";
    hintCHold = "Hold C: Back";
  }

  // DEVICE ACTION VIEW
  else if (view_ == View::DeviceAction) {
    header("Add Rule");
    
    if (selectedDeviceIndex_ >= state.deviceCount) {
      line(1, 22, 1, "No device selected");
      hintL = "--";
      hintR = "--";
      hintC = "--";
      hintCHold = "Hold C: Back";
    } else {
      const auto& d = state.devices[selectedDeviceIndex_];
      
      // Show device info at top
      char buf[64];
      const char* name = d.name[0] ? d.name : "--";
      snprintf(buf, sizeof(buf), "Dev: %.20s", name);
      line(1, 22, 1, buf);
      
      // Menu options with values
      char ouiStr[16] = "";
      char companyStr[32] = "";
      
      if (d.oui24 != 0) {
        formatOui_(d.oui24, ouiStr, sizeof(ouiStr));
      }
      
      if (d.hasMsdCompanyId && d.msdCompanyId != 0) {
        const char* companyName = btCompanyName(d.msdCompanyId, true);
        if (companyName) {
          snprintf(companyStr, sizeof(companyStr), "%.28s", companyName);
        }
      }
      
      for (uint8_t i = 0; i < 4; i++) {
        const bool hover = (i == deviceActionIndex_);
        const char* prefix = hover ? ">" : " ";
        
        switch (i) {
          case 0: // Add OUI
            if (d.oui24 != 0) {
              snprintf(buf, sizeof(buf), "%s Add OUI (%s)", prefix, ouiStr);
            } else {
              snprintf(buf, sizeof(buf), "%s Add OUI (n/a)", prefix);
            }
            break;
          case 1: // Add MAC
            snprintf(buf, sizeof(buf), "%s Add MAC (n/a)", prefix);
            break;
          case 2: // Add Company
            if (d.hasMsdCompanyId && d.msdCompanyId != 0 && companyStr[0]) {
              snprintf(buf, sizeof(buf), "%s Add Company (%s)", prefix, companyStr);
            } else {
              snprintf(buf, sizeof(buf), "%s Add Company (n/a)", prefix);
            }
            break;
          case 3: // Add Name
            if (d.name[0]) {
              snprintf(buf, sizeof(buf), "%s Add Name (%.20s)", prefix, d.name);
            } else {
              snprintf(buf, sizeof(buf), "%s Add Name (n/a)", prefix);
            }
            break;
        }
        
        line((size_t)(2 + i), 34 + (int)i * 12, 1, buf);
      }
      
      hintL = "L: Scroll";
      hintR = "R: Scroll";
      hintC = "C: Add";
      hintCHold = "Hold C: Back";
    }
  }

  // ALERTS CONFIG VIEW
  else if (view_ == View::AlertsConfig) {
    header("Alerts Config");
    char buf[64];
    const uint8_t itemCount = 6;

		uint8_t enabledPacks = 0;
		for (uint8_t i = 0; i < state.rulePackCount; i++) {
			if (state.rulePacks[i].enabled) enabledPacks++;
		}

    for (uint8_t i = 0; i < itemCount; i++) {
      const bool hover = (i == alertsConfigIndex_);
      const char* prefix = hover ? ">" : " ";
      switch (i) {
        case 0:
          snprintf(buf, sizeof(buf), "%s Packs %u/%u", prefix, (unsigned)enabledPacks, (unsigned)state.rulePackCount);
          break;
        case 1:
          snprintf(buf, sizeof(buf), "%s Service UUIDs: %u", prefix, (unsigned)state.ruleCountServiceUuid);
          break;
        case 2:
          snprintf(buf, sizeof(buf), "%s Company IDs: %u", prefix, (unsigned)state.ruleCountCompanyId);
          break;
        case 3:
          snprintf(buf, sizeof(buf), "%s Name contains: %u", prefix, (unsigned)state.ruleCountNameContains);
          break;
        case 4:
          snprintf(buf, sizeof(buf), "%s OUIs: %u", prefix, (unsigned)state.ruleCountOui);
          break;
        case 5:
          snprintf(buf, sizeof(buf), "%s MACs: %u", prefix, (unsigned)state.ruleCountMac);
          break;
        default:
          snprintf(buf, sizeof(buf), "%s --", prefix);
          break;
      }
      line((size_t)(1 + i), 22 + (int)i * 12, 1, buf);
    }

    hintL = "L: Scroll";
    hintR = "R: Scroll";
    hintC = "C: Select";
    hintCHold = "Hold C: Back";
  }

  // ALERTS RULE LIST VIEW
  else if (view_ == View::AlertsRuleList) {
    const char* kind = "Rules";
    switch (alertsRuleKind_) {
      case CoreState::RuleKind::ServiceUuid:
        kind = "Service";
        break;
      case CoreState::RuleKind::CompanyId:
        kind = "Company";
        break;
      case CoreState::RuleKind::NameContains:
        kind = "Name";
        break;
      case CoreState::RuleKind::Oui:
        kind = "OUI";
        break;
      case CoreState::RuleKind::Mac:
        kind = "MAC";
        break;
      default:
        kind = "Rules";
        break;
    }

    std::vector<uint8_t> idx;
    idx.reserve(state.ruleSummaryCount);
    for (uint8_t i = 0; i < state.ruleSummaryCount; i++) {
      if (state.ruleSummaries[i].kind == alertsRuleKind_) idx.push_back(i);
    }

    const uint8_t count = (uint8_t)idx.size();
    char hdr[32];
    if (count == 0) {
      snprintf(hdr, sizeof(hdr), "%s Rules", kind);
    } else {
      const uint8_t sel = (alertsRuleIndex_ < count) ? (uint8_t)(alertsRuleIndex_ + 1) : 1;
      snprintf(hdr, sizeof(hdr), "%s %u/%u", kind, (unsigned)sel, (unsigned)count);
    }
    header(hdr);

    if (count == 0) {
      line(1, 22, 1, "No rules");
      line(2, 34, 1, "Enable packs or add rules");
      line(7, 94, 1, "");
      line(8, 106, 1, "");
      hintL = "--";
      hintR = "--";
      hintC = "";
      hintCHold = "Hold C: Back";
    } else {
      if (alertsRuleIndex_ >= count) alertsRuleIndex_ = 0;
      const uint8_t visible = 6;
      const uint8_t pageStart = (uint8_t)((alertsRuleIndex_ / visible) * visible);
      for (uint8_t row = 0; row < visible; row++) {
        const uint8_t pos = (uint8_t)(pageStart + row);
        if (pos >= count) {
          line((size_t)(1 + row), 22 + (int)row * 12, 1, "");
          continue;
        }
        const uint8_t i = idx[pos];
        const auto& r = state.ruleSummaries[i];
        const bool hover = (pos == alertsRuleIndex_);
        const char* prefix = hover ? ">" : " ";

        char buf[64];
        const char* d = r.detail[0] ? r.detail : "--";
        snprintf(buf, sizeof(buf), "%s %c %.18s", prefix, r.enabled ? '+' : '-', d);
        line((size_t)(1 + row), 22 + (int)row * 12, 1, buf);
      }

      const auto& sel = state.ruleSummaries[idx[alertsRuleIndex_]];
      char buf[64];
      snprintf(buf, sizeof(buf), "Pack: %.16s", sel.pack[0] ? sel.pack : "--");
      line(7, 94, 1, buf);
      snprintf(buf, sizeof(buf), "Label: %.16s", sel.label[0] ? sel.label : "--");
      line(8, 106, 1, buf);

      hintL = "L: Scroll";
      hintR = "R: Scroll";
      hintC = "C: Toggle";
      hintCHold = "Hold C: Back";
    }
  }

  // SETTINGS VIEW
  else if (view_ == View::Settings) {
    header((editKind_ == EditKind::Setting) ? "Settings (EDIT)" : "Settings");
    char buf[64];
    // We show 7 editable settings + 1 submenu entry.
    for (uint8_t i = 0; i < 8; i++) {
      char value[24];
      switch (i) {
        case 0:
          snprintf(value, sizeof(value), "%lus", (unsigned long)(((editKind_ == EditKind::Setting && editIndex_ == 0) ? editSleepTimeoutMs_ : state.settings.sleepTimeoutMs) / 1000UL));
          break;
        case 1:
          snprintf(value, sizeof(value), "%lus", (unsigned long)(((editKind_ == EditKind::Setting && editIndex_ == 1) ? editWakeIntervalMs_ : state.settings.wakeIntervalMs) / 1000UL));
          break;
        case 2:
          snprintf(value, sizeof(value), "%lums", (unsigned long)((editKind_ == EditKind::Setting && editIndex_ == 2) ? editScanBurstMs_ : state.settings.scanBurstMs));
          break;
        case 3:
          snprintf(value, sizeof(value), "%s", ((editKind_ == EditKind::Setting && editIndex_ == 3) ? editAlertScreen_ : state.settings.alertScreen) ? "ON" : "OFF");
          break;
        case 4:
          snprintf(value, sizeof(value), "%s", ((editKind_ == EditKind::Setting && editIndex_ == 4) ? editAlertVibe_ : state.settings.alertVibe) ? "ON" : "OFF");
          break;
        case 5:
          snprintf(value, sizeof(value), ">>");
          break;
        case 6: {
          uint8_t level = (editKind_ == EditKind::Setting && editIndex_ == 6) ? editBacklightLevel_ : state.settings.backlightLevel;
          const char* levelName = "Min";
          switch (level) {
            case 0: levelName = "Min"; break;
            case 1: levelName = "Low"; break;
            case 2: levelName = "Med"; break;
            case 3: levelName = "High"; break;
            default: levelName = "--"; break;
          }
          snprintf(value, sizeof(value), "%s", levelName);
          break;
        }
        case 7:
          snprintf(value, sizeof(value), ">>");
          break;
        default:
          snprintf(value, sizeof(value), "--");
          break;
      }

      const char* name = "";
      switch (i) {
        case 0:
          name = "Sleep timeout";
          break;
        case 1:
          name = "Wake interval";
          break;
        case 2:
          name = "Scan burst";
          break;
        case 3:
          name = "Alert screen";
          break;
        case 4:
          name = "Alert vibe";
          break;
        case 5:
          name = "Debug options";
          break;
        case 6:
          name = "Backlight";
          break;
        case 7:
          name = "Wipe options";
          break;
      }

      const bool hover = (editKind_ != EditKind::Setting) && (i == settingsIndex_);
      const bool editing = (editKind_ == EditKind::Setting) && (i == editIndex_);
      const char* prefix = editing ? ">>" : (hover ? ">" : " ");
      snprintf(buf, sizeof(buf), "%s %s: %s", prefix, name, value);
      line((size_t)(1 + i), 22 + (int)i * 12, 1, buf);
    }
    if (editKind_ == EditKind::Setting) {
      hintL = "L: Change";
      hintR = "R: Change";
      hintC = "C: OK";
      hintCHold = "Hold C: Cancel";
    } else {
      hintL = "L: Scroll";
      hintR = "R: Scroll";
      hintC = (settingsIndex_ == 3 || settingsIndex_ == 4) ? "C: Toggle" : "C: Edit";
      hintCHold = "Hold C: Back";
    }
  }

  // WIPE OPTIONS SUBMENU
  else if (view_ == View::WipeOptions) {
    if (editKind_ == EditKind::Confirm) {
      header("Confirm");
      const char* what = "";
      switch (confirmAction_) {
        case ConfirmAction::WipeLocalStats:
          what = "Wipe local stats";
          break;
        case ConfirmAction::WipeCustomRules:
          what = "Wipe custom rules";
          break;
        case ConfirmAction::ResetSettings:
          what = "Reset settings";
          break;
        default:
          what = "--";
          break;
      }
      line(1, 22, 1, what);
      line(2, 34, 1, "Are you sure?");
      line(3, 46, 1, confirmYes_ ? ">> YES" : ">> NO");

      hintL = "L: No";
      hintR = "R: Yes";
      hintC = "C: OK";
      hintCHold = "Hold C: Cancel";
    } else {
      header("Wipe Options");
      const char* items[3] = {"Wipe local stats", "Wipe custom rules", "Reset settings"};
      for (uint8_t i = 0; i < 3; i++) {
        char buf[48];
        const bool hover = (i == wipeIndex_);
        snprintf(buf, sizeof(buf), "%s %s", hover ? ">" : " ", items[i]);
        line((size_t)(1 + i), 22 + (int)i * 12, 1, buf);
      }

      hintL = "L: Scroll";
      hintR = "R: Scroll";
      hintC = "C: Select";
      hintCHold = "Hold C: Back";
    }
  }

  // DEBUG OPTIONS SUBMENU
  else if (view_ == View::DebugOptions) {
    header((editKind_ == EditKind::Setting) ? "Debug (EDIT)" : "Debug Options");
    char buf[64];
    for (uint8_t i = 0; i < 2; i++) {
      char value[24];
      switch (i) {
        case 0: {
          uint8_t level = (editKind_ == EditKind::Setting && editIndex_ == 0) ? editDebugLevel_ : state.settings.debugLevel;
          const char* levelName = "Off";
          switch (level) {
            case 0: levelName = "Off"; break;
            case 1: levelName = "Low"; break;
            case 2: levelName = "High"; break;
            case 3: levelName = "Performance"; break;
            default: levelName = "--"; break;
          }
          snprintf(value, sizeof(value), "%s", levelName);
          break;
        }
        case 1:
          snprintf(value, sizeof(value), "%s", state.settings.debugLevel > 0 ? "ON" : "OFF");
          break;
        default:
          snprintf(value, sizeof(value), "--");
          break;
      }

      const char* name = "";
      switch (i) {
        case 0:
          name = "Debug level";
          break;
        case 1:
          name = "Serial output";
          break;
      }

      const bool hover = (editKind_ != EditKind::Setting) && (i == debugIndex_);
      const bool editing = (editKind_ == EditKind::Setting) && (i == editIndex_);
      const char* prefix = editing ? ">>" : (hover ? ">" : " ");
      snprintf(buf, sizeof(buf), "%s %s: %s", prefix, name, value);
      line((size_t)(1 + i), 22 + (int)i * 12, 1, buf);
    }
    if (editKind_ == EditKind::Setting) {
      hintL = "L: Change";
      hintR = "R: Change";
      hintC = "C: OK";
      hintCHold = "Hold C: Cancel";
    } else {
      hintL = "L: Scroll";
      hintR = "R: Scroll";
      hintC = "C: Edit";
      hintCHold = "Hold C: Back";
    }
  }

  // ALERTS VIEW
  else if (view_ == View::Alerts) {
    char buf[64];
    const bool flashing = (headerFlashActive_ && headerFlashView_ == View::Alerts);
    const uint16_t hdrColor = (flashing && headerFlashRed_) ? 0xF800 : 0xFFFF;
    
    // Keep "Alert: [name]" in header until view is exited
    std::string hdrText = "Alerts";
    if (state.lastAlertTsMs != 0) {
      char t[32];
      const char* label = state.lastAlertLabel[0] ? state.lastAlertLabel : "--";
      // At size=2, 240px wide -> ~20 chars; keep it short.
      snprintf(t, sizeof(t), "Alert: %.13s", label);
      hdrText = t;
    }
    renderHeader_(display_, hdrText, lastLines_[0], hdrColor, justCleared_);

    // Enabled pack list (comma-separated), wrapped across up to 2 lines.
    std::string enabledList;
		uint8_t enabledPacks = 0;
    for (uint8_t i = 0; i < state.rulePackCount; i++) {
      if (!state.rulePacks[i].enabled) continue;
			enabledPacks++;
      const char* name = state.rulePacks[i].name[0] ? state.rulePacks[i].name : "(unnamed)";
      if (!enabledList.empty()) enabledList += ", ";
      enabledList += name;
    }
    if (enabledList.empty()) enabledList = "--";

    // Rough fit for size=1 in safe area: ~30-32 chars.
    const size_t kMaxLine = 30;
		char packsHdr[16];
		snprintf(packsHdr, sizeof(packsHdr), "Packs %u/%u: ", (unsigned)enabledPacks, (unsigned)state.rulePackCount);
		std::string packsLine1 = packsHdr;
    std::string packsLine2;
    if ((packsLine1.size() + enabledList.size()) <= kMaxLine) {
      packsLine1 += enabledList;
    } else {
      // Split at the last comma+space that fits.
      const size_t avail = (kMaxLine > packsLine1.size()) ? (kMaxLine - packsLine1.size()) : 0;
      size_t cut = std::min(enabledList.size(), avail);
      if (cut < enabledList.size()) {
        const size_t comma = enabledList.rfind(", ", cut);
        if (comma != std::string::npos && comma > 0) cut = comma;
      }

      packsLine1 += enabledList.substr(0, cut);
      if (cut < enabledList.size()) {
        size_t start = cut;
        if (start < enabledList.size() && enabledList.compare(start, 2, ", ") == 0) start += 2;
        packsLine2 = enabledList.substr(start);
        if (packsLine2.size() > kMaxLine) {
          packsLine2 = packsLine2.substr(0, kMaxLine - 3);
          packsLine2 += "...";
        }
      }
    }

    if (state.lastAlertTsMs == 0) {
      line(1, 22, 1, "No alerts fired yet");
      line(2, 34, 1, "");
			snprintf(buf, sizeof(buf), "Rules O:%u C:%u N:%u M:%u S:%u",
			         (unsigned)state.ruleCountOui,
			         (unsigned)state.ruleCountCompanyId,
			         (unsigned)state.ruleCountNameContains,
			         (unsigned)state.ruleCountMac,
			         (unsigned)state.ruleCountServiceUuid);
			line(6, 82, 1, buf);
			line(7, 94, 1, packsLine1);
			line(8, 106, 1, packsLine2);
      hintL = "--";
      hintR = "--";
		hintC = "C: Config";
      hintCHold = "Hold C: Back";
    } else {
      // Show alert label in large text (size 2)
      const char* label = state.lastAlertLabel[0] ? state.lastAlertLabel : "--";
      line(1, 22, 2, label);
      
      // Show pack name
      const char* pack = state.lastAlertPack[0] ? state.lastAlertPack : "--";
      snprintf(buf, sizeof(buf), "Pack: %.20s", pack);
      line(2, 38, 1, buf);
      
      // Determine match type and show actual device data that triggered the alert
      if (state.lastAlertRuleAddr48 != 0) {
        // MAC address match - show the actual device MAC
        line(3, 50, 1, "Matched: MAC Address");
        char mac[18];
        const uint64_t addr = state.lastAlertRuleAddr48;
        const uint8_t b0 = (uint8_t)((addr >> 40) & 0xFF);
        const uint8_t b1 = (uint8_t)((addr >> 32) & 0xFF);
        const uint8_t b2 = (uint8_t)((addr >> 24) & 0xFF);
        const uint8_t b3 = (uint8_t)((addr >> 16) & 0xFF);
        const uint8_t b4 = (uint8_t)((addr >> 8) & 0xFF);
        const uint8_t b5 = (uint8_t)(addr & 0xFF);
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", b0, b1, b2, b3, b4, b5);
        snprintf(buf, sizeof(buf), "MAC: %s", mac);
        line(4, 62, 1, buf);
        // Show actual device name
        if (state.lastAlertDeviceName[0]) {
          snprintf(buf, sizeof(buf), "Name: %.18s", state.lastAlertDeviceName);
          line(5, 74, 1, buf);
        }
      } else if (state.lastAlertRuleCompanyId != 0) {
        // Company ID match - show actual company from device
        line(3, 50, 1, "Matched: Company ID");
        const uint16_t cid = state.lastAlertDeviceHasCompanyId ? state.lastAlertDeviceCompanyId : state.lastAlertRuleCompanyId;
        const char* companyName = btCompanyName(cid, true);
        snprintf(buf, sizeof(buf), "Company: %.18s", companyName);
        line(4, 62, 1, buf);
        // Show actual device name
        if (state.lastAlertDeviceName[0]) {
          snprintf(buf, sizeof(buf), "Name: %.18s", state.lastAlertDeviceName);
          line(5, 74, 1, buf);
        }
      } else if (state.lastAlertRuleOui24 != 0) {
        // OUI match - show actual OUI from device
        line(3, 50, 1, "Matched: OUI");
        const uint32_t oui = state.lastAlertDeviceOui24 != 0 ? state.lastAlertDeviceOui24 : state.lastAlertRuleOui24;
        char oui_str[10];
        formatOui_(oui, oui_str, sizeof(oui_str));
        snprintf(buf, sizeof(buf), "OUI: %s", oui_str);
        line(4, 62, 1, buf);
        // Show actual device name
        if (state.lastAlertDeviceName[0]) {
          snprintf(buf, sizeof(buf), "Name: %.18s", state.lastAlertDeviceName);
          line(5, 74, 1, buf);
        }
      } else if (state.lastAlertRuleName[0]) {
        // Name contains match - show actual full BLE name from device
        line(3, 50, 1, "Matched: BLE Name");
        const char* actualName = state.lastAlertDeviceName[0] ? state.lastAlertDeviceName : state.lastAlertRuleName;
        snprintf(buf, sizeof(buf), "Name: %.18s", actualName);
        line(4, 62, 1, buf);
      } else {
        // Service UUID match (or unknown)
        line(3, 50, 1, "Matched: Service UUID");
        snprintf(buf, sizeof(buf), "Service: %.18s", label);
        line(4, 62, 1, buf);
        // Show actual device name
        if (state.lastAlertDeviceName[0]) {
          snprintf(buf, sizeof(buf), "Name: %.18s", state.lastAlertDeviceName);
          line(5, 74, 1, buf);
        }
      }
      
      // Show age and RSSI - adjust line based on whether name was shown
      const uint32_t ageS = (now >= state.lastAlertTsMs) ? ((now - state.lastAlertTsMs) / 1000UL) : 0;
      snprintf(buf, sizeof(buf), "%lus ago  RSSI: %ddBm", (unsigned long)ageS, (int)state.lastAlertRssi);
      const bool hasName = (state.lastAlertDeviceName[0] != 0) && (state.lastAlertRuleName[0] == 0 || state.lastAlertRuleCompanyId != 0 || state.lastAlertRuleOui24 != 0 || state.lastAlertRuleAddr48 != 0);
      const int ageY = hasName ? 86 : 74;
      line(hasName ? 6 : 5, ageY, 1, buf);

      // Check if we're tracking and device was just seen (trigger flash)
      if (state.trackingOui24 != 0 && state.trackingLastSeenMs > lastTrackingSeenMs_) {
        lastTrackingSeenMs_ = state.trackingLastSeenMs;
        startHeaderFlash_(View::Alerts); // Flash when device is seen
      }

      // Menu options below the alert info
      const char* toggleText = state.lastAlertRuleEnabled ? "Disable Alert" : "Enable Alert";
      char opt1[32];
      snprintf(opt1, sizeof(opt1), "%s %s", (alertsIndex_ == 0) ? ">" : " ", toggleText);
      const char* opt2 = (alertsIndex_ == 1) ? "> Config" : "  Config";
      line(7, 98, 1, opt1);
      line(8, 110, 1, opt2);

      hintL = "L/R: Nav";
      hintR = "";
      hintC = "C: Select";
      hintCHold = "Hold C: Back";
    }
  }

  // ALERT PACKS SUBMENU
  else if (view_ == View::AlertPacks) {
    header("Alert Packs");
    const uint8_t count = state.rulePackCount;
    if (count == 0) {
      line(1, 22, 1, "No packs loaded");
      line(2, 34, 1, "Uploadfs /rules/*.json");
      hintL = "--";
      hintR = "--";
      hintC = "";
      hintCHold = "Hold C: Back";
    } else {
      // Simple paging so long pack lists don't overflow.
      const uint8_t visible = 6;
      const uint8_t pageStart = (uint8_t)((packIndex_ / visible) * visible);
      for (uint8_t row = 0; row < visible; row++) {
        const uint8_t i = (uint8_t)(pageStart + row);
        if (i >= count) {
          line((size_t)(1 + row), 22 + (int)row * 12, 1, "");
          continue;
        }
        char buf[64];
        const bool hover = (i == packIndex_);
        const bool enabled = state.rulePacks[i].enabled;
        const char* prefix = hover ? ">" : " ";
        snprintf(buf, sizeof(buf), "%s %.16s: %s",
                 prefix,
                 state.rulePacks[i].name[0] ? state.rulePacks[i].name : "(unnamed)",
                 enabled ? "ON" : "OFF");
        line((size_t)(1 + row), 22 + (int)row * 12, 1, buf);
      }
      hintL = "L: Scroll";
      hintR = "R: Scroll";
      hintC = "C: Toggle";
      hintCHold = "Hold C: Back";
    }
  }

  // Draw footer after main content so it stays consistent.
  // Row 1: left + center + right
  // Row 2: center (hold)
  if (lastLines_.size() >= 11) {
    const int footerSize = 1;
    const int lineH = 8 * footerSize;
    const int h = display_.height();
    const int yHold = std::max(0, h - lineH);
    const int yMain = std::max(0, yHold - lineH);

    renderFooterRow3_(display_, yMain, footerSize, hintL, hintC, hintR, lastLines_[9]);
    renderFooterCenter_(display_, yHold, footerSize, hintCHold, lastLines_[10]);
  }
}
