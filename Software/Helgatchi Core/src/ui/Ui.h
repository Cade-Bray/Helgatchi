
#pragma once

#include <Arduino.h>
#include <array>
#include <string>

#include "../core/CoreState.h"
#include "../core/EventBus.h"
#include "../core/Events.h"

class Display;
class Backlight;

class Ui {
public:
  Ui(Display& display, Backlight& backlight) : display_(display), backlight_(backlight) {}

  void begin();
  void tick(CoreState& state, EventBus& bus);

  void onButton(const ButtonEvent& b, CoreState& state, EventBus& bus);

  void setRootScreenMain();
  bool showAlertOverlay(const AlertFiredEvent& alert, CoreState& state);

private:
  enum class View : uint8_t {
    Status,
    MainMenu,
    Settings,
		Alerts,
		Devices,
    DeviceAction,
    AlertsConfig,
    AlertsRuleList,
		AlertPacks,
		WipeOptions,
		DebugOptions,
  };

  enum class EditKind : uint8_t {
    None,
    Setting,
    Pack,
    Confirm,
  };

  enum class ConfirmAction : uint8_t {
    None,
    WipeLocalStats,
    WipeCustomRules,
    ResetSettings,
  };

  Display& display_;
  Backlight& backlight_;
  uint32_t lastRenderMs_ = 0;
  uint32_t lastAlertRuleId_ = 0; // Track which alert we're showing
  uint32_t lastTrackingSeenMs_ = 0; // Track last device seen time during tracking
  bool alertShown_ = false; // Track if current alert has been shown to user
  bool dirty_ = true;

  bool needsClear_ = true;
  bool justCleared_ = false; // Track if we just cleared (skip fillRect until next frame)

  View view_ = View::Status;
  uint8_t menuIndex_ = 0;
  uint8_t settingsIndex_ = 0;
	uint8_t packIndex_ = 0;
  uint8_t devicesIndex_ = 0;
  uint8_t selectedDeviceIndex_ = 0; // Actual array index of selected device
  uint8_t deviceActionIndex_ = 0;
  uint8_t wipeIndex_ = 0;
  uint8_t debugIndex_ = 0;
  uint8_t alertsIndex_ = 0;
  uint8_t alertsConfigIndex_ = 0;
  uint8_t alertsRuleIndex_ = 0;
  CoreState::RuleKind alertsRuleKind_ = CoreState::RuleKind::Oui;

  EditKind editKind_ = EditKind::None;
  uint8_t editIndex_ = 0;

  // Temporary edit buffer (only committed on short-press Center).
  uint32_t editSleepTimeoutMs_ = 0;
  uint32_t editWakeIntervalMs_ = 0;
  uint32_t editScanBurstMs_ = 0;
  bool editAlertScreen_ = false;
  bool editAlertVibe_ = false;
  uint8_t editDebugLevel_ = 0;
  uint8_t editBacklightLevel_ = 0;
  bool editPackEnabled_ = false;
  bool confirmYes_ = false;
  ConfirmAction confirmAction_ = ConfirmAction::None;

  static constexpr size_t kLineCount = 32;
  std::array<std::string, kLineCount> lastLines_{};

  // Auto-scroll: long-press L/R continuously scrolls in scrollable views.
  bool autoScrollActive_ = false;
  int8_t autoScrollDir_ = 0; // -1 or +1
  View autoScrollView_ = View::Status;
  uint32_t autoScrollNextMs_ = 0;

  static constexpr uint32_t kAutoScrollInitialDelayMs = 250;
  static constexpr uint32_t kAutoScrollRepeatMs = 90;

  void stopAutoScroll_();
  bool canAutoScrollInView_(const CoreState& state) const;
  void stepScroll_(int8_t dir, const CoreState& state);
  void tickAutoScroll_(const CoreState& state, uint32_t nowMs);

  // Alert attention: flash header red a few times.
  bool headerFlashActive_ = false;
  bool headerFlashRed_ = false;
  uint8_t headerFlashTogglesLeft_ = 0;
  uint32_t headerFlashNextMs_ = 0;
  View headerFlashView_ = View::Alerts;

  static constexpr uint32_t kHeaderFlashIntervalMs = 160;

  void startHeaderFlash_(View v);
  void tickHeaderFlash_(uint32_t nowMs);
};
