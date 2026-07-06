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
#include "http_fetch.h"
#include <time.h>

/* ── Per-task HTTP client context ──
 *
 * Each instance_poll_task registers its context before calling poll functions.
 * http_get_json() looks up the context for the current task to enable
 * keep-alive client reuse via the shared fetcher (main/http_fetch.h). When no
 * context is registered (e.g., UI coordinator fetching thumbnails/graphs),
 * http_get_json() falls back to standalone mode (one-shot, no reuse, no
 * keep-alive).
 *
 * Implementation uses a small fixed-size registry indexed by instance,
 * avoiding FreeRTOS TLS (whose index 0 is claimed by LWIP/pthread).
 */
typedef struct {
    http_fetch_conn_t *conn;  /* Persistent keep-alive slot, owned by the caller's
                               * nina_poll_state_t.http_client. NULL = standalone/
                               * one-shot mode (no reuse, no keep-alive). */
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

/* Variant of http_get_json() that also captures the response "Date" header
 * (the NINA PC's own clock) parsed to a UTC epoch. Writes 0 to *date_epoch_out
 * when the header is missing/unparseable or the fetch fails; date_epoch_out
 * may be NULL (behaves exactly like http_get_json()). */
cJSON *http_get_json_dated(const char *url, int64_t *date_epoch_out);

/* Resolve an IPv4 hostname to its dotted-quad string using the app-level
 * DNS cache (60s TTL, stale fallback). On a hit, copies the cached IP into
 * ip_out and returns true; on a miss, performs a fresh getaddrinfo(), caches
 * the result, and returns true. Returns false (ip_out untouched/empty) only
 * when resolution fails and no cached entry exists.
 *
 * A numeric-IP host is returned verbatim (no DNS). ip_out should be at least
 * 16 bytes (INET_ADDRSTRLEN) for IPv4. This lets the request path replace a
 * .lan mDNS hostname with a numeric IP so esp_http_client skips getaddrinfo()
 * on the hot path, while the original hostname is still sent in the Host header. */
bool nina_client_resolve_host(const char *host, char *ip_out, size_t ip_len);

/* NINA Advanced API envelope helpers. The API wraps every response in
 * { "Response": ..., "Success": bool, ... }. These honor the application-level
 * Success flag so callers can treat Success!=true as "API unavailable" even when
 * the HTTP transport succeeded. Neither helper deletes the envelope — caller owns it. */

/* True iff envelope != NULL and its "Success" field is true. */
bool nina_api_envelope_ok(cJSON *envelope);

/* Returns the inner "Response" item ONLY if the envelope is OK (Success true);
 * else NULL. */
cJSON *nina_api_response(cJSON *envelope);

/* Parse ISO-8601 datetime string to time_t (UTC). Returns 0 on failure. */
time_t parse_iso8601(const char *str);
