#pragma once

#include "lvgl.h"
#include <stdbool.h>

/**
 * Generic full-screen "wait/loading" overlay, styled to mirror the OTA
 * progress screen (glow + main progress bar, large percentage label).
 *
 * All functions must be called from a display-locked context (the caller
 * holds bsp_display_lock); this module does NOT acquire the lock itself,
 * matching the nina_ota_prompt convention.
 */

/* Create the overlay once (hidden) under @p parent at dashboard init. */
void nina_wait_overlay_create(lv_obj_t *parent);

/* Show the overlay with the given title and optional subtitle (may be NULL
 * or empty). Starts an indeterminate pulse animation. */
void nina_wait_overlay_show(const char *title, const char *subtitle);

/* Set progress. percent >= 0 -> determinate bar + "NN%" label.
 * percent < 0 -> indeterminate pulse animation (percentage label hidden). */
void nina_wait_overlay_set_progress(int percent);

/* Hide the overlay and stop any running animation. */
void nina_wait_overlay_hide(void);

/* True when the overlay is currently visible. */
bool nina_wait_overlay_visible(void);

/* Recolor the overlay from the active theme (mirrors nina_ota_prompt_apply_theme). */
void nina_wait_overlay_apply_theme(void);
