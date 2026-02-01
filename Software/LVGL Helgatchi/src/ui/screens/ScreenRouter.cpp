#include "ScreenRouter.h"

#include <lvgl.h>

#include "MainMenuScreen.h"
#include "StatusScreen.h"
#include "AlertsScreen.h"

// Load a new screen and safely delete the previous one.
static void load_screen_(lv_obj_t *screen) {
  lv_obj_t *prev = lv_scr_act();
  lv_scr_load(screen);
  if (prev && prev != screen) {
    lv_obj_del_async(prev);
  }
}

void show_main_menu() {
  lv_obj_t *screen = create_main_menu_screen();
  load_screen_(screen);
}

void show_status_screen() {
  lv_obj_t *screen = create_status_screen();
  load_screen_(screen);
}

void show_alerts_screen() {
  lv_obj_t *screen = create_alerts_screen();
  load_screen_(screen);
}
