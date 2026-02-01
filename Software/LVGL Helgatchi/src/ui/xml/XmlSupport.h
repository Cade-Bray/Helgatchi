#pragma once

#include <lvgl.h>

// Initialize LVGL XML support (filesystem driver + XML module).
// Returns true if XML support is ready to load assets.
bool xml_support_init();

// Load all XML components/screens/translations from a path (e.g. "S:/ui").
bool xml_support_load_all(const char *path);

// Try to create a screen by XML name. Returns nullptr if not found.
lv_obj_t *xml_support_try_create_screen(const char *name);
