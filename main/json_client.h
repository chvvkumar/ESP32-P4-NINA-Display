#pragma once

/**
 * @file json_client.h
 * @brief Generic JSON HTTP client for the JSON Display page.
 *
 * Polls an arbitrary JSON HTTP endpoint (e.g. a Home Assistant /api/states
 * dump) and resolves a configured set of tile "paths" into raw scalar
 * strings. Modeled 1:1 on allsky_client.h/.c.
 *
 * The path resolver extends AllSky's dot-notation walker with two array
 * selectors ported from the JSON Display mockup (tokenizePath/evalPath):
 *   - .key            object member
 *   - [N]             array index N
 *   - [field=value]   first array element where String(el[field]) === value
 *                     (value may contain dots, e.g. an entity_id)
 *
 * The client stores only the RAW resolved scalar as a string. The UI layer
 * (ui/nina_json.c) computes label/format/threshold color from the same
 * json_tiles_config, parsed independently in the SAME row-major flatten order.
 */

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Row-based grid: <=6 rows x <=4 tiles/row, <=15 tiles total. */
#define JSON_MAX_TILES        15
#define JSON_TILE_VALUE_LEN   48   /* resolved raw scalar string per tile */

/* Resolved per-tile values in ROW-MAJOR flatten order (row 0 left->right,
 * then row 1, ...). Index i is the i-th tile in that order. The UI computes
 * label/format/color from json_tiles_config; the client only stores the raw
 * resolved scalar as a string (HA sends numbers as strings, e.g. "88"). */
typedef struct {
    bool    connected;                                   /* last poll reached host + parsed OK */
    char    values[JSON_MAX_TILES][JSON_TILE_VALUE_LEN]; /* raw scalar; values[i][0]=='\0' => unresolved */
    bool    resolved[JSON_MAX_TILES];                    /* true iff path resolved this poll */
    int     tile_count;                                  /* # tiles flattened from config this poll */
    int64_t last_poll_ms;
    SemaphoreHandle_t mutex;
} json_data_t;

/** Init json_data_t (creates mutex). Call once before polling. Mirrors allsky_data_init. */
void json_client_init(json_data_t *data);

/** Lock the data struct. Returns true if acquired within timeout_ms. Mirrors allsky_data_lock. */
bool json_client_lock(json_data_t *data, int timeout_ms);

/** Unlock the data struct. Mirrors allsky_data_unlock. */
void json_client_unlock(json_data_t *data);

/**
 * Fetch @p url (optional @p auth_header, "Name: value" or ""/NULL), parse JSON,
 * resolve every tile path from @p tiles_config_json in ROW-MAJOR order, and
 * store raw scalar strings into *data under its mutex.
 *  - url may be http or https; use_tls_bundle=true when it starts with "https".
 *  - auth_header is forwarded verbatim as a raw "Name: value" request header
 *    (via http_fetch_opts_t.extra_header), e.g. "Authorization: Bearer <token>"
 *    or "X-API-Key: <key>"; empty/NULL sends no extra header.
 *  - On any failure sets data->connected=false under the mutex (mirrors
 *    allsky_client_poll error path).
 */
void json_client_poll(const char *url, const char *auth_header,
                      const char *tiles_config_json, json_data_t *data);

/** Invalidate cached parsed tiles_config. Call when json_tiles_config changes.
 *  Mirrors allsky_invalidate_field_config_cache. */
void json_client_invalidate_config_cache(void);

/**
 * Resolve a single path against a parsed cJSON root and write the raw scalar
 * string into @p out (NUL-terminated, truncated to @p out_len). Supports the
 * .key / [N] / [field=value] token grammar described above.
 *
 * @return true if the path resolved to a scalar (string/number/bool),
 *         false otherwise (out is set to "" on false).
 *
 * @p root is void* to avoid forcing cJSON.h on this public header; the impl
 * casts to cJSON*.
 */
bool json_client_resolve_path(void *root, const char *path, char *out, size_t out_len);
