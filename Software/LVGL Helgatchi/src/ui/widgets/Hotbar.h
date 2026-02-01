#pragma once

#include <lvgl.h>

// Simple reusable hotbar widget shown at the bottom of every screen.
// Each screen can update the hints (Left / Center / Right).

// Create a hotbar attached to the given parent (usually a screen).
// Returns the hotbar object so the caller can update it later.
lv_obj_t *hotbar_create(lv_obj_t *parent);

// Update the hotbar text hints.
// Pass nullptr to leave a field unchanged.
void hotbar_set_hints(lv_obj_t *hotbar, const char *left, const char *center, const char *right);
