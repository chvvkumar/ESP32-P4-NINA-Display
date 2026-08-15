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
 * Page-active gate for keep-alive teardown. Call on every HA page enter/leave
 * transition (tasks.c, mirrors octoprint_client_set_page_active). On leave,
 * destroys the keep-alive conn slot -- a drained-parked slot holds an OPEN
 * socket, and the page-gated poll loop stops running, so the slot would
 * otherwise hold a dead socket against the ~9-connection ceiling indefinitely.
 * Safe against a poll in flight (internal mutex + zero-wait try-take).
 */
void ha_client_set_page_active(bool active);

/**
 * Single-entity live fetch -- used by the /api/ha-probe handler (one-shot, safe
 * from the httpd worker task). Fetches GET {base_url}/api/states/{entity_id} with
 * Bearer auth; returns the parsed entity JSON (caller must cJSON_Delete) or NULL
 * on transport/status/parse failure. @p base_url/@p token may come from saved
 * config or from X-HA-BASE/X-HA-TOKEN request headers.
 */
cJSON *ha_client_fetch_entity(const char *base_url, const char *token,
                              const char *entity_id);

/**
 * Credentials check -- used by the /api/ha-test handler (one-shot, safe from the
 * httpd worker task). Fetches GET {base_url}/api/config with Bearer auth.
 *
 * Returns the upstream HTTP status code, or 0 when no response ever arrived
 * (DNS/connect/timeout) -- ha_client_fetch_entity collapses all of those into
 * NULL, which is why this variant exists. On HTTP 200 with a parseable body,
 * *out_json receives the parsed JSON (caller must cJSON_Delete); in every other
 * case *out_json is set to NULL. @p out_json may be NULL (body discarded).
 */
int ha_client_fetch_config(const char *base_url, const char *token,
                           cJSON **out_json);
