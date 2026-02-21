#pragma once

/**
 * @file nina_info_overlay.h
 * @brief Public API for the shared info overlay module.
 *
 * Provides fullscreen detail overlays for camera, mount, image stats,
 * sequence, filter wheel, and autofocus data.  A single overlay container
 * is reused — show() dispatches to the appropriate content builder.
 */

#include "lvgl.h"
#include <stdbool.h>
#include "info_overlay_types.h"

/** Create the info overlay (initially hidden). Called once during dashboard init. */
void nina_info_overlay_create(lv_obj_t *parent);

/**
 * @brief Show overlay with a specific type.
 * @param type         Which detail overlay to display
 * @param page_index   The dashboard page index (for returning via back button)
 */
void nina_info_overlay_show(info_overlay_type_t type, int page_index);

/** Hide the overlay. */
void nina_info_overlay_hide(void);

/** Check visibility. */
bool nina_info_overlay_visible(void);

/** Check if an overlay data fetch has been requested. */
bool nina_info_overlay_requested(void);

/** Clear the request flag (call after starting the fetch). */
void nina_info_overlay_clear_request(void);

/** Get the currently requested overlay type. */
info_overlay_type_t nina_info_overlay_get_type(void);

/** Get the return page index (the page that opened the overlay). */
int nina_info_overlay_get_return_page(void);

/** Data population — call under display lock. */
void nina_info_overlay_set_camera_data(const camera_detail_data_t *data);
void nina_info_overlay_set_mount_data(const mount_detail_data_t *data);
void nina_info_overlay_set_imagestats_data(const imagestats_detail_data_t *data);
void nina_info_overlay_set_sequence_data(const sequence_detail_data_t *data);
void nina_info_overlay_set_filter_data(const filter_detail_data_t *data);
void nina_info_overlay_set_autofocus_data(const autofocus_data_t *data);

/** Apply current theme to the info overlay. */
void nina_info_overlay_apply_theme(void);
