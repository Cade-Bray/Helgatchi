#pragma once

#include <stdint.h>

#include "CoreState.h"
#include "EventBus.h"

class Buttons;
class Display;
class Backlight;
class VibrationMotor;

class Store;
class PowerManager;
class ScanCoordinator;
class RulesManager;
class AlertEngine;
class Telemetry;
class Ui;

class Core {
public:
  Core();
  ~Core();

  void setup();
  void loop();

private:
  void initSubsystems_();
  void postInitialEvents_();

  void handleEvent_(const Event& e);
  void tick_();

  void enterInteractive_();
  void enterIdleAwake_();
  void enterScanBurst_();
  void enterPreSleep_();
  void maybeSleep_();

  void updateBleUnique_(uint64_t addr48);
  uint32_t estimateBleUnique_() const;
  void maybePersistBleUnique_(uint32_t nowMs);
	void updateRulePackSummary_();
  void updateRuleCounts_();
  void updateRuleSummaries_();

private:
  AppState state_{};
  EventBus bus_;

  Buttons* buttons_ = nullptr;
  Display* display_ = nullptr;
  Backlight* backlight_ = nullptr;
  VibrationMotor* vibe_ = nullptr;

  Store* store_ = nullptr;
  PowerManager* power_ = nullptr;
  ScanCoordinator* scan_ = nullptr;
  RulesManager* rules_ = nullptr;
  AlertEngine* alerts_ = nullptr;
  Telemetry* telemetry_ = nullptr;

  Ui* ui_ = nullptr;

  static constexpr uint8_t kBleHllP_ = 10; // 2^10 = 1024 registers (~1KB)
  static constexpr size_t kBleHllM_ = (size_t)1u << kBleHllP_;
  uint8_t bleHll_[kBleHllM_]{};
  bool bleHllDirty_ = false;
  uint32_t lastBleHllSaveMs_ = 0;

  uint32_t lastTickMs_;
};
