SET(SOURCES screens/ui_Screen1.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_483745875.c
    images/ui_img_3_png.c
    images/ui_img_2_png.c
    images/ui_img_1_png.c)

add_library(ui ${SOURCES})
