#pragma once

#include "lvgl.h"

/* Build the UI on the given display. Must run in the LVGL task. */
void ui_init(lv_display_t *disp);
