#pragma once
#include "../core/EventBus.h"
#include "../core/CoreState.h"
#include "../core/Events.h"

struct AppContext {
  // TODO: pointers/refs to shared services (store, rules, scan coordinator, etc.)
};

class IApp {
public:
  virtual ~IApp() = default;

  virtual const char* name() const = 0;

  // Called at boot
  virtual void init(const AppContext& ctx) = 0;

  // Subscribe to events
  virtual void onEvent(const Event& e, AppState& state, EventBus& bus) = 0;

  // Optional: contribute scan needs / power hints / UI screens
  // TODO: scanNeeds(), powerHints(), registerScreens()
};
