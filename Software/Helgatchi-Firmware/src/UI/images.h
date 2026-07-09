#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_dc801_logo;
extern const lv_img_dsc_t img_helga_sit_scoot1;
extern const lv_img_dsc_t img_helga_sit_scoot2;
extern const lv_img_dsc_t img_helga_walk1;
extern const lv_img_dsc_t img_helga_walk2;
extern const lv_img_dsc_t img_helga_walk3;
extern const lv_img_dsc_t img_helga_walk4;
extern const lv_img_dsc_t img_helga_dance_party1;
extern const lv_img_dsc_t img_helga_dance_party2;
extern const lv_img_dsc_t img_helga_dance_party3;
extern const lv_img_dsc_t img_helga_dance_party4;
extern const lv_img_dsc_t img_helga_dance_party5;
extern const lv_img_dsc_t img_helga_dance_party6;
extern const lv_img_dsc_t img_helga_dance_party7;
extern const lv_img_dsc_t img_helga_dance_party8;
extern const lv_img_dsc_t img_helga_dance1;
extern const lv_img_dsc_t img_helga_dance2;
extern const lv_img_dsc_t img_helga_dance3;
extern const lv_img_dsc_t img_helga_dance4;
extern const lv_img_dsc_t img_helga_dance5;
extern const lv_img_dsc_t img_helga_dance6;
extern const lv_img_dsc_t img_helga_dance7;
extern const lv_img_dsc_t img_helga_dance8;
extern const lv_img_dsc_t img_helga_sniff_alert1;
extern const lv_img_dsc_t img_helga_sniff_alert2;
extern const lv_img_dsc_t img_helga_sniff_alert3;
extern const lv_img_dsc_t img_helga_sniff_alert4;
extern const lv_img_dsc_t img_helga_sniff_end1;
extern const lv_img_dsc_t img_helga_sniff_end2;
extern const lv_img_dsc_t img_helga_sniff_end3;
extern const lv_img_dsc_t img_helga_sniff_loop1;
extern const lv_img_dsc_t img_helga_sniff_loop2;
extern const lv_img_dsc_t img_helga_sniff_loop3;
extern const lv_img_dsc_t img_helga_sniff_loop4;
extern const lv_img_dsc_t img_helga_sniff_start1;
extern const lv_img_dsc_t img_helga_sniff_start2;
extern const lv_img_dsc_t img_helga_brush1;
extern const lv_img_dsc_t img_helga_brush2;
extern const lv_img_dsc_t img_helga_brush3;
extern const lv_img_dsc_t img_helga_brush4;
extern const lv_img_dsc_t img_helga_brush5;
extern const lv_img_dsc_t img_helga_idle1;
extern const lv_img_dsc_t img_helga_idle2;
extern const lv_img_dsc_t img_helga_idle3;
extern const lv_img_dsc_t img_helga_idle4;
extern const lv_img_dsc_t img_helga_idle5;
extern const lv_img_dsc_t img_helga_idle6;
extern const lv_img_dsc_t img_helga_idle7;
extern const lv_img_dsc_t img_helga_idle8;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[49];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/