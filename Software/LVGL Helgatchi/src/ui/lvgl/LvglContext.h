#pragma once

#include <lvgl.h>

// Simple shared context for LVGL input focus group.
void lvgl_set_input_group(lv_group_t *group);

lv_group_t *lvgl_get_input_group();
