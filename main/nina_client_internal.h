#pragma once

/**
 * @file nina_client_internal.h
 * @brief Internal shared utilities for nina_client modules.
 *
 * Not part of the public API — only included by nina_client.c,
 * nina_api_fetchers.c, nina_sequence.c, and nina_websocket.c.
 */

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include <time.h>

/* ── Per-task HTTP client context ──
 *
 * Each instance_poll_task registers its context before calling poll functions.
 * http_get_json() looks up the context for the current task to enable
 * keep-alive client reuse.  When no context is registered (e.g., UI
 * coordinator fetching thumbnails/graphs), http_get_json() falls back to
 * standalone mode (no reuse, no keep-alive).
 *
 * Implementation uses a small fixed-size registry indexed by instance,
 * avoiding FreeRTOS TLS (whose index 0 is claimed by LWIP/pthread).
 */
typedef struct {
    esp_http_client_handle_t *client_handle;  /* Points to local reuse_handle */
    bool poll_mode;                           /* true = keep-alive + client reuse */
} http_poll_ctx_t;

/**
 * Register/unregister a poll context for the calling task.
 * Pass NULL to unregister (clear) the context.
 */
void http_poll_ctx_set(http_poll_ctx_t *ctx);

/**
 * Look up the poll context for the calling task.
 * Returns NULL if no context is registered (standalone mode).
 */
http_poll_ctx_t *http_poll_ctx_get(void);

/* HTTP GET and parse JSON response. Caller must cJSON_Delete() the result.
 * Uses per-task poll context for client reuse when available. */
cJSON *http_get_json(const char *url);

/* Parse ISO-8601 datetime string to time_t (UTC). Returns 0 on failure. */
time_t parse_iso8601(const char *str);
