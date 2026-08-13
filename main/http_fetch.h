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
 * Binary bodies (GOES/solar tiles, NINA thumbnails, Spotify album art) go
 * through http_fetch_binary() at the bottom of this header, which shares the
 * same open/redirect/read shell but hands back a raw byte buffer instead of a
 * NUL-terminated string.
 *
 * NOT for: OTA binary download (goes through esp_https_ota / ota_github.c's
 * own 4KB chunked writer straight into the flash partition -- it never holds
 * the image in RAM, so it stays on its own path).
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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
    const char *extra_header;   /**< optional: raw "Name: value" line, split at the
                                  * first ':' and applied as one request header. Lets
                                  * a caller forward an arbitrary auth header (e.g.
                                  * "X-API-Key: abc") that is not Bearer-shaped. */
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

/* ------------------------------------------------------------------------
 * Binary streaming fetch (images)
 * ---------------------------------------------------------------------- */

/** What to do when the server's Content-Length exceeds opts->max_size.
 *
 * There is deliberately no "honour it anyway" policy: that lets one response
 * header dictate the allocation size, which is the whole reason max_size
 * exists. Pick CLAMP only where a truncated body is still useful. */
typedef enum {
    HTTP_BIN_OVERSIZE_CLAMP = 0, /**< read at most max_size bytes (GOES/solar tiles) */
    HTTP_BIN_OVERSIZE_REJECT,    /**< fail before allocating (Spotify album art,
                                   *  NINA thumbnail) */
} http_fetch_oversize_t;

/**
 * Options for http_fetch_binary(). Zero-initialise and set what you need;
 * max_size is the only required field.
 */
typedef struct {
    int timeout_ms;            /**< 0 -> 8000 */
    bool use_tls_bundle;       /**< true -> esp_crt_bundle_attach (HTTPS) */
    int max_redirects;         /**< 0 = don't follow 30x */
    int rx_buffer_size;        /**< esp_http_client rx buffer; 0 -> IDF default */
    int tx_buffer_size;        /**< esp_http_client tx buffer; 0 -> IDF default */
    size_t max_size;           /**< REQUIRED hard cap; see http_fetch_oversize_t */
    size_t unknown_len_size;   /**< initial buffer when no Content-Length; 0 -> max_size */
    size_t grow_step;          /**< realloc increment when the buffer fills; 0 = never grow */
    http_fetch_oversize_t oversize;
    bool shrink_to_fit;        /**< realloc down to the byte count actually read */
    bool probe_overflow;       /**< with no Content-Length and the buffer exactly full,
                                 *  read one more byte: if data remains the source
                                 *  exceeded max_size -- fail rather than return a
                                 *  silently truncated image */
    bool require_full_length;  /**< fail when a non-chunked response delivered fewer
                                 *  bytes than its Content-Length */
    uint8_t *prealloc;         /**< optional scratch buffer used INSTEAD of a malloc
                                 *  when the response fits, to avoid PSRAM churn.
                                 *  Requires prealloc_lock. Never grown; the result is
                                 *  copied out to a right-sized PSRAM allocation and
                                 *  the lock is released before returning, so the
                                 *  caller's ownership rules are the same either way. */
    size_t prealloc_size;
    SemaphoreHandle_t prealloc_lock;  /**< guards @ref prealloc; required to use it */
    int prealloc_lock_wait_ms;        /**< take() timeout; on timeout the fetch falls
                                        *  back to a plain PSRAM allocation */
    const char *label;         /**< short subject for log lines ("Album art"); NULL -> "HTTP" */
} http_fetch_binary_opts_t;

/**
 * Stream a binary HTTP GET body (JPEG/PNG/...) into a PSRAM buffer.
 *
 * Covers the shell that every image fetch on this device needs: client setup,
 * open + fetch_headers, the manual redirect chain (esp_http_client's streaming
 * path does NOT auto-follow; each hop closes the previous socket before
 * re-opening, which matters against this board's ~9 concurrent connection
 * ceiling), status check, sizing against a cap, the read loop, and cleanup.
 * Payload validation (JPEG magic, dimensions, decode) stays with the caller.
 *
 * On ESP_OK, *out_buf is a PSRAM buffer the caller frees with heap_caps_free()
 * and *out_len is its length. On failure both are left NULL/0 and nothing is
 * leaked. Returns the esp_http_client error for a transport failure,
 * ESP_ERR_NO_MEM for an allocation failure, ESP_FAIL otherwise (bad status,
 * empty/short body, over cap).
 */
esp_err_t http_fetch_binary(const char *url, const http_fetch_binary_opts_t *opts,
                             uint8_t **out_buf, size_t *out_len);

/**
 * Create a persistent keep-alive slot for the calling task's poll loop.
 * Returns NULL on allocation failure. Free with http_fetch_conn_destroy().
 */
http_fetch_conn_t *http_fetch_conn_create(void);

/** Destroy a keep-alive slot, cleaning up any live client handle it holds. */
void http_fetch_conn_destroy(http_fetch_conn_t *conn);
