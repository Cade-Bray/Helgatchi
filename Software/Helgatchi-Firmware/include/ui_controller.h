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

    // Ground-truth display flush rate, independent of LVGL's perf overlay.
    // Returns the number of flush_cb invocations since the previous call to
    // this function plus the elapsed milliseconds since that call. Resets
    // the internal counter; first call reports stats since boot.
    void getDisplayStats(uint32_t& flushes_out, uint32_t& elapsed_ms_out);

private:
    void _refreshNoAlertsLabel();
    EventBus* _bus = nullptr;
    bool      _render_enabled = true;
};

extern UIController g_ui;
