#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_MAIN_MENU = 1,
    SCREEN_ID_TUTORIAL_SPLASH_SCREEN = 2,
    SCREEN_ID_TUTORIAL = 3,
    SCREEN_ID_SETTINGS = 4,
    SCREEN_ID_INFO = 5,
    SCREEN_ID_SCREEN_TEMPLATE = 6,
    SCREEN_ID_ALERTS = 7,
    SCREEN_ID_DEVICES = 8,
    SCREEN_ID_DEVICE_UPDATING = 9,
    SCREEN_ID_DEBUG_INFO = 10,
    SCREEN_ID_OVERVIEW = 11,
    SCREEN_ID_POWER_MENU = 12,
    _SCREEN_ID_LAST = 12
};

typedef struct _objects_t {
    lv_obj_t *main_menu;
    lv_obj_t *tutorial_splash_screen;
    lv_obj_t *tutorial;
    lv_obj_t *settings;
    lv_obj_t *info;
    lv_obj_t *screen_template;
    lv_obj_t *alerts;
    lv_obj_t *devices;
    lv_obj_t *device_updating;
    lv_obj_t *debug_info;
    lv_obj_t *overview;
    lv_obj_t *power_menu;
    lv_obj_t *obj0;
    lv_obj_t *obj0__top_bar;
    lv_obj_t *obj0__left_text;
    lv_obj_t *obj0__top_bar_center_text;
    lv_obj_t *obj0__right_text;
    lv_obj_t *settings_top_bar;
    lv_obj_t *settings_top_bar__top_bar;
    lv_obj_t *settings_top_bar__left_text;
    lv_obj_t *settings_top_bar__top_bar_center_text;
    lv_obj_t *settings_top_bar__right_text;
    lv_obj_t *obj1;
    lv_obj_t *obj1__top_bar;
    lv_obj_t *obj1__left_text;
    lv_obj_t *obj1__top_bar_center_text;
    lv_obj_t *obj1__right_text;
    lv_obj_t *obj2;
    lv_obj_t *obj2__top_bar;
    lv_obj_t *obj2__left_text;
    lv_obj_t *obj2__top_bar_center_text;
    lv_obj_t *obj2__right_text;
    lv_obj_t *obj3;
    lv_obj_t *obj3__main_content;
    lv_obj_t *obj4;
    lv_obj_t *obj4__top_bar;
    lv_obj_t *obj4__left_text;
    lv_obj_t *obj4__top_bar_center_text;
    lv_obj_t *obj4__right_text;
    lv_obj_t *obj5;
    lv_obj_t *obj5__top_bar;
    lv_obj_t *obj5__left_text;
    lv_obj_t *obj5__top_bar_center_text;
    lv_obj_t *obj5__right_text;
    lv_obj_t *obj6;
    lv_obj_t *obj6__top_bar;
    lv_obj_t *obj6__left_text;
    lv_obj_t *obj6__top_bar_center_text;
    lv_obj_t *obj6__right_text;
    lv_obj_t *obj7;
    lv_obj_t *obj7__main_content;
    lv_obj_t *obj8;
    lv_obj_t *obj8__top_bar;
    lv_obj_t *obj8__left_text;
    lv_obj_t *obj8__top_bar_center_text;
    lv_obj_t *obj8__right_text;
    lv_obj_t *obj9;
    lv_obj_t *obj9__top_bar;
    lv_obj_t *obj9__left_text;
    lv_obj_t *obj9__top_bar_center_text;
    lv_obj_t *obj9__right_text;
    lv_obj_t *obj10;
    lv_obj_t *obj10__top_bar;
    lv_obj_t *obj10__left_text;
    lv_obj_t *obj10__top_bar_center_text;
    lv_obj_t *obj10__right_text;
    lv_obj_t *main_menu_scrolling_container;
    lv_obj_t *overview_panel;
    lv_obj_t *obj11;
    lv_obj_t *devices_panel;
    lv_obj_t *obj12;
    lv_obj_t *alerts_panel;
    lv_obj_t *games_panel;
    lv_obj_t *settings_panel;
    lv_obj_t *info_panel;
    lv_obj_t *power_panel;
    lv_obj_t *obj13;
    lv_obj_t *obj14;
    lv_obj_t *obj15;
    lv_obj_t *obj16;
    lv_obj_t *obj17;
    lv_obj_t *splash_screen_tutorial;
    lv_obj_t *obj18;
    lv_obj_t *start_tutorial_button;
    lv_obj_t *obj19;
    lv_obj_t *obj20;
    lv_obj_t *obj21;
    lv_obj_t *end_tutorial_button;
    lv_obj_t *obj22;
    lv_obj_t *obj23;
    lv_obj_t *obj24;
    lv_obj_t *screen_brightness_dropdown;
    lv_obj_t *led_brightness_dropdown;
    lv_obj_t *vibrate_on_alert_switch;
    lv_obj_t *le_ds_on_alert_switch;
    lv_obj_t *wake_screen_on_alert_switch;
    lv_obj_t *obj25;
    lv_obj_t *focus_on_alert_page_switch;
    lv_obj_t *ble_scanning_switch;
    lv_obj_t *wi_fi_scanning_switch;
    lv_obj_t *scan_mode_dropdown;
    lv_obj_t *debug_over_serial_switch;
    lv_obj_t *debug_level_dropdown;
    lv_obj_t *sleep_with_serial_switch;
    lv_obj_t *sleep_with_usb_switch;
    lv_obj_t *debug_screen_label;
    lv_obj_t *debug_screen_button;
    lv_obj_t *sleep_timer_label;
    lv_obj_t *sleep_button;
    lv_obj_t *reboot_button;
    lv_obj_t *shipping_mode_button;
    lv_obj_t *restart_tutorial_button;
    lv_obj_t *obj26;
    lv_obj_t *obj27;
    lv_obj_t *obj28;
    lv_obj_t *obj29;
    lv_obj_t *obj30;
    lv_obj_t *device_info;
    lv_obj_t *version_info;
    lv_obj_t *hardware_specs_container;
    lv_obj_t *software_specs_container;
    lv_obj_t *credits_container;
    lv_obj_t *obj31;
    lv_obj_t *alert_container;
    lv_obj_t *no_alerts_label;
    lv_obj_t *devices_container;
    lv_obj_t *obj32;
    lv_obj_t *system___health_container;
    lv_obj_t *power_container;
    lv_obj_t *scanning_container;
    lv_obj_t *rules___alerts_container;
    lv_obj_t *helga;
    lv_obj_t *sleep_now_button;
    lv_obj_t *restart_button;
    lv_obj_t *power_off_;
    lv_obj_t *obj33;
    lv_obj_t *obj34;
    lv_obj_t *obj35;
    lv_obj_t *obj36;
    lv_obj_t *obj37;
    lv_obj_t *obj38;
    lv_obj_t *obj39;
    lv_obj_t *obj40;
    lv_obj_t *obj41;
    lv_obj_t *obj42;
    lv_obj_t *obj43;
    lv_obj_t *obj44;
    lv_obj_t *obj45;
    lv_obj_t *obj46;
    lv_obj_t *obj47;
    lv_obj_t *obj48;
} objects_t;

extern objects_t objects;

void create_screen_main_menu();
void tick_screen_main_menu();

void create_screen_tutorial_splash_screen();
void tick_screen_tutorial_splash_screen();

void create_screen_tutorial();
void tick_screen_tutorial();

void create_screen_settings();
void tick_screen_settings();

void create_screen_info();
void tick_screen_info();

void create_screen_screen_template();
void tick_screen_screen_template();

void create_screen_alerts();
void tick_screen_alerts();

void create_screen_devices();
void tick_screen_devices();

void create_screen_device_updating();
void tick_screen_device_updating();

void create_screen_debug_info();
void tick_screen_debug_info();

void create_screen_overview();
void tick_screen_overview();

void create_screen_power_menu();
void tick_screen_power_menu();

void create_user_widget_top_bar(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_top_bar(void *flowState, int startWidgetIndex);

void create_user_widget_main_content(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_main_content(void *flowState, int startWidgetIndex);

void create_user_widget_alert(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_alert(void *flowState, int startWidgetIndex);

void create_user_widget_device(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_device(void *flowState, int startWidgetIndex);

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

// Groups

typedef struct _groups_t {
    lv_group_t *UINavigation;
} groups_t;

extern groups_t groups;

void ui_create_groups();

// Color themes

enum Themes {
    THEME_ID_DEFAULT,
    THEME_ID_PINK,
    THEME_ID_HATSUNE_MIKU,
};
enum Colors {
    COLOR_ID_FOCUS_BORDER,
    COLOR_ID_SWITCH_BACKGROUND,
    COLOR_ID_SUBTEXT,
};
void change_color_theme(uint32_t themeIndex);
extern uint32_t theme_colors[3][3];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/