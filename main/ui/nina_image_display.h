#pragma once

#include "lvgl.h"
#include "goes_client.h"
#include <stdbool.h>

lv_obj_t *nina_image_display_create(lv_obj_t *parent);
void      nina_image_display_update(goes_data_t *data);
/* Force the next nina_image_display_update() to re-render the already-cached
 * frame (e.g. re-apply a crop change locally) without an HTTP re-download. Call
 * it immediately before nina_image_display_update(&goes_data), both under the
 * display lock. No-op if no image is cached. */
void      nina_image_display_force_redraw(void);
/* True if a frame is currently displayed on the image page (false right after
 * page leave / cleanup, which frees the buffers). Call under the display lock. */
bool      nina_image_display_has_image(void);
void      nina_image_display_cleanup(void);
void      nina_image_display_set_overlay_visible(bool visible);
void      nina_image_display_apply_theme(void);
