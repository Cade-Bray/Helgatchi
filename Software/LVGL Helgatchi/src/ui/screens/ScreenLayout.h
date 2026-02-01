#pragma once

#include <lvgl.h>

// Standard reusable screen layout: top status bar, center content area, bottom hotbar.
// Use this for all screens unless a custom layout is explicitly required.
struct ScreenLayout {
  lv_obj_t *screen = nullptr;
  lv_obj_t *statusbar = nullptr;
  lv_obj_t *content = nullptr;
  lv_obj_t *hotbar = nullptr;
};

// Create the standard layout and return handles for customization.
ScreenLayout screen_layout_create_standard();

// Convenience helpers to update the standard bars.
void screen_layout_set_status_text(ScreenLayout &layout, const char *left, const char *center, const char *right);
void screen_layout_set_hotbar_hints(ScreenLayout &layout, const char *left, const char *center, const char *right);
