#pragma once

/**
 * @file nina_octoprint.h
 * @brief OctoPrint 3D Printer page — live print status from an OctoPrint host.
 *
 * Mirrors the JSON Display / AllSky page contract: the page is created once
 * (hidden), fed by the shared octoprint_data_t (populated by octoprint_poll_task
 * via octoprint_client_poll), and re-themed / rebuilt on config change. All
 * functions run with the LVGL display lock held by the CALLER (matches
 * nina_json.c / nina_allsky.c); this module never takes the display lock itself.
 */

#include "lvgl.h"
#include "octoprint_client.h"

/**
 * @brief Build the OctoPrint page (full-screen, no header band).
 *
 * Renders in the layout selected by app_config_t::octoprint_layout
 * (0=bento 1=retired (renders Bento; reserved) 2=glass 3=typographic 4=timeline).
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
 * Caller holds data->mutex AND the display lock. Mirrors json_page_update.
 */
void octoprint_page_update(const octoprint_data_t *data);

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

/** Current page root object, or NULL if the page is not built. */
lv_obj_t *octoprint_page_get_obj(void);
