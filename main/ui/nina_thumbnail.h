#pragma once

/**
 * @file nina_thumbnail.h
 * @brief Thumbnail overlay for the NINA dashboard.
 */

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/** Create the fullscreen thumbnail overlay (initially hidden). */
void nina_thumbnail_create(lv_obj_t *parent);

/** Show the overlay and set the thumbnail_requested flag. Called from header click. */
void nina_thumbnail_request(void);

/** Check if a thumbnail image has been requested. */
bool nina_dashboard_thumbnail_requested(void);

/** Clear the thumbnail request flag. */
void nina_dashboard_clear_thumbnail_request(void);

/** True when nina_dashboard_set_thumbnail() would KEEP the buffer (overlay
 *  built and visible). False means it would free it on arrival, so the caller
 *  can hand that buffer to a different owner instead of copying it.
 *  Call under the LVGL display lock. */
bool nina_dashboard_thumbnail_wants_data(void);

/** Set decoded thumbnail image data for display. Ownership of rgb565_data is transferred. */
void nina_dashboard_set_thumbnail(const uint8_t *rgb565_data, uint32_t w, uint32_t h, uint32_t data_size);

/** Hide the thumbnail overlay and free image memory. */
void nina_dashboard_hide_thumbnail(void);

/** Check if thumbnail overlay is currently visible. */
bool nina_dashboard_thumbnail_visible(void);

/** Apply current theme to thumbnail overlay (back button, labels). */
void nina_thumbnail_apply_theme(void);
