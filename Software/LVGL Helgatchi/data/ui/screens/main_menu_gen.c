/**
 * @file main_menu_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "main_menu_gen.h"
#include "helgatchi_ui.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/***********************
 *  STATIC VARIABLES
 **********************/

/***********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * main_menu_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");


    static bool style_inited = false;

    if (!style_inited) {

        style_inited = true;
    }

    lv_obj_t * lv_obj_0 = lv_obj_create(NULL);
    lv_obj_set_name_static(lv_obj_0, "main_menu_#");

    lv_obj_t * root = lv_obj_create(lv_obj_0);
    lv_obj_set_name(root, "root");
    lv_obj_set_width(root, lv_pct(100));
    lv_obj_set_height(root, lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, 0, 0);
    lv_obj_t * statusbar = lv_obj_create(root);
    lv_obj_set_name(statusbar, "statusbar");
    lv_obj_set_width(statusbar, lv_pct(100));
    lv_obj_set_height(statusbar, 24);
    lv_obj_set_flex_flow(statusbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(statusbar, 4, 0);
    lv_obj_set_style_pad_column(statusbar, 6, 0);
    lv_obj_set_style_border_width(statusbar, 1, 0);
    lv_obj_t * status_left = lv_label_create(statusbar);
    lv_obj_set_name(status_left, "status_left");
    lv_obj_set_width(status_left, lv_pct(33));
    lv_label_set_text(status_left, "Menu");
    lv_obj_set_style_text_align(status_left, LV_TEXT_ALIGN_LEFT, 0);
    
    lv_obj_t * status_center = lv_label_create(statusbar);
    lv_obj_set_name(status_center, "status_center");
    lv_obj_set_width(status_center, lv_pct(34));
    lv_label_set_text(status_center, "Main");
    lv_obj_set_style_text_align(status_center, LV_TEXT_ALIGN_CENTER, 0);
    
    lv_obj_t * status_right = lv_label_create(statusbar);
    lv_obj_set_name(status_right, "status_right");
    lv_obj_set_width(status_right, lv_pct(33));
    lv_label_set_text(status_right, "");
    lv_obj_set_style_text_align(status_right, LV_TEXT_ALIGN_RIGHT, 0);
    
    lv_obj_t * content = lv_obj_create(root);
    lv_obj_set_name(content, "content");
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 12, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_t * title = lv_label_create(content);
    lv_obj_set_name(title, "title");
    lv_label_set_text(title, "Main Menu");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    
    lv_obj_t * card_row = lv_obj_create(content);
    lv_obj_set_name(card_row, "card_row");
    lv_obj_set_width(card_row, lv_pct(92));
    lv_obj_set_height(card_row, lv_pct(78));
    lv_obj_set_flex_flow(card_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flag(card_row, LV_OBJ_FLAG_SCROLLABLE, true);
    lv_obj_set_scroll_snap_x(card_row, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(card_row, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flag(card_row, LV_OBJ_FLAG_SCROLL_ONE, true);
    lv_obj_set_style_pad_all(card_row, 0, 0);
    lv_obj_set_style_pad_column(card_row, 12, 0);
    lv_obj_set_style_radius(card_row, 8, 0);
    lv_obj_set_style_border_width(card_row, 2, 0);
    lv_obj_t * card_status = lv_obj_create(card_row);
    lv_obj_set_name(card_status, "card_status");
    lv_obj_set_width(card_status, 150);
    lv_obj_set_height(card_status, lv_pct(100));
    lv_obj_set_flex_flow(card_status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card_status, 10, 0);
    lv_obj_set_style_pad_row(card_status, 6, 0);
    lv_obj_t * lv_label_0 = lv_label_create(card_status);
    lv_label_set_text(lv_label_0, "Status");
    
    lv_obj_t * lv_label_1 = lv_label_create(card_status);
    lv_label_set_text(lv_label_1, "Battery: 78%");
    
    lv_obj_t * lv_label_2 = lv_label_create(card_status);
    lv_label_set_text(lv_label_2, "Uptime: 01:23");
    
    lv_obj_t * lv_label_3 = lv_label_create(card_status);
    lv_label_set_text(lv_label_3, "Devices: 12");
    
    lv_obj_t * card_alerts = lv_obj_create(card_row);
    lv_obj_set_name(card_alerts, "card_alerts");
    lv_obj_set_width(card_alerts, 150);
    lv_obj_set_height(card_alerts, lv_pct(100));
    lv_obj_set_flex_flow(card_alerts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card_alerts, 10, 0);
    lv_obj_set_style_pad_row(card_alerts, 6, 0);
    lv_obj_t * lv_label_4 = lv_label_create(card_alerts);
    lv_label_set_text(lv_label_4, "Alerts");
    
    lv_obj_t * lv_label_5 = lv_label_create(card_alerts);
    lv_label_set_text(lv_label_5, "Active: 0");
    
    lv_obj_t * lv_label_6 = lv_label_create(card_alerts);
    lv_label_set_text(lv_label_6, "Last: none");
    
    lv_obj_t * lv_label_7 = lv_label_create(card_alerts);
    lv_label_set_text(lv_label_7, "Rules: 5");
    
    lv_obj_t * card_devices = lv_obj_create(card_row);
    lv_obj_set_name(card_devices, "card_devices");
    lv_obj_set_width(card_devices, 150);
    lv_obj_set_height(card_devices, lv_pct(100));
    lv_obj_set_flex_flow(card_devices, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card_devices, 10, 0);
    lv_obj_set_style_pad_row(card_devices, 6, 0);
    lv_obj_t * lv_label_8 = lv_label_create(card_devices);
    lv_label_set_text(lv_label_8, "Devices");
    
    lv_obj_t * lv_label_9 = lv_label_create(card_devices);
    lv_label_set_text(lv_label_9, "Nearby: 3");
    
    lv_obj_t * lv_label_10 = lv_label_create(card_devices);
    lv_label_set_text(lv_label_10, "Tracked: 12");
    
    lv_obj_t * lv_label_11 = lv_label_create(card_devices);
    lv_label_set_text(lv_label_11, "New: 1");
    
    lv_obj_t * card_settings = lv_obj_create(card_row);
    lv_obj_set_name(card_settings, "card_settings");
    lv_obj_set_width(card_settings, 150);
    lv_obj_set_height(card_settings, lv_pct(100));
    lv_obj_set_flex_flow(card_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card_settings, 10, 0);
    lv_obj_set_style_pad_row(card_settings, 6, 0);
    lv_obj_t * lv_label_12 = lv_label_create(card_settings);
    lv_label_set_text(lv_label_12, "Settings");
    
    lv_obj_t * lv_label_13 = lv_label_create(card_settings);
    lv_label_set_text(lv_label_13, "Brightness");
    
    lv_obj_t * lv_label_14 = lv_label_create(card_settings);
    lv_label_set_text(lv_label_14, "Buttons");
    
    lv_obj_t * lv_label_15 = lv_label_create(card_settings);
    lv_label_set_text(lv_label_15, "Sleep");
    
    lv_obj_t * card_about = lv_obj_create(card_row);
    lv_obj_set_name(card_about, "card_about");
    lv_obj_set_width(card_about, 150);
    lv_obj_set_height(card_about, lv_pct(100));
    lv_obj_set_flex_flow(card_about, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card_about, 10, 0);
    lv_obj_set_style_pad_row(card_about, 6, 0);
    lv_obj_t * lv_label_16 = lv_label_create(card_about);
    lv_label_set_text(lv_label_16, "About");
    
    lv_obj_t * lv_label_17 = lv_label_create(card_about);
    lv_label_set_text(lv_label_17, "Helgatchi");
    
    lv_obj_t * lv_label_18 = lv_label_create(card_about);
    lv_label_set_text(lv_label_18, "v0.1");
    
    lv_obj_t * lv_label_19 = lv_label_create(card_about);
    lv_label_set_text(lv_label_19, "LVGL");
    
    lv_obj_t * hotbar = lv_obj_create(root);
    lv_obj_set_name(hotbar, "hotbar");
    lv_obj_set_width(hotbar, lv_pct(100));
    lv_obj_set_height(hotbar, 24);
    lv_obj_set_flex_flow(hotbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(hotbar, 4, 0);
    lv_obj_set_style_pad_column(hotbar, 6, 0);
    lv_obj_set_style_border_width(hotbar, 1, 0);
    lv_obj_t * hint_left = lv_label_create(hotbar);
    lv_obj_set_name(hint_left, "hint_left");
    lv_obj_set_width(hint_left, lv_pct(33));
    lv_label_set_text(hint_left, "L = Prev");
    lv_obj_set_style_text_align(hint_left, LV_TEXT_ALIGN_LEFT, 0);
    
    lv_obj_t * hint_center = lv_label_create(hotbar);
    lv_obj_set_name(hint_center, "hint_center");
    lv_obj_set_width(hint_center, lv_pct(34));
    lv_label_set_text(hint_center, "C = Select");
    lv_obj_set_style_text_align(hint_center, LV_TEXT_ALIGN_CENTER, 0);
    
    lv_obj_t * hint_right = lv_label_create(hotbar);
    lv_obj_set_name(hint_right, "hint_right");
    lv_obj_set_width(hint_right, lv_pct(33));
    lv_label_set_text(hint_right, "R = Next");
    lv_obj_set_style_text_align(hint_right, LV_TEXT_ALIGN_RIGHT, 0);

    LV_TRACE_OBJ_CREATE("finished");

    return lv_obj_0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

