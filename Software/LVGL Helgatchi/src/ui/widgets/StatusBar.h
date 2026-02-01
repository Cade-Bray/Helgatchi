#pragma once

#include <lvgl.h>

// Simple reusable status bar shown at the top of every screen.
// Each screen can update the left/center/right fields.

// Create a status bar attached to the given parent (usually a screen).
// Returns the bar object so the caller can update it later.
lv_obj_t *statusbar_create(lv_obj_t *parent);

// Update the status bar text fields.
// Pass nullptr to leave a field unchanged.
void statusbar_set_text(lv_obj_t *bar, const char *left, const char *center, const char *right);
