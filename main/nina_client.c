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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
// HTTP Client Reuse State (file-scoped, single-threaded access from data task)
// =============================================================================

static bool s_poll_mode = false;
static esp_http_client_handle_t s_reuse_client = NULL;

// Retry delays between attempts (ms). Index 0 = delay before 2nd attempt, etc.
static const int http_retry_delays_ms[] = {500};
#define HTTP_MAX_ATTEMPTS  2

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
    long t = 0;
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

cJSON *http_get_json(const char *url) {
    perf_timer_start(&g_perf.http_request);
    perf_counter_increment(&g_perf.http_request_count);
    for (int attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            perf_counter_increment(&g_perf.http_retry_count);
            ESP_LOGD(TAG, "HTTP retry %d/%d for %s (delay %d ms)",
                     attempt + 1, HTTP_MAX_ATTEMPTS, url,
                     http_retry_delays_ms[attempt - 1]);
            vTaskDelay(pdMS_TO_TICKS(http_retry_delays_ms[attempt - 1]));
        }

        esp_http_client_handle_t client;
        bool reusing = (s_poll_mode && s_reuse_client != NULL);

        if (reusing) {
            client = s_reuse_client;
            esp_http_client_set_url(client, url);
        } else {
            esp_http_client_config_t cfg = {
                .url = url,
                .timeout_ms = 3000,
                .keep_alive_enable = s_poll_mode,
            };
            client = esp_http_client_init(&cfg);
            if (!client) continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            if (reusing) {
                esp_http_client_cleanup(client);
                s_reuse_client = NULL;
            } else {
                esp_http_client_cleanup(client);
            }
            continue;  // Transport error — retryable
        }

        int content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            if (reusing) {
                esp_http_client_cleanup(client);
                s_reuse_client = NULL;
            } else {
                esp_http_client_cleanup(client);
            }
            continue;  // Transport error — retryable
        }

        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "HTTP %d for %s", status, url);
            if (reusing) {
                esp_http_client_close(client);
            } else {
                esp_http_client_cleanup(client);
            }
            if (status >= 500) continue;  // 5xx — retryable
            return NULL;  // 4xx — not retryable
        }

        // Guard against chunked transfer encoding or missing Content-Length.
        // NINA API always sends Content-Length; if absent, treat as error.
        if (content_length == 0) {
            ESP_LOGW(TAG, "No Content-Length for %s (chunked?), skipping", url);
            if (reusing) {
                esp_http_client_close(client);
            } else {
                esp_http_client_cleanup(client);
            }
            perf_timer_stop(&g_perf.http_request);
            return NULL;
        }

        char *buffer = heap_caps_malloc(content_length + 1, MALLOC_CAP_SPIRAM);
        if (!buffer) {
            if (reusing) esp_http_client_close(client);
            else esp_http_client_cleanup(client);
            return NULL;  // OOM — not retryable
        }

        int total_read_len = 0, read_len;
        while (total_read_len < content_length) {
            read_len = esp_http_client_read(client, buffer + total_read_len,
                                            content_length - total_read_len);
            if (read_len <= 0) break;
            total_read_len += read_len;
        }
        buffer[total_read_len] = '\0';

        // Keep connection alive for reuse in poll mode; cleanup otherwise
        if (s_poll_mode) {
            esp_http_client_close(client);
            s_reuse_client = client;
        } else {
            esp_http_client_cleanup(client);
        }

        perf_timer_start(&g_perf.json_parse);
        cJSON *json = cJSON_Parse(buffer);
        perf_timer_stop(&g_perf.json_parse);
        perf_counter_increment(&g_perf.json_parse_count);
        free(buffer);
        perf_timer_stop(&g_perf.http_request);
        return json;
    }

    perf_counter_increment(&g_perf.http_failure_count);
    perf_timer_stop(&g_perf.http_request);
    return NULL;  // All attempts exhausted
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
        esp_http_client_cleanup((esp_http_client_handle_t)state->http_client);
    }
    memset(state, 0, sizeof(nina_poll_state_t));
}

void nina_client_poll(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance) {
    // Enable HTTP client reuse for this poll cycle
    s_poll_mode = true;
    s_reuse_client = (esp_http_client_handle_t)state->http_client;

    int64_t now_ms = esp_timer_get_time() / 1000;

    // Reset only volatile fields (preserve persistent data between polls)
    data->connected = false;
    data->exposure_current = 0;
    strcpy(data->status, "IDLE");
    strcpy(data->time_remaining, "--");

    // On very first call, set defaults for persistent fields
    if (!state->static_fetched) {
        strcpy(data->target_name, "No Target");
        strcpy(data->meridian_flip, "--");
        strcpy(data->profile_name, "NINA");
        strcpy(data->current_filter, "--");
        data->filter_count = 0;
    }

    ESP_LOGI(TAG, "=== Polling NINA data (%s: %s) ===",
             state->bundle_not_available ? "tiered" : "bundled",
             state->bundle_not_available ? "equipment/*/info" : "equipment/info");

    // --- BUNDLED: All equipment in one request (ninaAPI 2.2.15+) ---
    if (!state->bundle_not_available) {
        perf_timer_start(&g_perf.poll_equipment_bundle);
        int bundle_result = fetch_equipment_info_bundled(base_url, data, !state->static_fetched);
        perf_timer_stop(&g_perf.poll_equipment_bundle);

        if (bundle_result == -2) {
            // Endpoint not available (old ninaAPI) — fall back to individual fetchers
            ESP_LOGW(TAG, "/equipment/info not available, falling back to individual fetchers");
            state->bundle_not_available = true;
            // Fall through to legacy path below
        }
        // bundle_result == -1: HTTP failure, data->connected stays false (set at line 332)
        // bundle_result == 0: success, data->connected set by bundled parser
    }

    if (state->bundle_not_available) {
        // --- LEGACY: Individual equipment fetchers (old ninaAPI without /equipment/info) ---
        perf_timer_start(&g_perf.poll_camera);
        fetch_camera_info_robust(base_url, data);
        perf_timer_stop(&g_perf.poll_camera);
    }

    // --- Connection check (both bundled and legacy paths) ---
    nina_conn_state_t conn_state = nina_connection_report_poll(instance, data->connected);
    data->connected = (conn_state == NINA_CONN_CONNECTED);

    if (!data->connected) {
        state->static_fetched = false;
        nina_connection_set_static_data_ready(instance, false);
        state->http_client = (void *)s_reuse_client;
        s_reuse_client = NULL;
        s_poll_mode = false;
        return;
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
    } else {
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
    }

    if (state->bundle_not_available) {
        // --- LEGACY: Fast + conditional + slow tier fetchers ---
        perf_timer_start(&g_perf.poll_guider);
        fetch_guider_robust(base_url, data);
        perf_timer_stop(&g_perf.poll_guider);

        if (!data->websocket_connected) {
            perf_timer_start(&g_perf.poll_image_history);
            fetch_image_history_robust(base_url, data);
            perf_timer_stop(&g_perf.poll_image_history);
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
            perf_timer_start(&g_perf.poll_image_history);
            fetch_image_history_robust(base_url, data);
            perf_timer_stop(&g_perf.poll_image_history);
        }
        if (data->telescope_name[0] != '\0') {
            snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
        }
    }

    // --- SEQUENCE: Timer-based + event-driven polling ---
    bool sequence_due = (now_ms - state->last_sequence_poll_ms >= NINA_POLL_SEQUENCE_MS);
    if (sequence_due || data->sequence_poll_needed) {
        if (data->sequence_poll_needed) {
            ESP_LOGD(TAG, "Event-driven sequence poll triggered");
        }
        data->sequence_poll_needed = false;
        perf_timer_start(&g_perf.poll_sequence);
        fetch_sequence_counts_optional(base_url, data);
        perf_timer_stop(&g_perf.poll_sequence);
        state->last_sequence_poll_ms = now_ms;
    }

    fixup_exposure_timing(data);

    // Save persistent HTTP client for next poll cycle
    state->http_client = (void *)s_reuse_client;
    s_reuse_client = NULL;
    s_poll_mode = false;

    ESP_LOGI(TAG, "=== Poll Summary ===");
    ESP_LOGI(TAG, "Connected: %d, Profile: %s", data->connected, data->profile_name);
    ESP_LOGI(TAG, "Target: %s, Filter: %s", data->target_name, data->current_filter);
    ESP_LOGI(TAG, "Exposure: %.1fs (%.1f/%.1f)", data->exposure_total, data->exposure_current, data->exposure_total);
    ESP_LOGI(TAG, "Guiding: %.2f\", HFR: %.2f, Stars: %d", data->guider.rms_total, data->hfr, data->stars);
}

void nina_client_poll_heartbeat(const char *base_url, nina_client_t *data, int instance) {
    data->connected = false;
    fetch_camera_info_robust(base_url, data);

    nina_conn_state_t conn_state = nina_connection_report_poll(instance, data->connected);
    data->connected = (conn_state == NINA_CONN_CONNECTED);

    ESP_LOGD(TAG, "Heartbeat: connected=%d", data->connected);
}

void nina_client_poll_background(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance) {
    // Enable HTTP client reuse for this poll cycle
    s_poll_mode = true;
    s_reuse_client = (esp_http_client_handle_t)state->http_client;

    int64_t now_ms = esp_timer_get_time() / 1000;

    // Reset only volatile fields (preserve persistent data between polls)
    data->connected = false;
    data->exposure_current = 0;
    strcpy(data->status, "IDLE");
    strcpy(data->time_remaining, "--");

    // On very first call, set defaults for persistent fields
    if (!state->static_fetched) {
        strcpy(data->target_name, "No Target");
        strcpy(data->meridian_flip, "--");
        strcpy(data->profile_name, "NINA");
        strcpy(data->current_filter, "--");
        data->filter_count = 0;
    }

    // --- Equipment fetch (bundled or individual camera heartbeat) ---
    if (!state->bundle_not_available) {
        // Bundled: all equipment in one request (skip guider RMS for background)
        int bundle_result = fetch_equipment_info_bundled(base_url, data, !state->static_fetched);
        if (bundle_result == -2) {
            ESP_LOGW(TAG, "Background: /equipment/info not available, falling back");
            state->bundle_not_available = true;
            // Fall through to legacy camera heartbeat
        }
    }

    if (state->bundle_not_available) {
        // Legacy: camera-only heartbeat
        fetch_camera_info_robust(base_url, data);
    }

    nina_conn_state_t conn_state = nina_connection_report_poll(instance, data->connected);
    data->connected = (conn_state == NINA_CONN_CONNECTED);

    if (!data->connected) {
        state->static_fetched = false;
        nina_connection_set_static_data_ready(instance, false);
        state->http_client = (void *)s_reuse_client;
        s_reuse_client = NULL;
        s_poll_mode = false;
        return;
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
    } else {
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

    // Save persistent HTTP client for next poll cycle
    state->http_client = (void *)s_reuse_client;
    s_reuse_client = NULL;
    s_poll_mode = false;

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

    // IP addresses don't need DNS resolution
    if (hostname[0] >= '0' && hostname[0] <= '9') return true;

    int64_t now_ms = esp_timer_get_time() / 1000;

    // Check DNS cache for a valid (non-expired) entry
    for (int i = 0; i < 3; i++) {
        if (strcmp(s_dns_cache[i].hostname, hostname) == 0 &&
            s_dns_cache[i].resolved_ip[0] != '\0') {
            if (now_ms - s_dns_cache[i].resolve_time_ms < DNS_CACHE_TTL_MS) {
                ESP_LOGD(TAG, "DNS cache hit for %s -> %s", hostname, s_dns_cache[i].resolved_ip);
                return true;
            }
            break;  // Found entry but expired — do fresh lookup
        }
    }

    // Cache miss or expired — perform real DNS lookup
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    struct addrinfo *result = NULL;
    int err = getaddrinfo(hostname, NULL, &hints, &result);

    if (err == 0 && result) {
        // Lookup succeeded — update cache
        char ip_str[46] = {0};
        struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
        inet_ntoa_r(addr->sin_addr, ip_str, sizeof(ip_str));
        freeaddrinfo(result);

        // Find existing slot or first empty slot
        int slot = -1;
        for (int i = 0; i < 3; i++) {
            if (strcmp(s_dns_cache[i].hostname, hostname) == 0 ||
                s_dns_cache[i].hostname[0] == '\0') {
                slot = i;
                break;
            }
        }
        if (slot < 0) slot = 0;  // Evict first slot if all full

        snprintf(s_dns_cache[slot].hostname, sizeof(s_dns_cache[slot].hostname), "%s", hostname);
        snprintf(s_dns_cache[slot].resolved_ip, sizeof(s_dns_cache[slot].resolved_ip), "%s", ip_str);
        s_dns_cache[slot].resolve_time_ms = now_ms;

        ESP_LOGD(TAG, "DNS cached %s -> %s", hostname, ip_str);
        return true;
    }

    if (result) freeaddrinfo(result);

    // Lookup failed — try stale cache as fallback
    for (int i = 0; i < 3; i++) {
        if (strcmp(s_dns_cache[i].hostname, hostname) == 0 &&
            s_dns_cache[i].resolved_ip[0] != '\0') {
            ESP_LOGW(TAG, "DNS lookup failed for %s, using stale cache -> %s",
                     hostname, s_dns_cache[i].resolved_ip);
            return true;
        }
    }

    ESP_LOGD(TAG, "DNS check failed for %s (err %d), no cache available", hostname, err);
    return false;
}

#define MAX_IMAGE_SIZE (4 * 1024 * 1024)  // 4 MB cap for image downloads

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

    int buf_size = (content_length > 0) ? content_length : (256 * 1024);
    uint8_t *buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for image", buf_size);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total_read = 0, read_len;
    while (1) {
        int to_read = buf_size - total_read;
        if (to_read <= 0) {
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
        free(buffer);
        return NULL;
    }

    if (!chunked && total_read < content_length) {
        ESP_LOGE(TAG, "Incomplete image read: %d/%d", total_read, content_length);
        free(buffer);
        return NULL;
    }

    *out_size = (size_t)total_read;
    ESP_LOGI(TAG, "Image fetched: %d bytes", total_read);
    return buffer;
}
