#include "power_menu_screen.h"
#include "event_ids.h"
#include "UI/screens.h"
#include <lvgl.h>
#include <stdio.h>

PowerMenuScreen g_power_menu_screen;

static EventBus* _bus = nullptr;

// ---------------------------------------------------------------------------
// Button CLICKED callbacks — each hands off to PowerManager (peripheral
// teardown + the actual transition live there, same as the Settings buttons).
// ---------------------------------------------------------------------------

static void _on_sleep_now(lv_event_t* /*e*/) {
    if (_bus) _bus->post(CMD_POWER_SLEEP);
}

static void _on_restart(lv_event_t* /*e*/) {
    if (_bus) _bus->post(CMD_POWER_REBOOT);
}

static void _on_power_off(lv_event_t* /*e*/) {
    // Deep sleep, no timer — wakes only on a CENTER long-hold. Unlike shipping
    // it leaves the tutorial flag intact.
    if (_bus) _bus->post(CMD_POWER_DOWN);
}

// ---------------------------------------------------------------------------
// Lifecycle — must follow g_ui.begin() so objects.* are valid.
// ---------------------------------------------------------------------------

void PowerMenuScreen::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(EV_SLEEP_COUNTDOWN_UPDATED, this);

    lv_obj_add_event_cb(objects.sleep_now_button, _on_sleep_now,  LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.restart_button,   _on_restart,    LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.power_off_,       _on_power_off,  LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// IEventHandler — reflect the sleep countdown while this screen is active.
// Same 1 Hz EV_SLEEP_COUNTDOWN_UPDATED PowerManager posts for the Settings
// screen: 0xFFFF = inhibited ("will not auto-sleep"), else seconds remaining.
// ---------------------------------------------------------------------------

void PowerMenuScreen::onEvent(const Event& e) {
    if (e.id != EV_SLEEP_COUNTDOWN_UPDATED) return;
    if (lv_scr_act() != objects.power_menu) return;

    uint16_t s = e.data.sleep_count.seconds;
    if (s == 0xFFFFu) {
        lv_label_set_text_static(objects.sleep_countdown_text,
                                 "Sleep now (will not auto-sleep)");
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "Sleep now (will sleep in %us)", s);
        lv_label_set_text(objects.sleep_countdown_text, buf);
    }
}
