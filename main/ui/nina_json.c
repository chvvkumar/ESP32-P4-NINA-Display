/**
 * @file nina_json.c
 * @brief JSON Display page — thin adapter over the shared nina_tile_grid renderer.
 *
 * The row-based tile grid (layout, parse, build, render, threshold/color logic,
 * empty-state overlay) now lives in nina_tile_grid.c and is source-agnostic.
 * This file keeps the page's public API (nina_json.h) and owns only the
 * JSON-specific overlay-state decisions (reads cfg->json_url + data->connected)
 * and the tile-grid handle. json_client.c (the JSON path resolver) is unchanged.
 *
 * All LVGL calls run under the display lock held by the caller; this module
 * never takes the lock itself.
 */

#include "nina_json.h"
#include "nina_tile_grid.h"
#include "nina_empty_state.h"   /* ICON_CLOUD_OFF */
#include "app_config.h"

static nina_tile_grid_t *s_grid = NULL;
static lv_obj_t         *s_json_root = NULL;

/* ── Page creation ────────────────────────────────────────────────────── */

lv_obj_t *json_page_create(lv_obj_t *parent) {
    s_grid = nina_tile_grid_create(parent, app_config_get_json_tiles(),
                                   ICON_CLOUD_OFF, "No JSON Source",
                                   "Check the JSON Display settings.");
    s_json_root = nina_tile_grid_get_root(s_grid);
    return s_json_root;
}

/* ── Update ───────────────────────────────────────────────────────────── */

void json_page_update(const json_data_t *data) {
    if (!s_grid || !data) {
        return;
    }

    const app_config_t *cfg = app_config_get();

    /* Overlay states (mirror mockup applyDeviceState). Page-specific: reads
     * cfg->json_url and data->connected, so it stays in the adapter. */
    if (cfg->json_url[0] == '\0') {
        nina_tile_grid_show_overlay(s_grid, "No JSON Source");
        return;
    }
    if (!data->connected) {
        nina_tile_grid_show_overlay(s_grid, "Cannot Reach Source");
        return;
    }
    if (data->tile_count == 0) {
        nina_tile_grid_show_overlay(s_grid, "No Tiles Configured");
        return;
    }
    nina_tile_grid_hide_overlay(s_grid);

    nina_tile_grid_update(s_grid, data->values, data->resolved, data->tile_count);
}

/* ── Theme application ────────────────────────────────────────────────── */

void json_page_apply_theme(void) {
    nina_tile_grid_apply_theme(s_grid);
}

/* ── Refresh config (rebuild the row/tile tree) ───────────────────────── */

void json_page_refresh_config(void) {
    nina_tile_grid_refresh_config(s_grid, app_config_get_json_tiles());
}
