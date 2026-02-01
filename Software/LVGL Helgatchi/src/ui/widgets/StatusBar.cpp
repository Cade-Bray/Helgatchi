#include "StatusBar.h"

// Internal references stored as user data on the status bar container.
struct StatusBarRefs {
  lv_obj_t *left_label;
  lv_obj_t *center_label;
  lv_obj_t *right_label;
};

static void statusbar_on_delete(lv_event_t *e) {
  lv_obj_t *bar = static_cast<lv_obj_t *>(lv_event_get_target(e));
  auto *refs = static_cast<StatusBarRefs *>(lv_obj_get_user_data(bar));
  if (refs) {
    lv_free(refs);
    lv_obj_set_user_data(bar, nullptr);
  }
}

lv_obj_t *statusbar_create(lv_obj_t *parent) {
  // Create a container for the status bar.
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_set_size(bar, lv_pct(100), 26);
  lv_obj_set_style_pad_all(bar, 4, 0);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_set_style_radius(bar, 0, 0);

  // Use a 3-column grid so left/center/right stay aligned.
  static lv_coord_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static lv_coord_t rows[] = {LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
  lv_obj_set_grid_dsc_array(bar, cols, rows);

  lv_obj_t *left = lv_label_create(bar);
  lv_label_set_text(left, "Menu");
  lv_obj_set_grid_cell(left, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  lv_obj_t *center = lv_label_create(bar);
  lv_label_set_text(center, "Main");
  lv_obj_set_grid_cell(center, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  lv_obj_t *right = lv_label_create(bar);
  lv_label_set_text(right, "--");
  lv_obj_set_grid_cell(right, LV_GRID_ALIGN_END, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);

  // Store label pointers so we can update text quickly later.
  auto *refs = static_cast<StatusBarRefs *>(lv_malloc(sizeof(StatusBarRefs)));
  if (refs) {
    refs->left_label = left;
    refs->center_label = center;
    refs->right_label = right;
    lv_obj_set_user_data(bar, refs);
    lv_obj_add_event_cb(bar, statusbar_on_delete, LV_EVENT_DELETE, nullptr);
  }

  return bar;
}

void statusbar_set_text(lv_obj_t *bar, const char *left, const char *center, const char *right) {
  auto *refs = static_cast<StatusBarRefs *>(lv_obj_get_user_data(bar));
  if (!refs) return;

  if (left) {
    lv_label_set_text(refs->left_label, left);
  }
  if (center) {
    lv_label_set_text(refs->center_label, center);
  }
  if (right) {
    lv_label_set_text(refs->right_label, right);
  }
}
