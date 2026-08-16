/**
 * @file http_fetch.c
 * @brief Shared JSON/text HTTP GET fetcher. See http_fetch.h for scope.
 */

#include "http_fetch.h"
#include "http_fetch_policy.h"

#include <string.h>
#include <strings.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_fetch";

struct http_fetch_conn {
    esp_http_client_handle_t client; /* NULL until first successful fetch */
    char extra_name[80];             /* name of the extra_header the parked handle
                                      * still carries; scrubbed before the next
                                      * request so a stale credential cannot ride
                                      * along to a host that never authorised it */
};

/**
 * Clear the caller's capture buffer (if provided). Called before every
 * response-header fetch -- including each redirect re-open -- so that only
 * the FINAL response's header value survives; each hop re-fetches headers
 * and would otherwise leave a stale value from an intermediate response.
 */
static void capture_reset(const http_fetch_opts_t *opts) {
    if (!opts) return;   /* binary path has no header capture */
    if (opts->capture_header_out && opts->capture_header_out_len > 0) {
        opts->capture_header_out[0] = '\0';
    }
}

/**
 * esp_http_client event callback registered on every client this module
 * creates (including reused keep-alive handles). Arbitrary response headers
 * are only observable via HTTP_EVENT_ON_HEADER -- they are not queryable
 * after esp_http_client_fetch_headers(). evt->user_data is read from the
 * client's user_data at dispatch time, so attempt_once() can re-point it at
 * the current request's opts even though the client handle persists across
 * requests.
 */
static esp_err_t header_capture_event_cb(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;

    const http_fetch_opts_t *opts = (const http_fetch_opts_t *)evt->user_data;
    if (!opts || !opts->capture_header ||
        !opts->capture_header_out || opts->capture_header_out_len == 0) {
        return ESP_OK;
    }
    if (!evt->header_key || !evt->header_value) return ESP_OK;
    if (strcasecmp(evt->header_key, opts->capture_header) != 0) return ESP_OK;

    /* Copy with an explicit length clamp: the event's value string is freed
     * by esp_http_client immediately after dispatch. */
    size_t n = strnlen(evt->header_value, opts->capture_header_out_len - 1);
    memcpy(opts->capture_header_out, evt->header_value, n);
    opts->capture_header_out[n] = '\0';
    return ESP_OK;
}

/** Fill in defaults for any unset (zero/negative) field. */
static void normalize_opts(http_fetch_opts_t *o) {
    if (o->timeout_ms <= 0) o->timeout_ms = 8000;
    if (o->max_attempts <= 0) o->max_attempts = 1;
    if (o->retry_delay_ms <= 0) o->retry_delay_ms = 500;
    if (o->max_response_bytes == 0) o->max_response_bytes = 65536;
}

/** Allocate a fresh esp_http_client for @p url per @p opts. */
static esp_http_client_handle_t make_client(const char *url,
                                             const http_fetch_opts_t *opts,
                                             bool keep_alive) {
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = opts->timeout_ms,
        .keep_alive_enable = keep_alive,
        .crt_bundle_attach = opts->use_tls_bundle ? esp_crt_bundle_attach : NULL,
        .event_handler = header_capture_event_cb,
        /* user_data intentionally NULL here: the capture target is attached
         * per request in attempt_once() via esp_http_client_set_user_data(),
         * because a keep-alive client outlives any single opts. */
    };
    return esp_http_client_init(&cfg);
}

/** True if @p s contains no CR or LF. NUL cannot appear inside a C string, so
 * CR/LF are the only characters that can terminate a header line early. */
static bool header_token_is_clean(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n') return false;
    }
    return true;
}

/**
 * Set one request header, refusing anything that could inject additional
 * headers or split the request.
 *
 * Every header this module emits routes through here, which matters because the
 * values are caller-supplied secrets that ultimately come from a user-editable
 * field: the /api/config/pull password, ha_token, json_auth_header, a Spotify
 * bearer. A CR or LF inside any of them would end the header line early and let
 * the remainder be read as further headers (or as a body). Reject the whole
 * header rather than strip the offending bytes -- a credential containing a
 * newline is malformed input, not something to silently repair, and stripping
 * would send a subtly wrong secret instead of failing visibly.
 *
 * The value is NEVER logged; it is the secret. The name is logged only once it
 * is known clean, so a crafted name cannot forge log lines either.
 */
static void set_header_checked(esp_http_client_handle_t client,
                               const char *name, const char *value) {
    if (!name || !value) return;
    if (!header_token_is_clean(name)) {
        ESP_LOGW(TAG, "Dropping request header: name contains CR/LF");
        return;
    }
    if (!header_token_is_clean(value)) {
        ESP_LOGW(TAG, "Dropping request header '%s': value contains CR/LF", name);
        return;
    }
    esp_http_client_set_header(client, name, value);
}

/** Apply the optional headers (and request method) from @p opts to @p client. */
static void apply_headers(esp_http_client_handle_t client, const http_fetch_opts_t *opts) {
    /* The method is not a header, but this is the right place for it: every
     * caller of apply_headers() is a point where a FRESH or a REUSED keep-alive
     * handle needs re-arming for this request, and a reused handle would
     * otherwise carry the previous request's method. Setting it explicitly in
     * both directions keeps a POST from leaking onto the next GET. */
    esp_http_client_set_method(client, opts->post_body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (opts->post_body) {
        set_header_checked(client, "Content-Type",
                           opts->content_type ? opts->content_type : "application/json");
    } else {
        /* Same reason as the method above, and the header list is NOT re-armed
         * on its own: drop a Content-Type left behind by a previous POST on this
         * keep-alive handle so it cannot ride along on this GET. */
        esp_http_client_delete_header(client, "Content-Type");
    }
    if (opts->host_header) {
        /* Re-assert on every call: a reused keep-alive handle may have served
         * a different Host on a prior request, and esp_http_client_set_url()
         * does not update an already-set Host header. */
        set_header_checked(client, "Host", opts->host_header);
    }
    if (opts->bearer_token) {
        /* 1200 bytes: real-world bearer tokens (e.g. Spotify OAuth access
         * tokens) can run several hundred characters -- 256 silently dropped
         * the header (snprintf truncation check below) for long tokens. */
        char hdr[1200];
        int n = snprintf(hdr, sizeof(hdr), "Bearer %s", opts->bearer_token);
        if (n > 0 && n < (int)sizeof(hdr)) {
            set_header_checked(client, "Authorization", hdr);
        }
    }
    if (opts->extra_header && opts->extra_header[0] != '\0') {
        /* Split a raw "Name: value" line at the FIRST ':' and apply it as one
         * request header, so a non-Bearer auth header (e.g. "X-API-Key: abc")
         * is forwarded verbatim. The name is copied out to NUL-terminate it;
         * leading spaces in the value are trimmed. Header line capped at 256. */
        const char *line = opts->extra_header;
        const char *colon = strchr(line, ':');
        if (colon && colon != line) {
            size_t name_len = (size_t)(colon - line);
            char name[256];
            if (name_len < sizeof(name)) {
                memcpy(name, line, name_len);
                name[name_len] = '\0';
                const char *value = colon + 1;
                while (*value == ' ') {
                    value++;
                }
                set_header_checked(client, name, value);
            }
        }
    }
    if (opts->user_agent) {
        set_header_checked(client, "User-Agent", opts->user_agent);
    }
    if (opts->accept) {
        set_header_checked(client, "Accept", opts->accept);
    }
}

/**
 * Drop the extra_header a parked keep-alive handle still carries from its
 * previous request, then record the name of the one this request will set.
 * Headers persist on an esp_http_client handle -- they are not per-request --
 * so without this a reused handle would keep sending an old credential (e.g.
 * an X-Api-Key) to a host the current request never authorised it for. No-op
 * without a conn slot; deleting a header a fresh handle never had is harmless.
 * Call before apply_headers() on every request that has a conn slot.
 */
static void scrub_stale_extra_header(esp_http_client_handle_t client,
                                     http_fetch_conn_t *conn,
                                     const char *new_extra) {
    if (!conn) return;
    if (conn->extra_name[0] != '\0') {
        esp_http_client_delete_header(client, conn->extra_name);
        conn->extra_name[0] = '\0';
    }
    if (new_extra && new_extra[0] != '\0') {
        const char *colon = strchr(new_extra, ':');
        size_t n = colon ? (size_t)(colon - new_extra) : 0;
        if (n > 0 && n < sizeof(conn->extra_name)) {
            memcpy(conn->extra_name, new_extra, n);
            conn->extra_name[n] = '\0';
        }
    }
}

/**
 * Open @p client and, for a POST, write the whole request body.
 *
 * esp_http_client_open() needs the body length up front (it emits the
 * Content-Length header from it), so the open and the single write belong
 * together -- and both have to repeat on every redirect hop, because each hop
 * re-opens the socket. @p opts is NULL on the binary path, which never posts,
 * so that reduces to the original open(client, 0).
 *
 * @p opened_out (optional) latches true the moment the socket is up, BEFORE the
 * body write: a server that accepted the connection and then dropped it mid-body
 * is reachable, and callers use this latch to tell "unreachable host" apart from
 * "reachable host, failed request".
 */
static esp_err_t open_write_body(esp_http_client_handle_t client,
                                  const http_fetch_opts_t *opts,
                                  bool *opened_out) {
    size_t blen = (opts && opts->post_body) ? strlen(opts->post_body) : 0;
    esp_err_t err = esp_http_client_open(client, (int)blen);
    if (err != ESP_OK) return err;
    if (opened_out) *opened_out = true;
    if (blen == 0) return ESP_OK;

    int written = esp_http_client_write(client, opts->post_body, (int)blen);
    if (written < 0 || (size_t)written != blen) {
        ESP_LOGW(TAG, "POST body write failed (%d of %u bytes)",
                 written, (unsigned)blen);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * Open @p client, fetch headers, and follow any redirect chain (streaming
 * open()/read() does not auto-follow -- must be done manually per hop).
 * On success fills *status_out / *content_length_out and returns ESP_OK.
 * On transport failure returns the esp_http_client error.
 *
 * @param opts          text-fetch options, used ONLY for response-header
 *                       capture; NULL from the binary path, which captures
 *                       nothing.
 * @param what          subject for the per-hop log line (URL, or a short label).
 * @param opened_out    set true as soon as the FIRST esp_http_client_open()
 *                       call (before any redirect hop) succeeds, regardless
 *                       of what happens afterward -- mirrors the "ever
 *                       connected" latch callers use to distinguish an
 *                       unreachable host from a reachable one that failed.
 * @param connect_us_out cumulative esp_http_client_open() duration across
 *                       the initial open and any redirect re-opens.
 * @param headers_us_out cumulative esp_http_client_fetch_headers() duration.
 * All three out-params may be NULL when the caller doesn't need them.
 */
static esp_err_t open_and_follow_redirects(esp_http_client_handle_t client,
                                            const http_fetch_opts_t *opts,
                                            int max_redirects, const char *what,
                                            int *status_out, int *content_length_out,
                                            bool *opened_out, int64_t *connect_us_out,
                                            int64_t *headers_us_out) {
    capture_reset(opts);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = open_write_body(client, opts, opened_out);
    if (connect_us_out) *connect_us_out += esp_timer_get_time() - t0;
    if (err != ESP_OK) return err;

    int64_t t1 = esp_timer_get_time();
    int content_length = esp_http_client_fetch_headers(client);
    if (headers_us_out) *headers_us_out += esp_timer_get_time() - t1;
    int status = esp_http_client_get_status_code(client);

    /* A POST is never replayed at a new location. Replaying the body would be
     * wrong for 303 (which mandates a bodyless GET) and for 301/302 (which every
     * client turns into a GET in practice), and silently downgrading the caller's
     * POST to a GET would send a different request than the one asked for. The
     * 3xx is handed back through status_out for the caller to deal with. */
    const bool has_body = (opts && opts->post_body);

    int redirects = 0;
    while (http_status_is_redirect(status) && redirects < max_redirects && !has_body) {
        ESP_LOGI(TAG, "HTTP %d redirect, following (hop %d): %s",
                 status, redirects + 1, what);
        err = esp_http_client_set_redirection(client);
        if (err != ESP_OK) break; /* no Location header or similar -- stop following */

        esp_http_client_close(client);
        capture_reset(opts); /* only the final hop's header value may survive */
        t0 = esp_timer_get_time();
        err = open_write_body(client, opts, opened_out);
        if (connect_us_out) *connect_us_out += esp_timer_get_time() - t0;
        if (err != ESP_OK) return err;

        t1 = esp_timer_get_time();
        content_length = esp_http_client_fetch_headers(client);
        if (headers_us_out) *headers_us_out += esp_timer_get_time() - t1;
        status = esp_http_client_get_status_code(client);
        redirects++;
    }

    *status_out = status;
    *content_length_out = content_length;
    return ESP_OK;
}

/**
 * Park (keep-alive) or cleanup @p client depending on whether @p conn is set.
 *
 * @p drained: the response body was fully consumed, so the connection is
 * position-synced and parked OPEN -- this is what makes reuse skip TCP+TLS:
 * esp_http_client_open() only reconnects when the handle's state dropped below
 * HTTP_STATE_CONNECTED, which close() causes (IDF 5.5.2 esp_http_client.c:1580).
 * Pass false whenever body bytes may remain unread (non-2xx exit, cap
 * truncation, partial read, mid-body failure): leftover bytes would desync the
 * next response on the socket, so those close first and reuse only the handle
 * allocation. Redirect hops are unaffected -- open_and_follow_redirects()
 * closes each intermediate hop itself; only the FINAL hop's connection ever
 * reaches this function. A parked-open socket the server later drops is caught
 * by the callers' stale-reconnect (error or status -1 on the reused handle ->
 * destroy, recreate, retry once).
 */
static void finish_client(esp_http_client_handle_t client, http_fetch_conn_t *conn,
                          bool drained) {
    if (conn) {
        /* Detach the capture context before parking the handle: it points at
         * this request's opts (stack of http_fetch_text) and would dangle. */
        esp_http_client_set_user_data(client, NULL);
        if (!drained) {
            esp_http_client_close(client);
        }
        conn->client = client;
    } else {
        esp_http_client_cleanup(client);
    }
}

/**
 * Perform a single fetch attempt (no retry-loop delay here -- that lives in
 * http_fetch_text()). Sets *retryable to tell the caller whether another
 * attempt is worth trying on failure. @p info accumulates per-phase timing /
 * status for opts->on_attempt; caller pre-zeroes it and sets attempt_index.
 */
static esp_err_t attempt_once(const char *url, const http_fetch_opts_t *opts,
                               char **out_body, size_t *out_len, bool *retryable,
                               http_fetch_attempt_info_t *info) {
    *retryable = true;
    http_fetch_conn_t *conn = opts->conn;
    bool reused = (conn && conn->client != NULL);

    esp_http_client_handle_t client;
    if (reused) {
        client = conn->client;
        esp_http_client_set_url(client, url);
    } else {
        client = make_client(url, opts, conn != NULL);
        if (!client) return ESP_ERR_NO_MEM;
    }
    scrub_stale_extra_header(client, conn, opts->extra_header);
    apply_headers(client, opts);
    /* Point the header-capture event handler at this request's opts (const
     * cast: the handler only writes through opts->capture_header_out). */
    esp_http_client_set_user_data(client, (void *)opts);

    int status = 0;
    int content_length = 0;
    esp_err_t err = open_and_follow_redirects(client, opts, opts->max_redirects, url,
                                               &status, &content_length,
                                               &info->ever_connected, &info->connect_us,
                                               &info->headers_us);

    if ((err != ESP_OK || status == -1) && reused) {
        /* Stale/dead keep-alive connection (status -1 = server closed it
         * silently) -- destroy and retry once within this attempt, mirroring
         * spotify_client.c's player_client reconnect pattern. */
        ESP_LOGD(TAG, "Stale keep-alive for %s -- reconnecting", url);
        esp_http_client_cleanup(client);
        conn->client = NULL;

        client = make_client(url, opts, true);
        if (!client) return ESP_ERR_NO_MEM;
        scrub_stale_extra_header(client, conn, opts->extra_header);
        apply_headers(client, opts);
        esp_http_client_set_user_data(client, (void *)opts);
        err = open_and_follow_redirects(client, opts, opts->max_redirects, url,
                                         &status, &content_length,
                                         &info->ever_connected, &info->connect_us,
                                         &info->headers_us);
    }

    info->status = status;

    if (err != ESP_OK) {
        /* Transport-level failure (connect/DNS/etc.) -- retryable. */
        esp_http_client_cleanup(client);
        return err;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        finish_client(client, conn, false);
        *retryable = (status >= 500);
        return ESP_FAIL;
    }

    size_t cap = opts->max_response_bytes;

    if (content_length > 0 && (size_t)content_length + 1 > cap) {
        ESP_LOGW(TAG, "response too large (%d bytes, cap %u) for %s",
                 content_length, (unsigned)cap, url);
        finish_client(client, conn, false);
        *retryable = false;
        return ESP_ERR_INVALID_SIZE;
    }

    size_t bufsize = http_buf_initial(content_length, cap);
    if (bufsize == 0) {
        finish_client(client, conn, false);
        *retryable = false;
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = heap_caps_malloc(bufsize, MALLOC_CAP_SPIRAM);
    if (!buf) {
        finish_client(client, conn, false);
        *retryable = false;
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    bool truncated = false;
    int64_t body_start_us = esp_timer_get_time();
    while (total + 1 < bufsize) {
        int n = esp_http_client_read(client, buf + total, (int)(bufsize - 1 - total));
        if (n <= 0) break;
        total += (size_t)n;

        if (total + 1 >= bufsize) {
            size_t grown = http_buf_grow(bufsize, cap);
            if (grown == 0) {
                /* Cap reached. For a known content_length this just means we
                 * read exactly up to it -- not truncation. For an unknown-
                 * length stream it genuinely is truncation. */
                truncated = (content_length <= 0);
                break;
            }
            char *nb = heap_caps_realloc(buf, grown, MALLOC_CAP_SPIRAM);
            if (!nb) {
                info->body_us = esp_timer_get_time() - body_start_us;
                heap_caps_free(buf);
                finish_client(client, conn, false);
                *retryable = false;
                return ESP_ERR_NO_MEM;
            }
            buf = nb;
            bufsize = grown;
        }
    }
    info->body_us = esp_timer_get_time() - body_start_us;
    buf[total] = '\0';

    if (content_length > 0 && total < (size_t)content_length) {
        ESP_LOGW(TAG, "Partial HTTP read: %u/%d bytes for %s",
                 (unsigned)total, content_length, url);
        heap_caps_free(buf);
        finish_client(client, conn, false);
        return ESP_FAIL; /* retryable stays true */
    }

    if (truncated) {
        ESP_LOGW(TAG, "Response truncated at cap (%u bytes) for %s", (unsigned)cap, url);
    }

    /* Drained = esp_http_client says the whole body was consumed (tracks both
     * Content-Length position and the chunked terminator). Covers the
     * truncated-at-cap success return too: incomplete -> close before park. */
    finish_client(client, conn, esp_http_client_is_complete_data_received(client));
    *out_body = buf;
    *out_len = total;
    info->ok = true;
    return ESP_OK;
}

esp_err_t http_fetch_text(const char *url, const http_fetch_opts_t *opts_in,
                           char **out_body, size_t *out_len) {
    if (!url || !out_body || !out_len) return ESP_ERR_INVALID_ARG;

    http_fetch_opts_t opts = opts_in ? *opts_in : (http_fetch_opts_t){0};
    normalize_opts(&opts);

    esp_err_t last_err = ESP_FAIL;
    for (int attempt = 0; attempt < opts.max_attempts; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(opts.retry_delay_ms));
        }

        bool retryable = true;
        http_fetch_attempt_info_t info = { .attempt_index = attempt };
        esp_err_t err = attempt_once(url, &opts, out_body, out_len, &retryable, &info);
        if (opts.on_attempt) opts.on_attempt(&info, opts.hook_ctx);
        if (opts.status_out && info.status != 0) *opts.status_out = info.status;
        if (err == ESP_OK) return ESP_OK;

        last_err = err;
        if (!retryable) break;
        if (!http_should_retry(attempt, opts.max_attempts)) break;
    }

    ESP_LOGW(TAG, "%s unreachable (%s)", url, esp_err_to_name(last_err));
    return last_err;
}

esp_err_t http_fetch_binary(const char *url, const http_fetch_binary_opts_t *opts_in,
                             uint8_t **out_buf, size_t *out_len) {
    if (!url || !opts_in || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    if (opts_in->max_size == 0) return ESP_ERR_INVALID_ARG;

    http_fetch_binary_opts_t o = *opts_in;
    if (o.timeout_ms <= 0) o.timeout_ms = 8000;
    if (o.unknown_len_size == 0) o.unknown_len_size = o.max_size;
    const char *what = o.label ? o.label : "HTTP";

    *out_buf = NULL;
    *out_len = 0;

    http_fetch_conn_t *conn = o.conn;
    bool reused = (conn && conn->client != NULL);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = o.timeout_ms,
        .buffer_size = o.rx_buffer_size,      /* 0 -> esp_http_client default */
        .buffer_size_tx = o.tx_buffer_size,   /* 0 -> esp_http_client default */
        .keep_alive_enable = (conn != NULL),
        .crt_bundle_attach = o.use_tls_bundle ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client;
    if (reused) {
        client = conn->client;
        esp_http_client_set_url(client, url);
    } else {
        client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "%s: failed to create HTTP client", what);
            return ESP_FAIL;
        }
    }

    /* Reuse the text path's header applier so the CR/LF injection check, the
     * "Name: value" split, and the reused-handle re-arming (method back to GET,
     * stale Content-Type dropped) behave identically for binary fetches. Only
     * extra_header is meaningful here; every other field stays NULL. The scrub
     * first removes whatever extra header the parked handle still carries, so
     * this fetch cannot send a previous request's API key to a different host. */
    scrub_stale_extra_header(client, conn, o.extra_header);
    http_fetch_opts_t hopts = { .extra_header = o.extra_header };
    apply_headers(client, &hopts);

    int status = 0;
    int content_length = 0;
    esp_err_t err = open_and_follow_redirects(client, NULL, o.max_redirects, what,
                                               &status, &content_length,
                                               NULL, NULL, NULL);
    if ((err != ESP_OK || status == -1) && reused) {
        /* Stale/dead keep-alive connection -- destroy and retry once within
         * this call, exactly as the text path's attempt_once() does. */
        ESP_LOGD(TAG, "%s: stale keep-alive -- reconnecting", what);
        esp_http_client_cleanup(client);
        conn->client = NULL;

        client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "%s: failed to create HTTP client", what);
            return ESP_ERR_NO_MEM;
        }
        scrub_stale_extra_header(client, conn, o.extra_header);
        apply_headers(client, &hopts);
        status = 0;
        err = open_and_follow_redirects(client, NULL, o.max_redirects, what,
                                         &status, &content_length,
                                         NULL, NULL, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: HTTP open failed: %s", what, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "%s: HTTP status %d", what, status);
        finish_client(client, conn, false);
        return ESP_FAIL;
    }

    bool chunked = esp_http_client_is_chunked_response(client);
    ESP_LOGI(TAG, "%s: content_length=%d, chunked=%d", what, content_length, (int)chunked);

    /* Size the buffer. A Content-Length above max_size never sizes the
     * allocation: either we take the first max_size bytes (CLAMP) or we refuse
     * the response outright, so a bad or hostile header cannot pick our
     * allocation size for us. */
    size_t needed;
    if (content_length > 0) {
        if ((size_t)content_length > o.max_size) {
            if (o.oversize == HTTP_BIN_OVERSIZE_CLAMP) {
                content_length = (int)o.max_size;
            } else {
                ESP_LOGW(TAG, "%s too large: %d bytes (cap %u)",
                         what, content_length, (unsigned)o.max_size);
                finish_client(client, conn, false);
                return ESP_FAIL;
            }
        }
        needed = (size_t)content_length;
    } else {
        needed = o.unknown_len_size;
    }

    /* Prefer the caller's scratch buffer when the body fits: avoids the
     * malloc/free churn that fragments PSRAM over a long soak. Requires the
     * lock -- an unguarded shared buffer would race between fetches. */
    uint8_t *buf = NULL;
    size_t bufsize = 0;
    bool prealloc_used = false;
    if (o.prealloc && o.prealloc_lock && needed <= o.prealloc_size &&
        xSemaphoreTake(o.prealloc_lock, pdMS_TO_TICKS(o.prealloc_lock_wait_ms)) == pdTRUE) {
        buf = o.prealloc;
        bufsize = o.prealloc_size;
        prealloc_used = true;
    }
    if (!buf) {
        bufsize = needed;
        buf = heap_caps_malloc(bufsize, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "%s: PSRAM alloc failed (%u bytes)", what, (unsigned)bufsize);
            finish_client(client, conn, false);
            return ESP_ERR_NO_MEM;
        }
    }

    size_t total = 0;
    esp_err_t rerr = ESP_OK;
    while (1) {
        if (total >= bufsize) {
            if (o.grow_step == 0) break;   /* fixed-size buffer: full is done */
            if (prealloc_used) {
                ESP_LOGW(TAG, "%s exceeds pre-allocated buffer (%u bytes)",
                         what, (unsigned)bufsize);
                rerr = ESP_FAIL;
                break;
            }
            size_t grown = bufsize + o.grow_step;
            if (grown > o.max_size) {
                ESP_LOGW(TAG, "%s exceeds %u byte cap, aborting fetch",
                         what, (unsigned)o.max_size);
                rerr = ESP_FAIL;
                break;
            }
            uint8_t *nb = heap_caps_realloc(buf, grown, MALLOC_CAP_SPIRAM);
            if (!nb) {
                ESP_LOGE(TAG, "%s: failed to grow buffer to %u", what, (unsigned)grown);
                rerr = ESP_ERR_NO_MEM;
                break;
            }
            buf = nb;
            bufsize = grown;
        }
        int n = esp_http_client_read(client, (char *)buf + total, (int)(bufsize - total));
        if (n <= 0) break;
        total += (size_t)n;
    }

    if (rerr == ESP_OK && o.probe_overflow && content_length <= 0 && total == bufsize) {
        /* Sized to the cap with no Content-Length: a buffer that filled exactly
         * may be a truncated image. If one more byte is available the source
         * was over cap -- reject rather than hand a partial JPEG to a decoder. */
        char probe;
        if (esp_http_client_read(client, &probe, 1) > 0) {
            ESP_LOGW(TAG, "%s exceeds %u byte cap (no content-length) -- rejecting",
                     what, (unsigned)o.max_size);
            rerr = ESP_FAIL;
        }
    }

    /* Drained: no failure so far AND the client consumed the full body (for a
     * CLAMPed oversize response the client's internal position is short of the
     * real Content-Length, so this correctly reads false; likewise after the
     * probe_overflow byte, rerr is already ESP_FAIL). The empty/short-body
     * checks below fail the FETCH but not the drain -- an empty 200 left the
     * socket position-synced, so parking it open stays correct. */
    finish_client(client, conn,
                  rerr == ESP_OK && esp_http_client_is_complete_data_received(client));

    if (rerr == ESP_OK && total == 0) {
        ESP_LOGW(TAG, "%s: empty response", what);
        rerr = ESP_FAIL;
    }
    if (rerr == ESP_OK && o.require_full_length && !chunked &&
        content_length > 0 && total < (size_t)content_length) {
        ESP_LOGE(TAG, "%s: incomplete read: %u/%d", what, (unsigned)total, content_length);
        rerr = ESP_FAIL;
    }

    if (rerr != ESP_OK) {
        if (prealloc_used) {
            xSemaphoreGive(o.prealloc_lock);
        } else {
            heap_caps_free(buf);
        }
        return rerr;
    }

    if (prealloc_used) {
        /* Copy out to a right-sized allocation so the scratch buffer (and its
         * lock) are released before returning: the caller owns plain PSRAM
         * either way and never sees the lock. */
        uint8_t *result = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
        if (!result) {
            ESP_LOGE(TAG, "%s: failed to allocate %u bytes for result",
                     what, (unsigned)total);
            xSemaphoreGive(o.prealloc_lock);
            return ESP_ERR_NO_MEM;
        }
        memcpy(result, buf, total);
        xSemaphoreGive(o.prealloc_lock);
        buf = result;
    } else if (o.shrink_to_fit && total < bufsize) {
        uint8_t *shrunk = heap_caps_realloc(buf, total, MALLOC_CAP_SPIRAM);
        if (shrunk) buf = shrunk;   /* on failure keep the oversized buffer -- still valid */
    }

    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

http_fetch_conn_t *http_fetch_conn_create(void) {
    return heap_caps_calloc(1, sizeof(http_fetch_conn_t), MALLOC_CAP_SPIRAM);
}

void http_fetch_conn_destroy(http_fetch_conn_t *conn) {
    if (!conn) return;
    if (conn->client) {
        esp_http_client_cleanup(conn->client);
    }
    heap_caps_free(conn);
}
