#pragma once

/**
 * @file http_fetch.h
 * @brief Shared JSON/text HTTP GET fetcher -- the common case only.
 *
 * Scope: a plain text/JSON HTTP GET with retry, manual redirect-following
 * (esp_http_client's streaming open()/read() path does NOT auto-follow
 * redirects -- only the one-shot perform() variant does), status checking,
 * and a PSRAM-backed response buffer, with optional persistent keep-alive
 * reuse across calls from the same task.
 *
 * NOT for: image streaming (GOES tiles, Spotify album art -- those decode
 * progressively into caller-managed buffers) or OTA binary download (goes
 * through esp_https_ota / ota_github.c's own 4KB chunked writer). Those
 * stay on their own hand-rolled paths.
 *
 * v3 seam note: NINA-specific URL building (/v2/api/ vs /v3/api/ path
 * prefixes), the response-envelope unwrap ({"Response":...,"Success":bool}),
 * and any NINA-specific auth scheme deliberately stay in nina_client.c, NOT
 * here. This module is protocol-agnostic on purpose so a future v2/v3
 * dual-client can sit on top of it unchanged. See
 * .claude memory: reference_ninaapi_v3_migration_delta.
 *
 * Thread-safety: an http_fetch_opts_t / http_fetch_conn_t pair is NOT safe
 * to share across tasks concurrently. Each task that wants keep-alive reuse
 * owns its own http_fetch_conn_t and passes only its own opts, mirroring the
 * per-task poll-context registry pattern in nina_client_internal.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

/** Opaque persistent keep-alive slot; one per task that wants reuse. */
typedef struct http_fetch_conn http_fetch_conn_t;

/**
 * Optional per-attempt diagnostics, reported via http_fetch_opts_t.on_attempt
 * once per attempt (success or failure). Lets a caller with its own latency
 * instrumentation (e.g. NINA's perf_monitor connect/TTFB/body timers) rebuild
 * per-phase timing and retry/failure counters without this module depending
 * on any specific metrics framework.
 */
typedef struct {
    int     attempt_index;    /**< 0-based */
    bool    ok;               /**< true if this attempt produced the final result */
    bool    ever_connected;   /**< true if esp_http_client_open() succeeded this attempt */
    int     status;           /**< HTTP status code; 0 if headers were never reached */
    int64_t connect_us;       /**< esp_http_client_open() duration (0 if not reached) */
    int64_t headers_us;       /**< fetch_headers (TTFB) duration (0 if not reached) */
    int64_t body_us;          /**< body read loop duration (0 if not reached) */
} http_fetch_attempt_info_t;

typedef struct {
    int timeout_ms;             /**< 0 -> 8000 */
    bool use_tls_bundle;        /**< true -> esp_crt_bundle_attach (HTTPS cert validation) */
    int max_redirects;          /**< 0 = don't follow redirects */
    int max_attempts;           /**< 0 -> 1 (no retry) */
    int retry_delay_ms;         /**< flat delay between attempts; 0 -> 500 */
    size_t max_response_bytes;  /**< 0 -> 65536; hard cap, includes the NUL terminator */
    const char *bearer_token;   /**< optional: adds "Authorization: Bearer <token>" */
    const char *user_agent;     /**< optional: adds a "User-Agent" header */
    const char *accept;         /**< optional: adds an "Accept" header */
    const char *host_header;    /**< optional: explicit "Host" header, re-applied on every
                                  * attempt (including a reused keep-alive connection).
                                  * For mDNS-bypass callers that rewrite the request URL's
                                  * host to a numeric IP so esp_http_client skips
                                  * getaddrinfo(): pass the original hostname here so the
                                  * server still sees the intended Host. Applied before
                                  * esp_http_client_open(), same as any other header. */
    void (*on_attempt)(const http_fetch_attempt_info_t *info, void *hook_ctx);
                                 /**< optional: NULL disables. See http_fetch_attempt_info_t. */
    void *hook_ctx;              /**< passed through unchanged to on_attempt */
    http_fetch_conn_t *conn;    /**< optional: NULL = one-shot client (no reuse) */
    int *status_out;            /**< optional: written with the FINAL attempt's HTTP
                                  * status code whenever a status was received --
                                  * including non-2xx failure returns (e.g. 204, 401,
                                  * 500). Left untouched if no response ever arrived
                                  * (transport failure -- DNS/connect/timeout). Caller
                                  * should pre-set to 0 to distinguish "no response"
                                  * from a real status of 0. */
    const char *capture_header;  /**< optional: name of ONE response header to capture
                                  * as a raw string (case-insensitive match); NULL = off.
                                  * No parsing happens here -- this layer stays
                                  * protocol-agnostic. */
    char *capture_header_out;    /**< caller buffer for the captured value. On a
                                  * successful fetch, holds the header value from the
                                  * FINAL response (after redirects), NUL-terminated
                                  * (truncated to fit). If the header was absent, or
                                  * capture is disabled, the buffer is set to "" (when
                                  * provided). */
    size_t capture_header_out_len; /**< size of capture_header_out incl. NUL */
} http_fetch_opts_t;

/**
 * Fetch @p url via HTTP GET.
 *
 * On success returns ESP_OK, and *out_body is a NUL-terminated PSRAM
 * buffer the caller must release with heap_caps_free(); *out_len is the
 * body length excluding the NUL. On failure returns an esp_err_t != ESP_OK
 * and leaves *out_body / *out_len untouched.
 *
 * @param opts  May be NULL to use all defaults (one-shot, no retry, 8s
 *              timeout, 64KB cap, no TLS bundle).
 */
esp_err_t http_fetch_text(const char *url, const http_fetch_opts_t *opts,
                           char **out_body, size_t *out_len);

/**
 * Convenience wrapper: http_fetch_text() + cJSON_Parse(). Returns NULL on
 * any failure (transport, HTTP status, or JSON parse). Caller must
 * cJSON_Delete() the result.
 */
cJSON *http_fetch_json(const char *url, const http_fetch_opts_t *opts);

/**
 * Create a persistent keep-alive slot for the calling task's poll loop.
 * Returns NULL on allocation failure. Free with http_fetch_conn_destroy().
 */
http_fetch_conn_t *http_fetch_conn_create(void);

/** Destroy a keep-alive slot, cleaning up any live client handle it holds. */
void http_fetch_conn_destroy(http_fetch_conn_t *conn);
