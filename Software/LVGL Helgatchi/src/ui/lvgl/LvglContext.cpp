#include "LvglContext.h"

static lv_group_t *g_input_group = nullptr;

void lvgl_set_input_group(lv_group_t *group) {
  g_input_group = group;
}

lv_group_t *lvgl_get_input_group() {
  return g_input_group;
}
