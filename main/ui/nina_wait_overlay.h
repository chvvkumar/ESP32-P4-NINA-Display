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
 *
 * Exception: nina_wait_overlay_cancel() may be called from within the LVGL
 * timer tick (stall_timer_cb) which already holds the LVGL tick lock.  In
 * that context do NOT call bsp_display_lock / lvgl_port_lock again —
 * nav_arbiter_submit_user() is lock-safe (atomic write + task notify).
 */

/* Create the overlay once (hidden) under @p parent at dashboard init. */
void nina_wait_overlay_create(lv_obj_t *parent);

/* Show the overlay with the given title and optional subtitle (may be NULL
 * or empty). Starts an indeterminate pulse animation and arms a 15-second
 * one-shot stall watchdog that auto-dismisses the overlay if loading stalls.
 * Caller holds the display lock. */
void nina_wait_overlay_show(const char *title, const char *subtitle);

/* Set progress. percent >= 0 -> determinate bar + "NN%" label.
 * percent < 0 -> indeterminate pulse animation (percentage label hidden). */
void nina_wait_overlay_set_progress(int percent);

/* Hide the overlay, stop any running animation, and delete the stall timer.
 * A successful image load calls this and cancels the watchdog automatically.
 * Caller holds the display lock. */
void nina_wait_overlay_hide(void);

/* True when the overlay is currently visible. */
bool nina_wait_overlay_visible(void);

/* Recolor the overlay from the active theme (mirrors nina_ota_prompt_apply_theme). */
void nina_wait_overlay_apply_theme(void);

/* Record the page to return to when the overlay is dismissed via swipe or
 * stall timeout.  Call this BEFORE show(), while current_committed still
 * holds the originating page.  Caller holds the display lock. */
void nina_wait_overlay_set_prior_page(int page);

/* Dismiss the overlay immediately (swipe cancel path) and navigate back to
 * the prior page via nav_arbiter_submit_user().  Caller holds the display
 * lock (this function calls nina_wait_overlay_hide which is lock-safe under
 * the existing lock; nav_arbiter_submit_user is always lock-safe). */
void nina_wait_overlay_cancel(void);
