SET(SOURCES ui_comp_top_bar_center_text.c
    ui_comp_top_bar_right_text.c
    ui_comp_top_bar_left_text.c
    ui_comp.c
    ui_screen_splash_screen.c
    ui_screen_status_screen.c
    ui_screen_settings_screen.c
    ui_screen_menu_screen.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_633823932.c)

add_library(ui ${SOURCES})
