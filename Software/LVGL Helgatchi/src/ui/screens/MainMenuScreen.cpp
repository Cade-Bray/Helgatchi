#include "MainMenuScreen.h"

#include "ScreenLayout.h"
#include "ScreenRouter.h"
#include "../lvgl/LvglContext.h"

// Simple data model for the main menu.
struct MenuItem {
  const char *title;
  const char *sub1;
  const char *sub2;
  const char *sub3;
  void (*action)();
};

static const MenuItem kMenuItems[] = {
  {"Status",  "Battery: 78%", "Uptime: 01:23", "Devices: 12", show_status_screen},
  {"Alerts",  "Active: 0",    "Last: none",   "Rules: 5",   show_alerts_screen},
  {"Devices", "Nearby: 3",    "Tracked: 12",  "New: 1",     nullptr},
  {"Settings","Brightness",  "Buttons",      "Sleep",      nullptr},
  {"About",   "Helgatchi",    "v0.1",         "LVGL",       nullptr},
};

struct MenuState {
  uint8_t index = 0;
  uint8_t count = 0;
  lv_obj_t *cards[8]{};
};

static void update_menu_(MenuState &state) {
  (void)state;
}

static void menu_on_delete_(lv_event_t *e) {
  auto *state = static_cast<MenuState *>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_target(e))));
  if (state) {
    lv_free(state);
  }
}

static void menu_key_cb_(lv_event_t *e) {
  const uint32_t key = lv_event_get_key(e);
  lv_obj_t *container = static_cast<lv_obj_t *>(lv_event_get_target(e));
  auto *state = static_cast<MenuState *>(lv_obj_get_user_data(container));
  if (!state) return;

  if (key == LV_KEY_LEFT) {
    state->index = (uint8_t)((state->index + state->count - 1) % state->count);
    if (state->cards[state->index]) {
      lv_obj_scroll_to_view(state->cards[state->index], LV_ANIM_ON);
    }
    update_menu_(*state);
  } else if (key == LV_KEY_RIGHT) {
    state->index = (uint8_t)((state->index + 1) % state->count);
    if (state->cards[state->index]) {
      lv_obj_scroll_to_view(state->cards[state->index], LV_ANIM_ON);
    }
    update_menu_(*state);
  } else if (key == LV_KEY_ENTER) {
    const MenuItem &item = kMenuItems[state->index];
    if (item.action) {
      item.action();
    }
  }
}

// Build a modal-style main menu screen with placeholder widgets.
// Kept intentionally simple and close to LVGL docs examples.
lv_obj_t* create_main_menu_screen() {
  // Build the standard screen layout.
  ScreenLayout layout = screen_layout_create_standard();
  screen_layout_set_status_text(layout, "  0 alerts", "Main menu", "47%  ");

  // Configure the content container for the main menu.
  lv_obj_t *content = layout.content;
  lv_obj_set_style_pad_row(content, 0, 0);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Main menu title outside the modal panel.
//   lv_obj_t *screen_title = lv_label_create(content);
//   lv_label_set_text(screen_title, "Main Menu");
//   lv_obj_set_style_text_font(screen_title, LV_FONT_DEFAULT, 0);
//   lv_obj_set_style_text_align(screen_title, LV_TEXT_ALIGN_CENTER, 0);

  // Horizontal snapping container for the menu cards (patterned after lv_example_scroll_2).
  // This replaces the extra nested panel so the structure stays simple.
  lv_obj_t *card_row = lv_obj_create(content);
  lv_obj_set_size(card_row, lv_pct(92), lv_pct(78));
  lv_obj_set_scroll_dir(card_row, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(card_row, LV_SCROLL_SNAP_CENTER);
  lv_obj_add_flag(card_row, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scrollbar_mode(card_row, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_all(card_row, 0, 0);
  lv_obj_set_style_pad_column(card_row, 12, 0);
  lv_obj_set_style_radius(card_row, 8, 0);
  lv_obj_set_style_border_width(card_row, 2, 0);
  lv_obj_set_flex_flow(card_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Create cards for each menu item.
  const uint8_t count = (uint8_t)(sizeof(kMenuItems) / sizeof(kMenuItems[0]));
  for (uint8_t i = 0; i < count; i++) {
    const MenuItem &item = kMenuItems[i];
    lv_obj_t *card = lv_obj_create(card_row);
    lv_obj_set_size(card, 150, lv_pct(100));
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, item.title);
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);

    lv_obj_t *sub1 = lv_label_create(card);
    lv_label_set_text(sub1, item.sub1);

    lv_obj_t *sub2 = lv_label_create(card);
    lv_label_set_text(sub2, item.sub2);

    lv_obj_t *sub3 = lv_label_create(card);
    lv_label_set_text(sub3, item.sub3);

    // Store references for snapping and indicator updates.
    if (i < (uint8_t)(sizeof(MenuState::cards) / sizeof(MenuState::cards[0]))) {
      // We'll fill the state later once it's allocated.
    }
  }

  // Allocate and attach state to the card container.
  auto *state = static_cast<MenuState *>(lv_malloc(sizeof(MenuState)));
  if (state) {
    *state = {};
    state->count = count;

    // Populate card references from the row container.
    lv_obj_t *child = lv_obj_get_child(card_row, 0);
    for (uint8_t i = 0; i < count && child; i++) {
      state->cards[i] = child;
      child = lv_obj_get_child(card_row, i + 1);
    }

    lv_obj_set_user_data(card_row, state);
    update_menu_(*state);
    lv_obj_add_event_cb(card_row, menu_on_delete_, LV_EVENT_DELETE, nullptr);
  }

  // Snap to the first card once everything is created.
  lv_obj_update_snap(card_row, LV_ANIM_ON);

  // Focus and key handling for left/right navigation.
  lv_group_t *group = lvgl_get_input_group();
  if (group) {
    lv_group_add_obj(group, card_row);
    lv_group_focus_obj(card_row);
    lv_obj_add_event_cb(card_row, menu_key_cb_, LV_EVENT_KEY, nullptr);
  }

  // Set the hints for this screen.
  screen_layout_set_hotbar_hints(layout, "L = Prev", "C = Select", "R = Next");

  return layout.screen;
}
