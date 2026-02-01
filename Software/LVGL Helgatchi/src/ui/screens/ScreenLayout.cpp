#include "ScreenLayout.h"

#include "../widgets/Hotbar.h"
#include "../widgets/StatusBar.h"

ScreenLayout screen_layout_create_standard() {
  ScreenLayout layout{};

  // Create a new screen object (not attached to the display yet).
  layout.screen = lv_obj_create(NULL);

  // Standard layout: status bar (top), content (middle), hotbar (bottom).
  lv_obj_set_flex_flow(layout.screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(layout.screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(layout.screen, 0, 0);
  lv_obj_set_style_pad_row(layout.screen, 0, 0);

  // Status bar at the top.
  layout.statusbar = statusbar_create(layout.screen);

  // Content container grows to fill remaining space between bars.
  layout.content = lv_obj_create(layout.screen);
  lv_obj_set_width(layout.content, lv_pct(100));
  lv_obj_set_flex_grow(layout.content, 1);
  lv_obj_set_style_pad_all(layout.content, 12, 0);
  lv_obj_set_style_pad_row(layout.content, 6, 0);

  // Hotbar at the bottom.
  layout.hotbar = hotbar_create(layout.screen);

  return layout;
}

void screen_layout_set_status_text(ScreenLayout &layout, const char *left, const char *center, const char *right) {
  if (!layout.statusbar) return;
  statusbar_set_text(layout.statusbar, left, center, right);
}

void screen_layout_set_hotbar_hints(ScreenLayout &layout, const char *left, const char *center, const char *right) {
  if (!layout.hotbar) return;
  hotbar_set_hints(layout.hotbar, left, center, right);
}
