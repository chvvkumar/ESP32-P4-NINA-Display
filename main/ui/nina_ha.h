#pragma once

/**
 * @file nina_ha.h
 * @brief Home Assistant page — thin adapter over the shared nina_tile_grid renderer.
 *
 * Mirrors nina_json.h 1:1. The page is created once (hidden), fed by the shared
 * ha_data_t (populated by ha_poll_task via ha_client_poll), and re-themed /
 * rebuilt on config change. All functions run with the LVGL display lock held
 * by the CALLER (matches nina_json.c); this module never takes the lock itself.
 */

#include "lvgl.h"
#include "ha_client.h"

/**
 * @brief Build the Home Assistant page (row-based tile grid, full-screen).
 * @param parent Parent LVGL container (main_cont)
 * @return The page root object (caller hides it initially).
 */
lv_obj_t *ha_page_create(lv_obj_t *parent);

/**
 * @brief Update all tile widgets from the latest resolved HA data.
 * Caller holds data->mutex AND the display lock. Mirrors json_page_update.
 */
void ha_page_update(const ha_data_t *data);

/** Apply the current theme to all page widgets. Mirrors json_page_apply_theme. */
void ha_page_apply_theme(void);

/**
 * @brief Re-read ha_tiles_config and rebuild the row/tile widget tree.
 * Call after ha_tiles_config changes (from the config POST handler, under the
 * display lock). Mirrors json_page_refresh_config.
 */
void ha_page_refresh_config(void);
