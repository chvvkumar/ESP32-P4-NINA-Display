#pragma once

#include "lvgl.h"
#include "goes_client.h"
#include <stdbool.h>

lv_obj_t *nina_image_display_create(lv_obj_t *parent);
void      nina_image_display_update(goes_data_t *data);
/* Display a small RGB565 render (w x h) scaled up to fill the panel, swapping
 * instantly (no crossfade). Used by the moon drag loop: it COPIES `buf` into an
 * owned, reused buffer (so the next render-task frame can overwrite the source
 * without tearing) and software-scales the copy. The caller still owns `buf` and
 * may overwrite it after this returns. Call under the display lock held by the
 * caller. No-op if a crossfade is in flight. */
void      nina_image_display_show_scaled(const uint16_t *buf, int w, int h);
/* Display a full-panel (w x h, normally 720x720) RGB565 buffer the CALLER owns,
 * pointing the LVGL descriptor straight at it at scale 1.0 with NO copy and an
 * instant swap. Used by the moon drag loop after a PPA hardware upscale: the
 * caller ping-pongs two 720 output buffers so the one handed in here is never the
 * one LVGL is still flushing. The slot is flagged "borrowed" so this module does
 * NOT free `buf` (the caller frees it on page leave). The caller must keep `buf`
 * valid until the next show_borrowed/show_scaled/update swaps it out. Call under
 * the display lock held by the caller. No-op if a crossfade is in flight. */
void      nina_image_display_show_borrowed(const uint16_t *buf, int w, int h);
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
