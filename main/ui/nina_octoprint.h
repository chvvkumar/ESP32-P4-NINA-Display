#pragma once

/**
 * @file nina_octoprint.h
 * @brief OctoPrint 3D Printer page — live print status from an OctoPrint host.
 *
 * Mirrors the JSON Display / AllSky page contract: the page is created once
 * (hidden), fed by the shared octoprint_data_t (populated by octoprint_poll_task
 * via octoprint_client_poll), and re-themed / rebuilt on config change. Every
 * function runs with the LVGL display lock held by the CALLER, with ONE
 * exception: octoprint_page_update() takes the display lock itself (see its
 * doc) so the bilinear image resample can run outside it.
 */

#include "lvgl.h"
#include "octoprint_client.h"

/**
 * @brief Build the OctoPrint page (full-screen, no header band).
 *
 * Renders in the layout selected by app_config_t::octoprint_layout
 * (0=bento 2=glass; 1, 3 and 4 are retired, render Bento and stay reserved).
 *
 * @param parent Parent LVGL container (main_cont)
 * @return The page root object (caller hides it initially). Mirrors json_page_create.
 */
lv_obj_t *octoprint_page_create(lv_obj_t *parent);

/**
 * @brief Update all widgets from the latest polled printer state.
 *
 * Reads job progress, temperatures, print/state text and the image slot
 * (G-code preview or webcam snapshot per app_config_t::octoprint_image_source).
 * Missing / unresolved values render "--" in the theme text color.
 *
 * Locking: caller holds data->mutex ONLY -- unlike the other page updates,
 * this function takes the display lock itself (client lock outside, display
 * lock inside), so the heavy bilinear image scale runs before it is acquired.
 * Do NOT call with the display lock already held.
 *
 * Mutates @p data under the caller's lock: clears new_image once the scaled
 * frame is bound, and for the webcam source consumes (frees and NULLs)
 * image_buf after scaling -- see octoprint_client.h.
 */
void octoprint_page_update(octoprint_data_t *data);

/**
 * @brief Release the page's copy of the decoded frame (up to 2 MB of PSRAM).
 *
 * Call from the page-leave arm, under the display lock, after the client has
 * been told the page is inactive. The UI keeps its own copy of the frame (see
 * the file header in nina_octoprint.c), so freeing the client's buffer alone
 * leaves this one held. Mirrors nina_spotify_free_art(). Safe to call twice.
 */
void octoprint_page_free_image(void);

/**
 * @brief Re-apply the current theme to the page.
 *
 * Rebuilds the widget tree in place: every layout resolves its decoration
 * colours at build time, so re-styling alone would leave them on the old
 * palette. No-op until the page has been created. Mirrors json_page_apply_theme.
 */
void octoprint_page_apply_theme(void);

/**
 * @brief Re-read the OctoPrint config and rebuild the widget tree.
 *
 * Call after octoprint_layout or octoprint_image_source changes (from the config
 * POST handler, under the display lock). Mirrors json_page_refresh_config.
 */
void octoprint_page_refresh_config(void);
