#pragma once
#include "../../core/CoreState.h"
#include "../../core/EventBus.h"
#include "../../core/Events.h"

class Display;

class Screen {
public:
  virtual ~Screen() = default;

  // Render full screen (use dirty)
  virtual void render(Display& d, const CoreState& s) = 0;

  // Handle input; may push events or request navigation
  virtual void onButton(const ButtonEvent& b, CoreState& s, EventBus& bus) = 0;

  // Optional periodic updates
  virtual void tick(CoreState& s, EventBus& bus) {}
};
