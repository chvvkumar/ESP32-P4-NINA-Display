#pragma once

#include "lvgl.h"
#include "goes_client.h"
#include <stdbool.h>

lv_obj_t *nina_image_display_create(lv_obj_t *parent);
void      nina_image_display_update(goes_data_t *data);
void      nina_image_display_cleanup(void);
void      nina_image_display_set_overlay_visible(bool visible);
void      nina_image_display_apply_theme(void);
