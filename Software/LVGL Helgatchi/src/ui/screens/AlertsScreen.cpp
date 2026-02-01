#include "AlertsScreen.h"

#include "ScreenLayout.h"
#include "ScreenRouter.h"
#include "../lvgl/LvglContext.h"

// Key handler for the alerts screen.
static void alerts_key_cb(lv_event_t *e) {
  const uint32_t key = lv_event_get_key(e);
  if (key == LV_KEY_ESC) {
    show_main_menu();
  }
}

lv_obj_t* create_alerts_screen() {
  // Build the standard screen layout.
  ScreenLayout layout = screen_layout_create_standard();
  screen_layout_set_status_text(layout, "Menu", "Alerts", "");

  // Configure the content container for the alerts page.
  lv_obj_t *content = layout.content;
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  // Title.
  lv_obj_t *title = lv_label_create(content);
  lv_label_set_text(title, "Alerts");
  lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);

  // Dummy info rows.
  lv_obj_t *row1 = lv_label_create(content);
  lv_label_set_text(row1, "Active alerts: 0");

  lv_obj_t *row2 = lv_label_create(content);
  lv_label_set_text(row2, "Last alert: none");

  lv_obj_t *row3 = lv_label_create(content);
  lv_label_set_text(row3, "Rules enabled: 5");

  // Hotbar with back hint.
  screen_layout_set_hotbar_hints(layout, "L = Up", "Hold C = Back", "R = Down");

  // Ensure the screen receives key events by focusing a tiny hidden button.
  lv_group_t *group = lvgl_get_input_group();
  if (group) {
    lv_obj_t *focus = lv_button_create(content);
    lv_obj_set_size(focus, 1, 1);
    lv_obj_set_style_opa(focus, LV_OPA_0, 0);
    lv_obj_clear_flag(focus, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(group, focus);
    lv_group_focus_obj(focus);
    lv_obj_add_event_cb(focus, alerts_key_cb, LV_EVENT_KEY, nullptr);
  }

  return layout.screen;
}
