SET(SOURCES components/ui_comp_temp.c
    components/ui_comp.c
    screens/ui_Home_Scr.c
    screens/ui_Music_scr.c
    screens/ui_Number_scr.c
    screens/ui_Vlaga_scr.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    ui_events.c)

add_library(ui ${SOURCES})
