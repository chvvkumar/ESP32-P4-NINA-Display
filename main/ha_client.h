#pragma once

/**
 * @file ha_client.h
 * @brief Home Assistant client for the HA page.
 *
 * Per-tile entity+attribute model. Each poll: parse ha_tiles_config into a
 * row-major (entity_id, attr) list; DE-DUPE unique entity_ids; fetch
 * GET {base}/api/states/{entity_id} for each UNIQUE entity SEQUENTIALLY (never
 * concurrent -- respect the ~9-socket ceiling), reusing one keep-alive slot;
 * parse each entity once; resolve every tile's value (state or attributes.<attr>)
 * into a raw scalar string. "unavailable"/"unknown"/missing => resolved=false
 * (renderer shows "--"). Auth: build "Authorization: Bearer <token>" and pass via
 * http_fetch_opts_t.extra_header (mirrors json_client's extra_header path exactly,
 * avoiding any dependency on http_fetch's internal bearer buffer size).
 *
 * Single-owner: only ha_poll_task calls ha_client_poll(); the keep-alive slot
 * needs no locking. Data publish is mutex-protected. Modeled 1:1 on json_client.
 */

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "json_client.h"   /* JSON_MAX_TILES, JSON_TILE_VALUE_LEN */

typedef struct {
    bool    connected;                                   /* last poll reached host + parsed >=1 entity */
    char    values[JSON_MAX_TILES][JSON_TILE_VALUE_LEN]; /* raw scalar; [i][0]=='\0' => unresolved */
    bool    resolved[JSON_MAX_TILES];
    int     tile_count;
    int64_t last_poll_ms;
    SemaphoreHandle_t mutex;
} ha_data_t;

/** Init ha_data_t (creates mutex). Call once before polling. Mirrors json_client_init. */
void ha_client_init(ha_data_t *data);

/** Lock the data struct. Returns true if acquired within timeout_ms. */
bool ha_client_lock(ha_data_t *data, int timeout_ms);

/** Unlock the data struct. */
void ha_client_unlock(ha_data_t *data);

/**
 * Poll all configured HA entities and resolve every tile value.
 *  - base_url: scheme+host+port (no path); appends "/api/states/<entity_id>".
 *  - token: RAW long-lived token; wrapped as Authorization: Bearer.
 *  - tiles_config_json: ha_tiles_config (rows/tiles; each tile has entity_id+attr).
 * On any transport/parse failure of ALL entities, sets data->connected=false.
 */
void ha_client_poll(const char *base_url, const char *token,
                    const char *tiles_config_json, ha_data_t *data);

/** Drop the cached parsed tiles_config. Call when ha_tiles_config changes. */
void ha_client_invalidate_config_cache(void);

/**
 * Single-entity live fetch -- used by the /api/ha-probe handler (one-shot, safe
 * from the httpd worker task). Fetches GET {base_url}/api/states/{entity_id} with
 * Bearer auth; returns the parsed entity JSON (caller must cJSON_Delete) or NULL
 * on transport/status/parse failure. @p base_url/@p token may come from saved
 * config or from X-HA-BASE/X-HA-TOKEN request headers.
 */
cJSON *ha_client_fetch_entity(const char *base_url, const char *token,
                              const char *entity_id);
