
#pragma once

#include "../core/EventBus.h"
#include "../core/Events.h"
#include "../core/CoreState.h"

class RulesManager;

class AlertEngine {
public:
  void onWifiSighting(const WifiSightingEvent& e, const RulesManager& rules, CoreState& state, EventBus& bus);
  void onBleSighting(const BleSightingEvent& e, const RulesManager& rules, CoreState& state, EventBus& bus);

  void setMinIntervalMs(uint32_t ms) { minIntervalMs_ = ms; }

private:
  bool shouldFire_(uint32_t ruleId, uint32_t nowMs);

  uint32_t minIntervalMs_ = 2000;
  struct LastFire {
    uint32_t ruleId;
    uint32_t tsMs;
  };
  LastFire last_[16] = {};
  uint8_t lastCount_ = 0;
};
