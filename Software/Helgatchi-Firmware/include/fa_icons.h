#pragma once
// ============================================================================
// FontAwesome 4.7 codepoint macros (LVGL-friendly, UTF-8-encoded).
//
// Usage:
//   #include "fa_icons.h"
//   lv_label_set_text(my_label, FA_GEARS);
//
// Each macro expands to a UTF-8 string literal containing one FA glyph in
// the FontAwesome private-use range (U+F000..U+F8FF). To use the macro on
// a label, the text font applied to that label MUST contain the codepoint.
//
// IMPORTANT — what's in the stock font vs. what isn't:
//   The default LVGL Montserrat fonts (lv_font_montserrat_*) contain ONLY
//   the ~50 FA glyphs that have LV_SYMBOL_* definitions in lv_symbol_def.h.
//   Any macro below that's marked /* LVGL */ is in stock Montserrat and
//   works out of the box. Any macro marked /* custom */ requires you to
//   regenerate the font with that codepoint included (see end of file).
//
// To add a new icon from the FA cheatsheet:
//   1. Look up the codepoint (e.g. fa-cogs = U+F085).
//   2. Encode it as UTF-8. For all FA codepoints (U+F000..U+F8FF):
//        byte1 = 0xEF
//        byte2 = 0x80 | ((cp >> 6) & 0x3F)
//        byte3 = 0x80 | (cp & 0x3F)
//      e.g. U+F085 → 0xEF, 0x80 | (0xF085 >> 6) & 0x3F = 0x82,
//                          0x80 | (0xF085 & 0x3F) = 0x85
//                  → "\xEF\x82\x85"
//   3. Add a #define here and a one-line comment with the codepoint.
//   4. If the codepoint is NOT in the LVGL-stock list, regenerate the font
//      to include it (or your label will show tofu / nothing).
// ============================================================================

// ---- Stock Montserrat (already in lv_font_montserrat_*; these all have
//      equivalent LV_SYMBOL_* macros — listed for cheatsheet-driven lookup) ----

#define FA_BARS                  "\xEF\x83\x89"  // U+F0C9   /* LVGL — LV_SYMBOL_BARS    */
#define FA_BOLT                  "\xEF\x83\xA7"  // U+F0E7   /* LVGL — LV_SYMBOL_CHARGE  */
#define FA_COG                   "\xEF\x80\x93"  // U+F013   /* LVGL — LV_SYMBOL_SETTINGS (single gear) */
#define FA_GEAR                  FA_COG          //          /* LVGL — alias for FA_COG  */
#define FA_HOME                  "\xEF\x80\x95"  // U+F015   /* LVGL — LV_SYMBOL_HOME    */
#define FA_KEYBOARD              "\xEF\x84\x9C"  // U+F11C   /* LVGL — LV_SYMBOL_KEYBOARD */
#define FA_LIST                  "\xEF\x80\xBA"  // U+F03A   /* LVGL — LV_SYMBOL_LIST    */
#define FA_REFRESH               "\xEF\x80\xA1"  // U+F021   /* LVGL — LV_SYMBOL_REFRESH */
#define FA_USB                   "\xEF\x8A\x87"  // U+F287   /* LVGL — LV_SYMBOL_USB     */
#define FA_WARNING               "\xEF\x81\xB1"  // U+F071   /* LVGL — LV_SYMBOL_WARNING */
#define FA_WIFI                  "\xEF\x87\xAB"  // U+F1EB   /* LVGL — LV_SYMBOL_WIFI    */
#define FA_BLUETOOTH             "\xEF\x8A\x93"  // U+F293   /* LVGL — LV_SYMBOL_BLUETOOTH */

// ---- Not in stock Montserrat — need a custom-generated font (see below) ----

#define FA_BAR_CHART             "\xEF\x82\x80"  // U+F080   /* custom */
#define FA_BELL                  "\xEF\x83\xB3"  // U+F0F3   /* custom */
#define FA_BELL_SLASH            "\xEF\x87\xB6"  // U+F1F6   /* custom */
#define FA_BLUETOOTH_B           "\xEF\x8A\x94"  // U+F294   /* custom — bold-rounded variant */
#define FA_COGS                  "\xEF\x82\x85"  // U+F085   /* custom — multi-gear */
#define FA_GEARS                 FA_COGS         //          /* custom — alias */
#define FA_EXCLAMATION_CIRCLE    "\xEF\x81\xAA"  // U+F06A   /* custom */
#define FA_EXCLAMATION_TRIANGLE  FA_WARNING      //          /* LVGL — alias for FA_WARNING */
#define FA_HEARTBEAT             "\xEF\x88\x9E"  // U+F21E   /* custom */
#define FA_INFO                  "\xEF\x84\xA9"  // U+F129   /* custom */
#define FA_INFO_CIRCLE           "\xEF\x81\x9A"  // U+F05A   /* custom */
#define FA_LINE_CHART            "\xEF\x88\x81"  // U+F201   /* custom */
#define FA_LOCK                  "\xEF\x80\xA3"  // U+F023   /* custom */
#define FA_MICROCHIP             "\xEF\x8B\x9B"  // U+F2DB   /* custom */
#define FA_PLUG                  "\xEF\x87\xA6"  // U+F1E6   /* custom */
#define FA_POWER_OFF             "\xEF\x80\x91"  // U+F011   /* custom */
#define FA_QUESTION_CIRCLE       "\xEF\x81\x99"  // U+F059   /* custom */
#define FA_SEARCH                "\xEF\x80\x82"  // U+F002   /* custom */
#define FA_SIGNAL                "\xEF\x80\x92"  // U+F012   /* custom */
#define FA_SLIDERS               "\xEF\x87\x9E"  // U+F1DE   /* custom */
#define FA_STAR                  "\xEF\x80\x85"  // U+F005   /* custom */
#define FA_UNLOCK                "\xEF\x82\x9C"  // U+F09C   /* custom */

// ============================================================================
// Generating a font that contains the /* custom */ glyphs above
// ============================================================================
//
// Tool: lv_font_conv (npm install -g lv_font_conv)
// Source: a TTF file containing the FA codepoints. The simplest source is
// FontAwesome 4.7's `fontawesome-webfont.ttf` (still freely available).
//
// Example — regenerate Montserrat 36 with the FA range tacked on, output a
// drop-in replacement for `lv_font_montserrat_36` so the menu icon font
// keeps working without further code changes:
//
//   lv_font_conv \
//     --font Montserrat-Medium.ttf -r 0x20-0x7F \
//     --font fontawesome-webfont.ttf -r 0xF000-0xF2FF \
//     --bpp 4 --size 36 --no-compress \
//     --format lvgl --lv-include "lvgl.h" \
//     --output assets/fonts/montserrat_36_fa.c
//
// Or for an icon-only font (cleaner separation, smaller per-icon set):
//
//   lv_font_conv \
//     --font fontawesome-webfont.ttf \
//       -r 0xF002,0xF005,0xF011,0xF012,0xF023,0xF059,0xF05A,0xF06A,\
//          0xF080,0xF085,0xF09C,0xF129,0xF1DE,0xF1E6,0xF1F6,0xF201,\
//          0xF21E,0xF294,0xF2DB,0xF0F3 \
//     --bpp 4 --size 36 --no-compress \
//     --format lvgl --lv-include "lvgl.h" \
//     --output assets/fonts/fa_icons_36.c
//
// Then in the firmware:
//   1. Drop the generated .c into src/ (or assets/ if your build picks it up).
//   2. `extern const lv_font_t fa_icons_36;` near the top of ui_controller.cpp.
//   3. `lv_obj_set_style_text_font(label, &fa_icons_36, 0);` on icon labels.
//
// Web alternative (no CLI install): https://lvgl.io/tools/fontconverter
// ============================================================================
