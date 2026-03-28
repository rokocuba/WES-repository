SET(SOURCES components/ui_comp_temp.c
    components/ui_comp.c
    screens/ui_Home_Scr.c
    screens/ui_Color_Scr.c
    screens/ui_Settings_Scr.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    images/ui_img_colorwheel_icon_png.c
    images/ui_img_bl_logo_symbol_png.c
    images/ui_img_settings_icon_png.c
    images/ui_img_home_icon_png.c)

add_library(ui ${SOURCES})
