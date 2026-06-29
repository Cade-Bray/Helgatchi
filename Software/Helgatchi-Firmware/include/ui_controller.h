#pragma once
#include "event_bus.h"
#include <lvgl.h>

class UIController : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // PowerManager calls this from _setDisplay so we skip lv_timer_handler
    // when the screen is off — saves the ~70 % CPU LVGL spends rendering
    // invisible frames during silent (TIMER-wake) scan windows.
    void setRenderEnabled(bool enabled) { _render_enabled = enabled; }

private:
    EventBus* _bus = nullptr;
    bool      _render_enabled = true;
};

extern UIController g_ui;
