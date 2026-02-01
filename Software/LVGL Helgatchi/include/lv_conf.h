#pragma once

/*
 * Minimal LVGL configuration for Arduino + ESP32-S3 + ST7789 (SPI)
 *
 * LVGL v9 checks for LV_CONF_H to confirm that this header was found.
 * Defining it here removes the "Possible failure to include lv_conf.h" note.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Color depth: 16-bit RGB565 */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Adjust the max resolution to the rotated panel size */
#define LV_HOR_RES_MAX 280
#define LV_VER_RES_MAX 240

/* Memory settings */
#define LV_MEM_CUSTOM 0

/* Enable basic widgets */
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_BAR 1

/* XML/UI designer support */
#define LV_USE_XML 1
#define LV_USE_OBSERVER 1
#define LV_USE_OBJ_NAME 1

/* Fonts */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Logging (off by default) */
#define LV_USE_LOG 0

/* Tick settings (we call lv_tick_inc manually) */
#define LV_TICK_CUSTOM 0

#endif /* LV_CONF_H */
