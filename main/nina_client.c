/**
 * @file nina_client.c
 * @brief NINA Client - Public polling API and HTTP utilities
 * @version 3.0
 *
 * Provides the tiered polling orchestrator, HTTP helpers shared by
 * nina_api_fetchers.c and nina_sequence.c, and the prepared-image fetch.
 */

#include "nina_client.h"
#include "nina_client_internal.h"
#include "nina_api_fetchers.h"
#include "nina_sequence.h"
#include "nina_websocket.h"
#include "nina_connection.h"
#include "http_fetch.h"
#include "time_parse.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include "perf_monitor.h"

static const char *TAG = "nina_client";

// =============================================================================
// Per-task HTTP poll context registry (replaces FreeRTOS TLS to avoid
// index conflicts with LWIP/pthread which both claim TLS index 0)
// =============================================================================

#include "app_config.h"  // MAX_NINA_INSTANCES

typedef struct {
    TaskHandle_t      task;
    http_poll_ctx_t  *ctx;
} poll_ctx_slot_t;

/* One slot per poll task + one spare for safety */
static poll_ctx_slot_t s_poll_ctx_registry[MAX_NINA_INSTANCES + 1];

void http_poll_ctx_set(http_poll_ctx_t *ctx) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (ctx) {
        /* Register: find empty slot or overwrite own entry */
        for (int i = 0; i < MAX_NINA_INSTANCES + 1; i++) {
            if (s_poll_ctx_registry[i].task == self || s_poll_ctx_registry[i].task == NULL) {
                s_poll_ctx_registry[i].task = self;
                s_poll_ctx_registry[i].ctx  = ctx;
                return;
            }
        }
    } else {
        /* Unregister */
        for (int i = 0; i < MAX_NINA_INSTANCES + 1; i++) {
            if (s_poll_ctx_registry[i].task == self) {
                s_poll_ctx_registry[i].task = NULL;
                s_poll_ctx_registry[i].ctx  = NULL;
                return;
            }
        }
    }
}

http_poll_ctx_t *http_poll_ctx_get(void) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < MAX_NINA_INSTANCES + 1; i++) {
        if (s_poll_ctx_registry[i].task == self)
            return s_poll_ctx_registry[i].ctx;
    }
    return NULL;
}

// =============================================================================
// DNS Cache Mutex (for concurrent poll tasks)
// =============================================================================

static SemaphoreHandle_t s_dns_mutex = NULL;

#define HTTP_MAX_ATTEMPTS    2      // Total attempts: 1 initial + 1 retry
#define HTTP_RETRY_DELAY_MS  500    // Flat delay before the retry
#define HTTP_JSON_MAX_SIZE (1024 * 1024)  // 1 MB cap for JSON API responses

// =============================================================================
// Mutex Helpers
// =============================================================================

void nina_client_init_mutex(nina_client_t *client) {
    if (client && !client->mutex) {
        client->mutex = xSemaphoreCreateMutex();
    }
}

bool nina_client_lock(nina_client_t *client, uint32_t timeout_ms) {
    if (!client || !client->mutex) return false;
    return xSemaphoreTake(client->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void nina_client_unlock(nina_client_t *client) {
    if (client && client->mutex) {
        xSemaphoreGive(client->mutex);
    }
}

// =============================================================================
// Shared HTTP Helper Functions (exposed via nina_client_internal.h)
// =============================================================================

static time_t my_timegm(struct tm *tm) {
    int64_t t = 0;
    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon; // 0-11

    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    for (int y = 1970; y < year; y++) {
        t += 365 * 24 * 3600;
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
            t += 24 * 3600;
        }
    }

    for (int m = 0; m < mon; m++) {
        t += days_per_month[m] * 24 * 3600;
        if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
            t += 24 * 3600;
        }
    }

    t += (tm->tm_mday - 1) * 24 * 3600;
    t += tm->tm_hour * 3600;
    t += tm->tm_min * 60;
    t += tm->tm_sec;

    return (time_t)t;
}

time_t parse_iso8601(const char *str) {
    if (!str) return 0;

    struct tm tm = {0};
    int tz_hours = 0, tz_mins = 0;
    char tz_sign = '+';

    int parsed = sscanf(str, "%d-%d-%dT%d:%d:%d.%*d%c%d:%d",
        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
        &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
        &tz_sign, &tz_hours, &tz_mins);

    if (parsed < 6) {
        parsed = sscanf(str, "%d-%d-%dT%d:%d:%d%c%d:%d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
            &tz_sign, &tz_hours, &tz_mins);
    }

    if (parsed >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;

        time_t utc_time = my_timegm(&tm);

        if (parsed >= 9) {
            int tz_offset_seconds = (tz_hours * 3600 + tz_mins * 60);
            if (tz_sign == '-') {
                utc_time += tz_offset_seconds;
            } else {
                utc_time -= tz_offset_seconds;
            }
        }

        return utc_time;
    }

    return 0;
}

/* ── Per-attempt perf instrumentation bridge for http_get_json() ──
 * http_fetch.c (main/http_fetch.h/.c) is deliberately protocol- and
 * metrics-agnostic, so it reports diagnostics per attempt via an optional
 * callback instead of calling into perf_monitor directly. This adapter
 * reconstructs the exact connect/TTFB/body timing and retry/failure
 * counters the old hand-rolled loop recorded inline. */
typedef struct {
    bool ever_connected;   /* true if ANY attempt's esp_http_client_open() succeeded */
    bool succeeded_late;   /* true if the attempt that finally produced the result was > 0 */
} http_get_json_perf_ctx_t;

static void http_get_json_on_attempt(const http_fetch_attempt_info_t *info, void *hook_ctx) {
    http_get_json_perf_ctx_t *pctx = (http_get_json_perf_ctx_t *)hook_ctx;

    if (info->attempt_index > 0) {
        perf_counter_increment(&g_perf.http_retry_count);
    }

    /* http_connect times esp_http_client_open() itself, so it's meaningful
     * whether or not the open succeeded (matches the original timer, which
     * wrapped open() unconditionally). http_ttfb (fetch_headers) and
     * http_body (read loop) are only ever attempted once open succeeded. */
    perf_timer_record(&g_perf.http_connect, info->connect_us);
    if (info->ever_connected) {
        pctx->ever_connected = true;
        perf_timer_record(&g_perf.http_ttfb, info->headers_us);
    }
    if (info->body_us > 0) {
        perf_timer_record(&g_perf.http_body, info->body_us);
    }

    if (info->ok && info->attempt_index > 0) {
        pctx->succeeded_late = true;
    }
}

cJSON *http_get_json_dated(const char *url, int64_t *date_epoch_out) {
    if (date_epoch_out) *date_epoch_out = 0;

    /* Read per-task HTTP context (set by poll tasks) for keep-alive reuse via
     * the shared fetcher (main/http_fetch.h). If no context is registered,
     * or its conn slot is unset, http_fetch treats a NULL conn as a one-shot
     * client (no reuse, no keep-alive) -- same fallback as before. */
    http_poll_ctx_t *tls_ctx = http_poll_ctx_get();
    http_fetch_conn_t *reuse_conn = tls_ctx ? tls_ctx->conn : NULL;

    /* ── mDNS-bypass URL rewrite ──
     * NINA hostnames are typically .lan (mDNS), and esp_http_client does a fresh
     * getaddrinfo() on every connect — that DNS+connect is ~42% of avg HTTP latency
     * and the multi-second tail. Resolve the host ONCE via the app DNS cache, rebuild
     * the request URL with the numeric IP (so connect skips getaddrinfo), and send the
     * ORIGINAL hostname in the Host header so NINA's HTTP routing is unaffected.
     * On a resolve miss we fall through to the original hostname URL (current
     * behavior) — a resolve miss never breaks a request. */
    const char *req_url = url;          /* URL actually handed to esp_http_client */
    const char *host_hdr = NULL;        /* original hostname for the Host header, or NULL */
    char rewritten_url[288];
    char host_buf[128];
    {
        const char *scheme_end = strstr(url, "://");
        if (scheme_end) {
            const char *hstart = scheme_end + 3;
            const char *hend = hstart;
            while (*hend && *hend != ':' && *hend != '/') hend++;
            size_t hlen = (size_t)(hend - hstart);
            /* Only rewrite non-numeric hosts (a numeric host is already DNS-free). */
            if (hlen > 0 && hlen < sizeof(host_buf) &&
                !(hstart[0] >= '0' && hstart[0] <= '9')) {
                memcpy(host_buf, hstart, hlen);
                host_buf[hlen] = '\0';
                /* Always carry the original hostname in Host (even on resolve
                 * miss): a reused handle may have an explicit Host set from a
                 * prior request, so we must re-assert the correct one every time
                 * rather than leave a stale value behind. */
                host_hdr = host_buf;
                char ip_str[16];  /* INET_ADDRSTRLEN for IPv4 */
                if (nina_client_resolve_host(host_buf, ip_str, sizeof(ip_str)) &&
                    ip_str[0] != '\0') {
                    /* Rebuild: <scheme://><ip><:port/path...>  (hend points at ':' or '/') */
                    int n = snprintf(rewritten_url, sizeof(rewritten_url),
                                     "%.*s%s%s",
                                     (int)(hstart - url), url, ip_str, hend);
                    if (n > 0 && n < (int)sizeof(rewritten_url)) {
                        req_url = rewritten_url;  /* connect to IP, skip getaddrinfo */
                    }
                    /* else: rebuild overflowed — fall back to hostname URL, Host
                     * still set to the same hostname (harmless, self-consistent). */
                }
            }
        }
    }

    perf_timer_start(&g_perf.http_request);
    perf_counter_increment(&g_perf.http_request_count);

    /* Optional response-Date capture (NINA-PC clock domain). RFC-1123 dates
     * are 29 chars ("Mon, 06 Jul 2026 19:06:19 GMT"); 48 leaves headroom. */
    char date_buf[48];
    date_buf[0] = '\0';

    http_get_json_perf_ctx_t pctx = { 0 };
    http_fetch_opts_t opts = {
        .timeout_ms = 3000,
        .max_attempts = HTTP_MAX_ATTEMPTS,
        .retry_delay_ms = HTTP_RETRY_DELAY_MS,
        /* +1 for the NUL terminator: http_fetch's cap check rejects when
         * content_length+1 exceeds it, matching the original's
         * "content_length > HTTP_JSON_MAX_SIZE" boundary (content_length ==
         * HTTP_JSON_MAX_SIZE exactly was allowed). */
        .max_response_bytes = HTTP_JSON_MAX_SIZE + 1,
        .host_header = host_hdr,
        .on_attempt = http_get_json_on_attempt,
        .hook_ctx = &pctx,
        .conn = reuse_conn,
        .capture_header = date_epoch_out ? "Date" : NULL,
        .capture_header_out = date_epoch_out ? date_buf : NULL,
        .capture_header_out_len = date_epoch_out ? sizeof(date_buf) : 0,
    };

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_fetch_text(req_url, &opts, &body, &body_len);

    if (err != ESP_OK) {
        /* Extract host from URL for a clean log message. (http_fetch.c also
         * logs its own generic failure line under the "http_fetch" tag; this
         * host-only line is kept for parity with the previous log format.) */
        const char *host = url;
        const char *scheme_end = strstr(url, "://");
        if (scheme_end) host = scheme_end + 3;
        const char *host_end = strchr(host, ':');
        if (!host_end) host_end = strchr(host, '/');
        int host_len = host_end ? (int)(host_end - host) : (int)strlen(host);
        ESP_LOGW(TAG, "%.*s unreachable", host_len, host);

        if (pctx.ever_connected) {
            perf_counter_increment(&g_perf.http_failure_count);
        } else {
            perf_counter_increment(&g_perf.http_unreachable_count);
        }
        perf_timer_stop(&g_perf.http_request);
        return NULL;  // All attempts exhausted
    }

    if (pctx.succeeded_late) {
        perf_counter_increment(&g_perf.http_attempt0_fail_count);
    }

    /* Parse the captured Date header (empty string when absent). 0 stays in
     * *date_epoch_out on any parse failure. */
    if (date_epoch_out && date_buf[0] != '\0') {
        *date_epoch_out = (int64_t)time_parse_rfc1123(date_buf);
    }

    /* Guard against chunked transfer encoding or missing Content-Length.
     * NINA API always sends Content-Length; an empty body is treated as
     * error, matching the original manual-client behavior. */
    if (body_len == 0) {
        ESP_LOGW(TAG, "No Content-Length for %s (chunked?), skipping", url);
        heap_caps_free(body);
        perf_timer_stop(&g_perf.http_request);
        return NULL;
    }

    perf_timer_start(&g_perf.json_parse);
    cJSON *json = cJSON_Parse(body);
    perf_timer_stop(&g_perf.json_parse);
    perf_counter_increment(&g_perf.json_parse_count);
    heap_caps_free(body);
    perf_timer_stop(&g_perf.http_request);
    return json;
}

/* Thin wrapper — the common no-Date-capture case used by ~17 call sites. */
cJSON *http_get_json(const char *url) {
    return http_get_json_dated(url, NULL);
}

/* Current time in the NINA-PC clock domain. See nina_client.h for the
 * locking contract: nina_clock_epoch/nina_clock_mono_us are written under
 * the client mutex (camera-info fetchers only); callers should hold the
 * mutex or tolerate a rare torn int64 read on RV32 — lock-free UI timers
 * use a cached pair copied under the lock instead. All math is int64
 * (P4 FPU is single-precision; no double). */
int64_t nina_client_now_epoch(const nina_client_t *client) {
    if (client && client->nina_clock_epoch != 0) {
        return client->nina_clock_epoch +
               (esp_timer_get_time() - client->nina_clock_mono_us) / 1000000;
    }
    return (int64_t)time(NULL);
}

/* ── NINA API envelope helpers ──
 * The Advanced API wraps every response in an envelope:
 *   { "Response": ..., "Error": "", "StatusCode": 200, "Success": true, "Type": "API" }
 * http_get_json() already returns NULL on transport failure / non-2xx / empty /
 * partial bodies, so NULL is a reliable "unreachable" signal. These helpers add the
 * application-level Success check so connectivity reflects the CURRENT poll's
 * envelope rather than a latched side-effect. Neither helper deletes the
 * envelope — the caller owns it. */

bool nina_api_envelope_ok(cJSON *envelope) {
    if (!envelope) return false;
    return cJSON_IsTrue(cJSON_GetObjectItem(envelope, "Success"));
}

cJSON *nina_api_response(cJSON *envelope) {
    if (!nina_api_envelope_ok(envelope)) return NULL;
    return cJSON_GetObjectItem(envelope, "Response");
}

// =============================================================================
// Exposure Timing Fixup
// =============================================================================

static void fixup_exposure_timing(nina_client_t *data) {
    if (data->exposure_current < 0) {
        float remaining = -data->exposure_current;
        if (data->exposure_total > 0) {
            data->exposure_current = data->exposure_total - remaining;
        } else {
            data->exposure_current = 0;
        }

        int rem_sec = (int)remaining;
        if (rem_sec < 0) rem_sec = 0;
        snprintf(data->time_remaining, sizeof(data->time_remaining), "%02d:%02d", rem_sec / 60, rem_sec % 60);
    }

    // Clamp
    if (data->exposure_current < 0) data->exposure_current = 0;
    if (data->exposure_total > 0 && data->exposure_current > data->exposure_total) {
        data->exposure_current = data->exposure_total;
    }
}

// =============================================================================
// Public API
// =============================================================================

void nina_client_get_data(const char *base_url, nina_client_t *data) {
    // Initialize
    memset(data, 0, sizeof(nina_client_t));
    data->connected = false;
    strcpy(data->target_name, "No Target");
    strcpy(data->status, "IDLE");
    strcpy(data->meridian_flip, "--");
    strcpy(data->profile_name, "NINA");
    strcpy(data->current_filter, "--");
    strcpy(data->time_remaining, "--");
    data->target_time_remaining[0] = '\0';
    data->target_time_reason[0] = '\0';
    data->filter_count = 0;

    ESP_LOGI(TAG, "=== Fetching NINA data (robust method) ===");

    fetch_camera_info_robust(base_url, data);

    if (data->connected) {
        fetch_filter_robust_ex(base_url, data, true);
        fetch_image_history_robust(base_url, data);
        fetch_profile_robust(base_url, data);
        fetch_guider_robust(base_url, data);
        fetch_mount_robust(base_url, data);
        fetch_focuser_robust(base_url, data);
        fetch_sequence_counts_optional(base_url, data);

        fixup_exposure_timing(data);
    }

    ESP_LOGI(TAG, "=== Data Summary ===");
    ESP_LOGI(TAG, "Connected: %d, Profile: %s", data->connected, data->profile_name);
    ESP_LOGI(TAG, "Target: %s, Filter: %s", data->target_name, data->current_filter);
    ESP_LOGI(TAG, "Exposure: %.1fs (%.1f/%.1f)", data->exposure_total, data->exposure_current, data->exposure_total);
    ESP_LOGI(TAG, "Camera: %.1fC (%.0f%% power)", data->camera.temp, data->camera.cooler_power);
    ESP_LOGI(TAG, "Guiding: %.2f\", HFR: %.2f, Stars: %d", data->guider.rms_total, data->hfr, data->stars);
}

// =============================================================================
// Tiered Polling API
// =============================================================================

void nina_poll_state_init(nina_poll_state_t *state) {
    if (state->http_client) {
        http_fetch_conn_destroy((http_fetch_conn_t *)state->http_client);
    }
    memset(state, 0, sizeof(nina_poll_state_t));
    state->cached_image_count = -1;  // Force initial full fetch
}

void nina_client_poll(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance) {
    // Set per-task HTTP context for client reuse during this poll cycle.
    // Lazily create the persistent keep-alive slot on first use (mirrors the
    // previous lazy esp_http_client_handle_t allocation inside http_get_json).
    // state->http_client holds the http_fetch_conn_t* directly and persists
    // across calls, so — unlike the old reuse_handle round-trip — there is
    // nothing to write back at the end of this function.
    if (!state->http_client) {
        state->http_client = http_fetch_conn_create();
    }
    http_poll_ctx_t poll_ctx = { .conn = (http_fetch_conn_t *)state->http_client };
    http_poll_ctx_set(&poll_ctx);

    int64_t now_ms = esp_timer_get_time() / 1000;

    // No need to pre-clear data->connected here: the fetchers now explicitly set
    // data->connected = false on every failure path (unreachable, Success!=true,
    // missing Response) and = true only on an OK envelope, so connectivity is
    // recomputed fresh each poll. The WebSocket handlers do NOT write data->connected
    // (they only write websocket_connected / rotator_connected / safety_connected),
    // so there is no struct race to worry about. The connection state machine below
    // adds hysteresis on top of this per-poll value.

    ESP_LOGI(TAG, "=== Polling NINA data (%s: %s) ===",
             state->bundle_not_available ? "tiered" : "bundled",
             state->bundle_not_available ? "equipment/*/info" : "equipment/info");

    // --- BUNDLED: All equipment in one request (ninaAPI 2.2.15+) ---
    if (!state->bundle_not_available) {
        perf_timer_start(&g_perf.poll_equipment_bundle);
        uint16_t eq_mask = 0;
        int bundle_result = fetch_equipment_info_bundled(base_url, data, !state->static_fetched, &eq_mask);
        perf_timer_stop(&g_perf.poll_equipment_bundle);

        if (bundle_result == 0) {
            // Seed WebSocket equipment mask from actual NINA connected state
            nina_websocket_update_equipment_mask(instance, eq_mask);
        } else if (bundle_result == -2) {
            // Endpoint not available (old ninaAPI) — fall back to individual fetchers
            ESP_LOGW(TAG, "/equipment/info not available, falling back to individual fetchers");
            state->bundle_not_available = true;
            // Fall through to legacy path below
        }
        // bundle_result == -1: API unreachable or Success!=true — the fetcher already
        // set data->connected = false, so the connection check below sees a failed poll.
    }

    if (state->bundle_not_available) {
        // --- LEGACY: Individual equipment fetchers (old ninaAPI without /equipment/info) ---
        perf_timer_start(&g_perf.poll_camera);
        fetch_camera_info_robust(base_url, data);
        perf_timer_stop(&g_perf.poll_camera);
    }

    // --- Connection check (both bundled and legacy paths) ---
    nina_conn_state_t conn_state = nina_connection_report_poll(instance, data->connected);
    if (nina_client_lock(data, 100)) {
        data->connected = (conn_state == NINA_CONN_CONNECTED);
        nina_client_unlock(data);
    }

    if (!data->connected) {
        state->static_fetched = false;
        state->cached_image_count = -1;
        nina_connection_set_static_data_ready(instance, false);
        if (nina_client_lock(data, 100)) {
            data->prev_target_container[0] = '\0';
            nina_client_unlock(data);
        }
        http_poll_ctx_set(NULL);
        return;
    }

    // --- Handle PROFILE-CHANGED: invalidate cached static data ---
    if (atomic_exchange(&data->profile_refresh_needed, false)) {
        state->static_fetched = false;
        state->cached_image_count = -1;
        nina_connection_set_static_data_ready(instance, false);
        ESP_LOGI(TAG, "Profile changed — re-fetching static data for instance %d", instance);
    }

    // --- ONCE: Static data (profile, image history; filters/switch/safety handled by bundle) ---
    if (!state->static_fetched) {
        perf_timer_start(&g_perf.poll_profile);
        fetch_profile_robust(base_url, data);
        perf_timer_stop(&g_perf.poll_profile);

        if (state->bundle_not_available) {
            // Legacy: fetch equipment data individually on first connect
            perf_timer_start(&g_perf.poll_filter);
            fetch_filter_robust_ex(base_url, data, true);
            perf_timer_stop(&g_perf.poll_filter);

            perf_timer_start(&g_perf.poll_switch);
            fetch_switch_info(base_url, data);
            perf_timer_stop(&g_perf.poll_switch);

            fetch_safety_monitor_info(base_url, data);
        }

        perf_timer_start(&g_perf.poll_image_history);
        fetch_image_history_robust(base_url, data);
        perf_timer_stop(&g_perf.poll_image_history);

        snprintf(state->cached_profile, sizeof(state->cached_profile), "%s", data->profile_name);
        snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
        memcpy(state->cached_filters, data->filters, sizeof(state->cached_filters));
        state->cached_filter_count = data->filter_count;

        state->static_fetched = true;
        nina_connection_set_static_data_ready(instance, true);
        state->last_slow_poll_ms = now_ms;
        state->last_sequence_poll_ms = now_ms;
        /* Force a sequence poll this same cycle so exposure_total reflects the
         * real current-sub length (not the stale image-history total) before the
         * UI seeds the exposure arc on first connect. */
        data->sequence_poll_needed = true;
    } else if (nina_client_lock(data, 100)) {
        snprintf(data->profile_name, sizeof(data->profile_name), "%s", state->cached_profile);
        if (state->cached_telescope[0] != '\0') {
            snprintf(data->telescope_name, sizeof(data->telescope_name), "%s", state->cached_telescope);
        }
        if (state->bundle_not_available) {
            // Legacy: restore cached filters
            memcpy(data->filters, state->cached_filters, sizeof(data->filters));
            data->filter_count = state->cached_filter_count;
        } else {
            // Bundled: filters come fresh every cycle, update cache
            memcpy(state->cached_filters, data->filters, sizeof(state->cached_filters));
            state->cached_filter_count = data->filter_count;
        }
        nina_client_unlock(data);
    }

    if (state->bundle_not_available) {
        // --- LEGACY: Fast + conditional + slow tier fetchers ---
        perf_timer_start(&g_perf.poll_guider);
        fetch_guider_robust(base_url, data);
        perf_timer_stop(&g_perf.poll_guider);

        if (!data->websocket_connected) {
            /* Image count gate: only fetch full image-history if the count changed.
             * Saves ~95% of fetches during long exposures (30-600s between changes). */
            int img_count = fetch_image_count(base_url);
            if (img_count >= 0 && img_count != state->cached_image_count) {
                state->cached_image_count = img_count;
                perf_timer_start(&g_perf.poll_image_history);
                fetch_image_history_robust(base_url, data);
                perf_timer_stop(&g_perf.poll_image_history);
            }
            if (data->telescope_name[0] != '\0') {
                snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
            }
            perf_timer_start(&g_perf.poll_filter);
            fetch_filter_robust_ex(base_url, data, false);
            perf_timer_stop(&g_perf.poll_filter);
        } else {
            if (data->telescope_name[0] != '\0') {
                snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
            }
        }

        if (now_ms - state->last_slow_poll_ms >= NINA_POLL_SLOW_MS) {
            perf_timer_start(&g_perf.poll_focuser);
            fetch_focuser_robust(base_url, data);
            perf_timer_stop(&g_perf.poll_focuser);

            perf_timer_start(&g_perf.poll_mount);
            fetch_mount_robust(base_url, data);
            perf_timer_stop(&g_perf.poll_mount);

            perf_timer_start(&g_perf.poll_switch);
            fetch_switch_info(base_url, data);
            perf_timer_stop(&g_perf.poll_switch);

            state->last_slow_poll_ms = now_ms;
        }
    } else {
        // --- BUNDLED: Only image history needs separate fetch (not in bundle) ---
        if (!data->websocket_connected) {
            /* Image count gate: lightweight count check (~50 bytes) before
             * full image-history fetch (~400-800 bytes). */
            int img_count = fetch_image_count(base_url);
            if (img_count >= 0 && img_count != state->cached_image_count) {
                state->cached_image_count = img_count;
                perf_timer_start(&g_perf.poll_image_history);
                fetch_image_history_robust(base_url, data);
                perf_timer_stop(&g_perf.poll_image_history);
            }
        }
        if (data->telescope_name[0] != '\0') {
            snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
        }
    }

    // --- SEQUENCE: Timer-based + event-driven polling ---
    bool sequence_event = atomic_exchange(&data->sequence_poll_needed, false);
    bool sequence_due = (now_ms - state->last_sequence_poll_ms >= NINA_POLL_SEQUENCE_MS);
    if (sequence_due || sequence_event) {
        if (sequence_event) {
            ESP_LOGD(TAG, "Event-driven sequence poll triggered");
        }
        perf_timer_start(&g_perf.poll_sequence);
        fetch_sequence_counts_optional(base_url, data);
        perf_timer_stop(&g_perf.poll_sequence);
        state->last_sequence_poll_ms = now_ms;
    }

    if (nina_client_lock(data, 100)) {
        fixup_exposure_timing(data);
        nina_client_unlock(data);
    }

    http_poll_ctx_set(NULL);

    ESP_LOGI(TAG, "=== Poll Summary ===");
    ESP_LOGI(TAG, "Connected: %d, Profile: %s", data->connected, data->profile_name);
    ESP_LOGI(TAG, "Target: %s, Filter: %s", data->target_name, data->current_filter);
    ESP_LOGI(TAG, "Exposure: %.1fs (%.1f/%.1f)", data->exposure_total, data->exposure_current, data->exposure_total);
    ESP_LOGI(TAG, "Guiding: %.2f\", HFR: %.2f, Stars: %d", data->guider.rms_total, data->hfr, data->stars);
}

void nina_client_poll_heartbeat(const char *base_url, nina_client_t *data, int instance) {
    fetch_camera_info_robust(base_url, data);

    nina_conn_state_t conn_state = nina_connection_report_poll(instance, data->connected);
    if (nina_client_lock(data, 100)) {
        data->connected = (conn_state == NINA_CONN_CONNECTED);
        nina_client_unlock(data);
    }

    ESP_LOGD(TAG, "Heartbeat: connected=%d", data->connected);
}

void nina_client_poll_background(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance) {
    // Set per-task HTTP context for client reuse during this poll cycle.
    // Lazily create the persistent keep-alive slot on first use; state->http_client
    // holds the http_fetch_conn_t* directly and persists across calls (see
    // nina_client_poll() above for the full rationale).
    if (!state->http_client) {
        state->http_client = http_fetch_conn_create();
    }
    http_poll_ctx_t poll_ctx = { .conn = (http_fetch_conn_t *)state->http_client };
    http_poll_ctx_set(&poll_ctx);

    int64_t now_ms = esp_timer_get_time() / 1000;

    // Mark disconnected before fetch — fetchers set connected=true on success.
    if (nina_client_lock(data, 100)) {
        data->connected = false;
        nina_client_unlock(data);
    }

    // --- Equipment fetch ---
    // Use the full bundle only on first connect (to seed static data: filters, switch, etc.).
    // Subsequent background polls use camera-only heartbeat (~1-2 KB vs ~10 KB bundle),
    // reducing network traffic by ~80% for background instances.
    if (!state->static_fetched && !state->bundle_not_available) {
        int bundle_result = fetch_equipment_info_bundled(base_url, data, true, NULL);
        if (bundle_result == -2) {
            ESP_LOGW(TAG, "Background: /equipment/info not available, falling back");
            state->bundle_not_available = true;
            fetch_camera_info_robust(base_url, data);
        }
    } else {
        // Camera-only heartbeat for connection status
        fetch_camera_info_robust(base_url, data);
    }

    nina_conn_state_t conn_state = nina_connection_report_poll(instance, data->connected);
    if (nina_client_lock(data, 100)) {
        data->connected = (conn_state == NINA_CONN_CONNECTED);
        nina_client_unlock(data);
    }

    if (!data->connected) {
        state->static_fetched = false;
        state->cached_image_count = -1;
        nina_connection_set_static_data_ready(instance, false);
        http_poll_ctx_set(NULL);
        return;
    }

    // --- Handle PROFILE-CHANGED: invalidate cached static data ---
    if (atomic_exchange(&data->profile_refresh_needed, false)) {
        state->static_fetched = false;
        state->cached_image_count = -1;
        nina_connection_set_static_data_ready(instance, false);
        ESP_LOGI(TAG, "Profile changed — re-fetching static data for background instance %d", instance);
    }

    // --- ONCE: Static data (profile, image history; filters/switch/safety from bundle) ---
    if (!state->static_fetched) {
        fetch_profile_robust(base_url, data);

        if (state->bundle_not_available) {
            // Legacy: fetch equipment data individually
            fetch_filter_robust_ex(base_url, data, true);
            fetch_switch_info(base_url, data);
            fetch_safety_monitor_info(base_url, data);
        }

        fetch_image_history_robust(base_url, data);

        snprintf(state->cached_profile, sizeof(state->cached_profile), "%s", data->profile_name);
        snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
        memcpy(state->cached_filters, data->filters, sizeof(state->cached_filters));
        state->cached_filter_count = data->filter_count;

        state->static_fetched = true;
        nina_connection_set_static_data_ready(instance, true);
        state->last_slow_poll_ms = now_ms;
        state->last_sequence_poll_ms = now_ms;
    } else if (nina_client_lock(data, 100)) {
        snprintf(data->profile_name, sizeof(data->profile_name), "%s", state->cached_profile);
        if (state->cached_telescope[0] != '\0') {
            snprintf(data->telescope_name, sizeof(data->telescope_name), "%s", state->cached_telescope);
        }
        if (state->bundle_not_available) {
            memcpy(data->filters, state->cached_filters, sizeof(data->filters));
            data->filter_count = state->cached_filter_count;
        } else {
            // Bundled: filters come fresh, update cache
            memcpy(state->cached_filters, data->filters, sizeof(state->cached_filters));
            state->cached_filter_count = data->filter_count;
        }
        nina_client_unlock(data);
    }

    // --- SLOW: Focuser + Mount + Switch (every 30s, legacy only — bundle handles these) ---
    if (state->bundle_not_available && now_ms - state->last_slow_poll_ms >= NINA_POLL_SLOW_MS) {
        fetch_focuser_robust(base_url, data);
        fetch_mount_robust(base_url, data);
        fetch_switch_info(base_url, data);
        state->last_slow_poll_ms = now_ms;
    }

    // --- SEQUENCE: Timer-based polling (background instances) ---
    if (now_ms - state->last_sequence_poll_ms >= NINA_POLL_SEQUENCE_MS) {
        fetch_sequence_counts_optional(base_url, data);
        state->last_sequence_poll_ms = now_ms;
    }

    http_poll_ctx_set(NULL);

    ESP_LOGD(TAG, "Background poll: connected=%d, profile=%s, target=%s",
             data->connected, data->profile_name, data->target_name);
}

// =============================================================================
// DNS Pre-check with caching
// =============================================================================

#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define DNS_CACHE_TTL_MS 60000  // 60-second TTL

typedef struct {
    char hostname[128];
    char resolved_ip[46];  // INET6_ADDRSTRLEN
    int64_t resolve_time_ms;
} dns_cache_entry_t;

static dns_cache_entry_t s_dns_cache[3];  // One per instance (MAX_NINA_INSTANCES)

void nina_client_init(void) {
    if (!s_dns_mutex) {
        s_dns_mutex = xSemaphoreCreateMutex();
    }
}

/* Resolve a hostname to a dotted-quad IPv4 string via the app-level DNS cache.
 * Returns true and fills ip_out on success (cache hit, fresh lookup, or stale
 * fallback); false (ip_out left empty) only when resolution fails with no cache.
 * Shared by nina_client_dns_check() (connectivity pre-check) and http_get_json()
 * (per-request URL rewrite to skip per-connect getaddrinfo on .lan hosts). */
bool nina_client_resolve_host(const char *host, char *ip_out, size_t ip_len) {
    if (!host || !ip_out || ip_len == 0) return false;
    ip_out[0] = '\0';

    size_t host_len = strlen(host);
    if (host_len == 0 || host_len >= 128) return false;

    // Numeric IPv4 host — return verbatim, no DNS needed.
    if (host[0] >= '0' && host[0] <= '9') {
        if (host_len >= ip_len) return false;
        memcpy(ip_out, host, host_len + 1);
        return true;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    // Check DNS cache for a valid (non-expired) entry. On a failed take, skip the
    // cache entirely and fall through to a fresh lookup — never give a mutex we
    // do not hold, and never touch the cache unlocked.
    bool dns_locked = (s_dns_mutex && xSemaphoreTake(s_dns_mutex, pdMS_TO_TICKS(5000)) == pdTRUE);
    if (dns_locked) {
        for (int i = 0; i < 3; i++) {
            if (strcmp(s_dns_cache[i].hostname, host) == 0 &&
                s_dns_cache[i].resolved_ip[0] != '\0') {
                if (now_ms - s_dns_cache[i].resolve_time_ms < DNS_CACHE_TTL_MS) {
                    snprintf(ip_out, ip_len, "%s", s_dns_cache[i].resolved_ip);
                    ESP_LOGD(TAG, "DNS cache hit for %s -> %s", host, ip_out);
                    xSemaphoreGive(s_dns_mutex);
                    return true;
                }
                break;  // Found entry but expired — do fresh lookup
            }
        }
        xSemaphoreGive(s_dns_mutex);
    }

    // Cache miss or expired — perform real DNS lookup (outside mutex — can block)
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    struct addrinfo *result = NULL;
    int err = getaddrinfo(host, NULL, &hints, &result);

    if (err == 0 && result) {
        // Lookup succeeded — update cache under mutex
        char ip_str[46] = {0};
        struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
        inet_ntoa_r(addr->sin_addr, ip_str, sizeof(ip_str));
        freeaddrinfo(result);

        // The resolved IP is in a local; return it regardless of the cache lock.
        // Only update the shared cache when the take succeeded.
        dns_locked = (s_dns_mutex && xSemaphoreTake(s_dns_mutex, pdMS_TO_TICKS(5000)) == pdTRUE);
        if (dns_locked) {
            // Find existing slot or first empty slot
            int slot = -1;
            for (int i = 0; i < 3; i++) {
                if (strcmp(s_dns_cache[i].hostname, host) == 0 ||
                    s_dns_cache[i].hostname[0] == '\0') {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) slot = 0;  // Evict first slot if all full

            snprintf(s_dns_cache[slot].hostname, sizeof(s_dns_cache[slot].hostname), "%s", host);
            snprintf(s_dns_cache[slot].resolved_ip, sizeof(s_dns_cache[slot].resolved_ip), "%s", ip_str);
            s_dns_cache[slot].resolve_time_ms = now_ms;

            xSemaphoreGive(s_dns_mutex);
        }

        snprintf(ip_out, ip_len, "%s", ip_str);

        ESP_LOGD(TAG, "DNS cached %s -> %s", host, ip_str);
        return true;
    }

    if (result) freeaddrinfo(result);

    // Lookup failed — try stale cache as fallback (only if the take succeeds;
    // a failed take skips the stale-cache path and returns failure).
    dns_locked = (s_dns_mutex && xSemaphoreTake(s_dns_mutex, pdMS_TO_TICKS(5000)) == pdTRUE);
    if (dns_locked) {
        for (int i = 0; i < 3; i++) {
            if (strcmp(s_dns_cache[i].hostname, host) == 0 &&
                s_dns_cache[i].resolved_ip[0] != '\0') {
                snprintf(ip_out, ip_len, "%s", s_dns_cache[i].resolved_ip);
                ESP_LOGW(TAG, "DNS lookup failed for %s, using stale cache -> %s",
                         host, ip_out);
                xSemaphoreGive(s_dns_mutex);
                return true;
            }
        }
        xSemaphoreGive(s_dns_mutex);
    }

    ESP_LOGD(TAG, "DNS check failed for %s (err %d), no cache available", host, err);
    return false;
}

bool nina_client_dns_check(const char *base_url) {
    if (!base_url) return false;

    // Extract hostname from URL: "http://hostname:port/path..."
    const char *host_start = strstr(base_url, "://");
    if (!host_start) return false;
    host_start += 3;

    // Find end of hostname (at ':' for port or '/' for path)
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;

    int host_len = host_end - host_start;
    if (host_len <= 0 || host_len >= 128) return false;

    char hostname[128];
    memcpy(hostname, host_start, host_len);
    hostname[host_len] = '\0';

    char ip_str[46];
    return nina_client_resolve_host(hostname, ip_str, sizeof(ip_str));
}

#define MAX_IMAGE_SIZE (4 * 1024 * 1024)  // 4 MB cap for image downloads

/* Pre-allocated PSRAM buffer for image fetching — avoids repeated malloc/free
 * that fragments PSRAM during long soak sessions.  Only one thumbnail fetch
 * can be in-flight at a time (serialized by mutex). */
#define IMAGE_FETCH_BUF_SIZE  (1024 * 1024)  // 1 MB pre-allocated fetch buffer

static uint8_t *s_image_fetch_buf = NULL;
static SemaphoreHandle_t s_image_mutex = NULL;

void nina_client_init_image_buffers(void) {
    s_image_fetch_buf = heap_caps_malloc(IMAGE_FETCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_image_mutex = xSemaphoreCreateMutex();
    if (!s_image_fetch_buf || !s_image_mutex) {
        ESP_LOGE(TAG, "Failed to pre-allocate image fetch buffer");
    } else {
        ESP_LOGI(TAG, "Image fetch buffer pre-allocated: %dKB", IMAGE_FETCH_BUF_SIZE / 1024);
    }
}

uint8_t *nina_client_fetch_prepared_image(const char *base_url, int width, int height, int quality, size_t *out_size) {
    char url[320];
    snprintf(url, sizeof(url),
        "%sprepared-image?resize=true&size=%dx%d&quality=%d&autoPrepare=true",
        base_url, width, height, quality);

    ESP_LOGI(TAG, "Fetching prepared image: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for image");
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "Image request failed with HTTP %d", status);
        esp_http_client_cleanup(client);
        return NULL;
    }

    bool chunked = esp_http_client_is_chunked_response(client);
    ESP_LOGI(TAG, "Image response: content_length=%d, chunked=%d", content_length, chunked);

    // Use pre-allocated buffer when available to avoid PSRAM fragmentation
    bool using_static = false;
    uint8_t *buffer = NULL;
    int buf_size = 0;

    if (s_image_fetch_buf && s_image_mutex &&
        xSemaphoreTake(s_image_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int needed = (content_length > 0) ? content_length : (256 * 1024);
        if (needed <= IMAGE_FETCH_BUF_SIZE) {
            buffer = s_image_fetch_buf;
            buf_size = IMAGE_FETCH_BUF_SIZE;
            using_static = true;
        } else {
            xSemaphoreGive(s_image_mutex);
        }
    }

    if (!buffer) {
        // Fallback to dynamic allocation
        buf_size = (content_length > 0) ? content_length : (256 * 1024);
        buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for image", buf_size);
            esp_http_client_cleanup(client);
            return NULL;
        }
    }

    int total_read = 0, read_len;
    while (1) {
        int to_read = buf_size - total_read;
        if (to_read <= 0) {
            if (using_static) {
                // Static buffer full — can't grow it, abort
                ESP_LOGW(TAG, "Image exceeds pre-allocated buffer (%d bytes)", buf_size);
                xSemaphoreGive(s_image_mutex);
                esp_http_client_cleanup(client);
                return NULL;
            }
            int new_size = buf_size + (256 * 1024);
            if (new_size > MAX_IMAGE_SIZE) {
                ESP_LOGW(TAG, "Image exceeds %d byte cap, aborting fetch", MAX_IMAGE_SIZE);
                free(buffer);
                esp_http_client_cleanup(client);
                return NULL;
            }
            uint8_t *new_buf = heap_caps_realloc(buffer, new_size, MALLOC_CAP_SPIRAM);
            if (!new_buf) {
                ESP_LOGE(TAG, "Failed to grow image buffer to %d", new_size);
                free(buffer);
                esp_http_client_cleanup(client);
                return NULL;
            }
            buffer = new_buf;
            buf_size = new_size;
            to_read = buf_size - total_read;
        }
        read_len = esp_http_client_read(client, (char *)buffer + total_read, to_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }

    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGE(TAG, "No image data received");
        if (!using_static) free(buffer);
        else xSemaphoreGive(s_image_mutex);
        return NULL;
    }

    if (!chunked && total_read < content_length) {
        ESP_LOGE(TAG, "Incomplete image read: %d/%d", total_read, content_length);
        if (!using_static) free(buffer);
        else xSemaphoreGive(s_image_mutex);
        return NULL;
    }

    // If using static buffer, copy to a right-sized allocation for the caller
    uint8_t *result = buffer;
    if (using_static) {
        result = heap_caps_malloc(total_read, MALLOC_CAP_SPIRAM);
        if (!result) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for image result", total_read);
            xSemaphoreGive(s_image_mutex);
            return NULL;
        }
        memcpy(result, s_image_fetch_buf, total_read);
        xSemaphoreGive(s_image_mutex);
    }

    *out_size = (size_t)total_read;
    ESP_LOGI(TAG, "Image fetched: %d bytes", total_read);
    return result;
}
