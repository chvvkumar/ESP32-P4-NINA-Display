#pragma once

#include "lvgl.h"

void nina_setup_screen_create(lv_obj_t *parent);
void nina_setup_screen_destroy(void);
bool is_setup_mode(void);
void set_setup_mode(bool enabled);
