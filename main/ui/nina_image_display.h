#pragma once

#include "lvgl.h"
#include "goes_client.h"
#include "app_config.h"
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

/**
 * @brief Apply Image Display config changes live (preview), comparing @p prev to @p cur.
 *
 * Performs the same live-apply the Image Display config POST handler does after a
 * save: pushes the enable/overlay state to the dashboard, then branches on what
 * changed between @p prev and @p cur:
 *   - source / solar_band / goes_region changed (or a custom-URL change while the
 *     active source is Custom, or @p force_fetch with the Custom source): flag the
 *     next fetch as manual (wait overlay) and wake goes_poll_task to re-download.
 *   - Moon-only parameters changed (source == Moon): wake goes_poll_task so its
 *     local Moon branch re-renders (no manual-fetch flag, no overlay).
 *   - crop / orientation only: re-render the already-decoded frame locally under
 *     the display lock (no HTTP re-download).
 * All re-fetch/re-render work is gated on cur->image_display_enabled.
 *
 * @param prev        Full config snapshot taken BEFORE the field writes.
 * @param cur         Saved config snapshot (post-write).
 * @param force_fetch Force a re-fetch for the Custom source even if no field changed.
 */
void image_display_apply_live(const app_config_t *prev, const app_config_t *cur, bool force_fetch);
