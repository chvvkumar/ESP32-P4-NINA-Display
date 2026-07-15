/**
 * @file nina_ha.c
 * @brief Home Assistant page — thin adapter over the shared nina_tile_grid renderer.
 *
 * Mirrors nina_json.c 1:1. The row-based tile grid (layout, parse, build,
 * render, threshold/color logic, empty-state overlay) lives in nina_tile_grid.c
 * and is source-agnostic. This file keeps the page's public API (nina_ha.h) and
 * owns only the HA-specific overlay-state decisions (reads cfg->ha_base_url +
 * data->connected) and the tile-grid handle. ha_client.c (the entity/attr
 * resolver) is a separate module.
 *
 * All LVGL calls run under the display lock held by the caller; this module
 * never takes the lock itself.
 */

#include "nina_ha.h"
#include "nina_tile_grid.h"
#include "nina_empty_state.h"   /* ICON_CLOUD_OFF */
#include "app_config.h"

static nina_tile_grid_t *s_grid = NULL;
static lv_obj_t         *s_ha_root = NULL;

/* ── Page creation ────────────────────────────────────────────────────── */

lv_obj_t *ha_page_create(lv_obj_t *parent) {
    s_grid = nina_tile_grid_create(parent, app_config_get_ha_tiles(),
                                   ICON_CLOUD_OFF, "Not Connected",
                                   "Enter your Home Assistant address and token.");
    s_ha_root = nina_tile_grid_get_root(s_grid);
    return s_ha_root;
}

/* ── Update ───────────────────────────────────────────────────────────── */

void ha_page_update(const ha_data_t *data) {
    if (!s_grid || !data) {
        return;
    }

    const app_config_t *cfg = app_config_get();

    /* Overlay states. Page-specific: reads cfg->ha_base_url and data->connected,
     * so it stays in the adapter. */
    if (cfg->ha_base_url[0] == '\0') {
        nina_tile_grid_show_overlay(s_grid, "Not Connected");
        return;
    }
    if (!data->connected) {
        nina_tile_grid_show_overlay(s_grid, "Cannot Reach Home Assistant");
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

void ha_page_apply_theme(void) {
    nina_tile_grid_apply_theme(s_grid);
}

/* ── Refresh config (rebuild the row/tile tree) ───────────────────────── */

void ha_page_refresh_config(void) {
    nina_tile_grid_refresh_config(s_grid, app_config_get_ha_tiles());
}
