#pragma once
#include "event_bus.h"
#include "settings_service.h"

class LogService : public IEventHandler {
public:
    // Call after g_settings.begin(). Uses subscribeAll — one bus slot.
    void begin(EventBus& bus);

    // IEventHandler
    void onEvent(const Event& e) override;

    // Re-evaluate the LVGL FPS overlay's visibility against the current
    // debug level. Must be called any time the LVGL display might have been
    // (re)created or when render is being re-enabled — LVGL auto-shows the
    // overlay when LV_USE_PERF_MONITOR=1 in lv_display_create(), so we have
    // to actively re-hide it after init unless we're at RENDERING_PERF+.
    void applyPerfMonitor() { _applyPerfMonitor(); }

private:
    void _syncSettings();
    void _applyPerfMonitor();    // show/hide LVGL FPS overlay based on level
    void _emitPerfLine();        // one-line perf summary at PERF level
    static const char* _eventName(EventId id);

    bool       _enabled     = false;
    DebugLevel _debug_level = DEBUG_INFORMATIONAL;
};

extern LogService g_logger;
