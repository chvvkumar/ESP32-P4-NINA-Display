#pragma once

/**
 * @file nina_json.h
 * @brief JSON Display page — row-based tile grid driven by any JSON API.
 *
 * Mirrors the AllSky page contract: the page is created once (hidden), fed by
 * the shared json_data_t (populated by json_poll_task via json_client_poll),
 * and re-themed / rebuilt on config change. All functions run with the LVGL
 * display lock held by the CALLER (matches nina_allsky.c); this module never
 * takes the display lock itself.
 */

#include "lvgl.h"
#include "json_client.h"

/**
 * @brief Build the JSON Display page (row-based tile grid, full-screen, no header band).
 * @param parent Parent LVGL container (main_cont)
 * @return The page root object (caller hides it initially). Mirrors allsky_page_create.
 */
lv_obj_t *json_page_create(lv_obj_t *parent);

/**
 * @brief Update all tile widgets from the latest resolved data.
 *
 * Reads json_tiles_config (parsed at create/refresh time) to know each tile's
 * type / label / unit / threshold / value-map, and colors the value:
 *   - Number: coerce with strtof, format %.*f, color by low/high thresholds.
 *   - Text: case-insensitive value→color map, else theme text color.
 *   - Boolean: true/false text + color per truthiness.
 * Missing / unresolved value renders "--" in the theme text color.
 *
 * Caller holds data->mutex AND the display lock. Mirrors allsky_page_update.
 */
void json_page_update(const json_data_t *data);

/** Apply the current theme to all page widgets. Mirrors allsky_page_apply_theme. */
void json_page_apply_theme(void);

/**
 * @brief Re-read json_tiles_config and rebuild the row/tile widget tree.
 * Call after json_tiles_config changes (from the config POST handler, under the
 * display lock). Mirrors allsky_page_refresh_config.
 */
void json_page_refresh_config(void);
