#include "power_menu_screen.h"
#include "event_ids.h"
#include "UI/screens.h"
#include "UI/eez-flow.h"   // eez_flow_set_screen
#include <lvgl.h>
#include <stdio.h>

PowerMenuScreen g_power_menu_screen;

static EventBus* _bus = nullptr;

// ---------------------------------------------------------------------------
// Power action → Power Action screen → (hold) → command on the bus
//
// A button doesn't fire its command immediately. It swaps to the Power Action
// screen (fade-in, so "Restarting now…" etc. is visible), then a one-shot
// timer posts the actual CMD_POWER_* after a short hold — a visual beat before
// the device tears down. Command post is deferred anyway (the bus drains next
// loop), so the message stays on screen through the transition.
// ---------------------------------------------------------------------------

static constexpr uint32_t POWER_ACTION_HOLD_MS = 1800;   // in the user's 1.5–2 s window

static lv_timer_t* _action_timer   = nullptr;
static EventId     _pending_action = CMD_POWER_SLEEP;    // set before the timer is armed

static void _action_timer_cb(lv_timer_t* /*t*/) {
    // Delete first: the command is queued (not synchronous), so without this
    // the repeating timer would re-fire the transition every hold interval.
    if (_action_timer) { lv_timer_delete(_action_timer); _action_timer = nullptr; }
    if (_bus) _bus->post(_pending_action);
}

static void _beginPowerAction(EventId cmd, const char* msg) {
    if (_action_timer) return;   // an action is already counting down
    _pending_action = cmd;
    lv_label_set_text_static(objects.power_action_text, msg);
    eez_flow_set_screen(SCREEN_ID_POWER_ACTION_SCREEN, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
    _action_timer = lv_timer_create(_action_timer_cb, POWER_ACTION_HOLD_MS, nullptr);
}

// ---------------------------------------------------------------------------
// Button CLICKED callbacks — show the action screen, then hand off to
// PowerManager (peripheral teardown + the actual transition live there).
// ---------------------------------------------------------------------------

static void _on_sleep_now(lv_event_t* /*e*/) {
    _beginPowerAction(CMD_POWER_SLEEP, "Sleeping now...");
}

static void _on_restart(lv_event_t* /*e*/) {
    _beginPowerAction(CMD_POWER_REBOOT, "Restarting now...");
}

static void _on_power_off(lv_event_t* /*e*/) {
    // Deep sleep, no timer — wakes only on a CENTER long-hold. Unlike shipping
    // it leaves the tutorial flag intact.
    _beginPowerAction(CMD_POWER_DOWN, "Powering off...");
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
