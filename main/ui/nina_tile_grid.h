#pragma once

/**
 * @file nina_tile_grid.h
 * @brief Source-agnostic row-based tile grid (extracted from nina_json.c).
 *
 * Vertical flex of row containers; each row is a horizontal flex whose tiles
 * flex-grow to fill the width; rows split the 720px height evenly. Each tile is
 * a bento box with an uppercase label, a large value, and an optional unit.
 * Values recolor by threshold (Number), value map (Text), or true/false state
 * (Boolean). No header band. A full-coverage empty/error overlay
 * (nina_empty_state; own FLOATING + 100% + opaque black backdrop) is created and
 * managed by this module.
 *
 * Consumed by nina_json.c (JSON Display) and nina_ha.c (Home Assistant). Each
 * page owns one opaque nina_tile_grid_t. The producing page's client (json_client
 * / ha_client) fills a values[]/resolved[] array in ROW-MAJOR flatten order that
 * matches the SAME tiles-config; this module renders it.
 *
 * Tiles-config JSON schema (shared): { "rows": [ [ tile, ... ], ... ] } where
 * each tile carries the DISPLAY keys this module reads:
 *   label, type ("number"|"text"|"bool"),
 *   number: unit, decimals(0-4), low, high, cLow, cNorm, cHigh
 *   text:   maps: [ {val,color}, ... ]  (<=4)
 *   bool:   tText, fText, tColor, fColor
 * Any source field ("path" for JSON, "entity_id"/"attr" for HA) is IGNORED here
 * and parsed only by the page's client.
 *
 * Caps: 6 rows, 4 tiles/row, JSON_MAX_TILES tiles total.
 *
 * LVGL lock: every function runs with the display lock held BY THE CALLER
 * (matches nina_allsky.c / nina_json.c); this module never takes the lock.
 */

#include "lvgl.h"
#include "json_client.h"   /* JSON_MAX_TILES, JSON_TILE_VALUE_LEN (shared caps) */

typedef struct nina_tile_grid nina_tile_grid_t;   /* opaque handle */

/**
 * Build the grid as a child of @p parent. Parses @p tiles_config_json for the
 * display schema, builds the row/tile widget tree, and creates a hidden
 * full-coverage empty-state overlay using @p icon / @p empty_title / @p
 * empty_remedy. Returns an opaque handle (PSRAM-allocated) or NULL on failure.
 * The caller gets the page root via nina_tile_grid_get_root().
 */
nina_tile_grid_t *nina_tile_grid_create(lv_obj_t *parent,
                                        const char *tiles_config_json,
                                        const char *icon,          /* ICON_CLOUD_OFF etc. */
                                        const char *empty_title,
                                        const char *empty_remedy);

/** The page root object (caller hides it initially; dashboard ops.create returns it). */
lv_obj_t *nina_tile_grid_get_root(nina_tile_grid_t *g);

/**
 * Render all tiles from @p values / @p resolved (row-major, @p count tiles).
 * Number: strtof + %.*f, recolor by low/high thresholds. Text: case-insensitive
 * value->color map else theme text color. Boolean: truthy()->t/f text+color.
 * A tile with resolved==false OR values[i][0]=='\0' renders "--" in theme text.
 * Caller holds the source data->mutex AND the display lock.
 * The array types match json_data_t / ha_data_t exactly.
 */
void nina_tile_grid_update(nina_tile_grid_t *g,
                           const char values[][JSON_TILE_VALUE_LEN],
                           const bool *resolved, int count);

/** Re-read @p tiles_config_json and rebuild the row/tile tree (overlay survives). */
void nina_tile_grid_refresh_config(nina_tile_grid_t *g, const char *tiles_config_json);

/** Re-apply the current theme to all widgets + the overlay. */
void nina_tile_grid_apply_theme(nina_tile_grid_t *g);

/** Show/hide the full-coverage empty/error overlay (idempotent; no-op on NULL). */
void nina_tile_grid_show_overlay(nina_tile_grid_t *g, const char *title);
void nina_tile_grid_hide_overlay(nina_tile_grid_t *g);
