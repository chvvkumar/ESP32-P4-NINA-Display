/**
 * @file nina_websocket.c
 * @brief WebSocket client lifecycle and event handling for NINA.
 *
 * Maintains one persistent WebSocket connection per NINA instance
 * (up to MAX_NINA_INSTANCES). Each connection receives IMAGE-SAVE,
 * FILTERWHEEL-CHANGED, SEQUENCE-FINISHED, SEQUENCE-STARTING,
 * GUIDER-DITHER, and GUIDER-START events independently.
 */

#include "nina_websocket.h"
#include "nina_client_internal.h"
#include "app_config.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include "perf_monitor.h"
#include "nina_connection.h"
#include "tasks.h"
#include "ui/nina_toast.h"
#include "ui/nina_event_log.h"
#include "ui/nina_alerts.h"
#include "ui/nina_safety.h"
#include "ui/nina_session_stats.h"

static const char *TAG = "nina_ws";

#define WS_BACKOFF_INITIAL_MS  5000
#define WS_BACKOFF_MAX_MS      60000

static esp_websocket_client_handle_t ws_clients[MAX_NINA_INSTANCES];
static nina_client_t *ws_client_data[MAX_NINA_INSTANCES];

// Exponential backoff state per instance (atomic: accessed from WS event handler + poll task)
static _Atomic int ws_backoff_ms[MAX_NINA_INSTANCES];
static _Atomic int64_t ws_disconnect_time_ms[MAX_NINA_INSTANCES];
static _Atomic bool ws_needs_reconnect[MAX_NINA_INSTANCES];

/* ── Connect aggregation state ──────────────────────────────────────── */
#define CONNECT_AGG_IDLE_TIMEOUT_MS 60000

typedef enum {
    EQ_CAMERA = 0, EQ_MOUNT, EQ_GUIDER, EQ_FOCUSER, EQ_FILTERWHEEL,
    EQ_ROTATOR, EQ_SAFETY, EQ_DOME, EQ_FLAT, EQ_SWITCH, EQ_WEATHER,
    EQ_COUNT
} equipment_type_t;

static const char *equipment_names[] = {
    "Camera", "Mount", "Guider", "Focuser", "Filterwheel",
    "Rotator", "Safety", "Dome", "Flat", "Switch", "Weather"
};

typedef struct {
    int64_t  window_start_ms;       // 0 = no active window
    int64_t  last_connect_ms;       // for 60s idle timeout
    uint16_t connected_mask;        // bits for each equipment type
    uint16_t disconnected_mask;     // bits for disconnect cancellation
} connect_agg_state_t;

static connect_agg_state_t s_agg[MAX_NINA_INSTANCES];
static esp_timer_handle_t  s_agg_timers[MAX_NINA_INSTANCES];
static portMUX_TYPE        s_agg_lock = portMUX_INITIALIZER_UNLOCKED;
/* Track equipment that has connected at least once — suppress disconnect
 * toasts for equipment we've never seen connect (e.g., no driver selected
 * in NINA). NINA fires DISCONNECT for unconfigured equipment on startup. */
static uint16_t            s_ever_connected[MAX_NINA_INSTANCES];

/**
 * @brief Extract hostname from a NINA instance config URL.
 *
 * Given "http://astromele2.lan:1888/v2/api/" returns "astromele2.lan".
 * Result is written to @p out (max @p out_size bytes).
 */
static void get_instance_hostname(int index, char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    if (index < 0 || index >= MAX_NINA_INSTANCES) return;
    const char *url = app_config_get_instance_url(index);
    if (!url || url[0] == '\0') return;

    /* Skip scheme (http:// or https://) */
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;

    /* Copy until ':' or '/' or end */
    size_t i = 0;
    while (host[i] && host[i] != ':' && host[i] != '/' && i < out_size - 1) {
        out[i] = host[i];
        i++;
    }
    out[i] = '\0';
}

/** Show a toast prefixed with the NINA instance hostname. Thread-safe. */
static void ws_toast(int index, toast_severity_t sev, const char *msg) {
    char host[48];
    get_instance_hostname(index, host, sizeof(host));
    if (host[0]) {
        nina_toast_show_fmt(sev, "%s: %s", host, msg);
    } else {
        nina_toast_show(sev, msg);
    }
}

/** Printf-style ws_toast. Thread-safe. */
static void ws_toast_fmt(int index, toast_severity_t sev, const char *fmt, ...) {
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ws_toast(index, sev, msg);
}

/* ── Aggregation timer callback ──────────────────────────────────────── */
static void agg_timer_cb(void *arg) {
    int index = (int)(intptr_t)arg;
    if (index < 0 || index >= MAX_NINA_INSTANCES) return;

    /* Snapshot aggregation state under spinlock, then reset */
    portENTER_CRITICAL(&s_agg_lock);
    uint16_t conn_mask = s_agg[index].connected_mask;
    uint16_t disc_mask = s_agg[index].disconnected_mask;
    s_agg[index].connected_mask = 0;
    s_agg[index].disconnected_mask = 0;
    s_agg[index].window_start_ms = 0;
    portEXIT_CRITICAL(&s_agg_lock);

    const app_config_t *cfg = app_config_get();

    /* Masks already reflect final state (last event wins — connect clears disconnect
     * bit and vice versa), so no cancellation logic needed here. */
    uint16_t connect_final = conn_mask;
    if (connect_final) {
        /* Count connected equipment */
        int count = 0;
        for (int i = 0; i < EQ_COUNT; i++) {
            if (connect_final & (1 << i)) count++;
        }

        char buf[128];
        int pos = 0;
        char host[48];
        get_instance_hostname(index, host, sizeof(host));

        if (count > 4) {
            /* Many devices — show count to keep message short */
            if (host[0])
                snprintf(buf, sizeof(buf), "%s: %d devices connected", host, count);
            else
                snprintf(buf, sizeof(buf), "%d devices connected", count);
        } else {
            /* Few devices — list them by name */
            if (host[0])
                pos = snprintf(buf, sizeof(buf), "%s: Connected ", host);
            else
                pos = snprintf(buf, sizeof(buf), "Connected ");

            bool first = true;
            for (int i = 0; i < EQ_COUNT && pos < (int)sizeof(buf) - 2; i++) {
                if (connect_final & (1 << i)) {
                    if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ", ");
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", equipment_names[i]);
                    first = false;
                }
            }
        }

        if ((cfg->toast_notify_mask & (1 << 0)) && !cfg->toast_instance_muted[index]) {
            nina_toast_show(TOAST_SUCCESS, buf);
        }
    }

    /* Show deferred disconnects — only for equipment we've seen connect before.
     * NINA fires DISCONNECT for unconfigured equipment (no driver) on startup. */
    uint16_t disc_final = disc_mask & s_ever_connected[index];
    if (disc_final) {
        /* Determine severity — need to check sequence status */
        nina_client_t *data = ws_client_data[index];
        bool seq_running = false;
        if (data && nina_client_lock(data, 50)) {
            seq_running = (strcmp(data->status, "RUNNING") == 0);
            nina_client_unlock(data);
        }

        for (int i = 0; i < EQ_COUNT; i++) {
            if (!(disc_final & (1 << i))) continue;

            toast_severity_t sev = TOAST_WARNING;
            if (i == EQ_SAFETY) sev = TOAST_ERROR;
            else if (seq_running) sev = TOAST_ERROR;

            if ((cfg->toast_notify_mask & (1 << 1)) && !cfg->toast_instance_muted[index]) {
                ws_toast_fmt(index, sev, "%s disconnected", equipment_names[i]);
            }
            nina_event_log_add_fmt(sev == TOAST_ERROR ? EVENT_SEV_ERROR : EVENT_SEV_WARNING,
                                   index, "%s disconnected", equipment_names[i]);
        }
    }
}

/* ── Record connect event (with aggregation) ────────────────────────── */
static void record_connect_event(int index, equipment_type_t eq) {
    const app_config_t *cfg = app_config_get();
    int64_t now = esp_timer_get_time() / 1000;

    // If aggregation disabled, show individual toast
    if (cfg->toast_aggregation_window_s == 0) {
        if ((cfg->toast_notify_mask & (1 << 0)) && !cfg->toast_instance_muted[index]) {
            ws_toast(index, TOAST_SUCCESS, equipment_names[eq]);
        }
        nina_event_log_add_fmt(EVENT_SEV_INFO, index, "%s connected", equipment_names[eq]);
        return;
    }

    bool start_timer = false;

    portENTER_CRITICAL(&s_agg_lock);

    connect_agg_state_t *agg = &s_agg[index];

    // Check 60s idle timeout
    if (agg->window_start_ms != 0 &&
        (now - agg->last_connect_ms) > CONNECT_AGG_IDLE_TIMEOUT_MS) {
        agg->window_start_ms = 0;
        agg->connected_mask = 0;
        agg->disconnected_mask = 0;
    }

    // Start new window if needed
    if (agg->window_start_ms == 0) {
        agg->window_start_ms = now;
        agg->connected_mask = 0;
        agg->disconnected_mask = 0;
        start_timer = true;
    }

    agg->connected_mask |= (1 << eq);
    agg->disconnected_mask &= ~(1 << eq);  /* Last event wins: connect clears disconnect */
    s_ever_connected[index] |= (1 << eq);  /* Remember this equipment has connected */
    agg->last_connect_ms = now;

    portEXIT_CRITICAL(&s_agg_lock);

    // Start timer outside critical section (esp_timer APIs are not ISR-safe)
    if (start_timer) {
        esp_timer_stop(s_agg_timers[index]);
        esp_timer_start_once(s_agg_timers[index],
                             (uint64_t)cfg->toast_aggregation_window_s * 1000000);
    }

    nina_event_log_add_fmt(EVENT_SEV_INFO, index, "%s connected", equipment_names[eq]);
}

/* ── Record disconnect event (with aggregation deferral) ──────────────
 * NINA fires DISCONNECT before CONNECT during a fresh connect sequence.
 * When aggregation is enabled, disconnects are deferred to the window
 * boundary so they can cancel with subsequent reconnects. This prevents
 * false disconnect toasts during NINA's connect handshake.
 * ──────────────────────────────────────────────────────────────────── */
static void record_disconnect_event(int index, equipment_type_t eq) {
    const app_config_t *cfg = app_config_get();
    int64_t now = esp_timer_get_time() / 1000;

    /* When aggregation is enabled, defer disconnects to the window boundary */
    if (cfg->toast_aggregation_window_s > 0) {
        bool start_timer = false;

        portENTER_CRITICAL(&s_agg_lock);

        connect_agg_state_t *agg = &s_agg[index];

        /* Check 60s idle timeout */
        if (agg->window_start_ms != 0 &&
            (now - agg->last_connect_ms) > CONNECT_AGG_IDLE_TIMEOUT_MS) {
            agg->window_start_ms = 0;
            agg->connected_mask = 0;
            agg->disconnected_mask = 0;
        }

        /* Start a new window if needed (disconnect can start a window) */
        if (agg->window_start_ms == 0) {
            agg->window_start_ms = now;
            agg->connected_mask = 0;
            agg->disconnected_mask = 0;
            start_timer = true;
        }

        agg->disconnected_mask |= (1 << eq);
        agg->connected_mask &= ~(1 << eq);  /* Last event wins: disconnect clears connect */
        agg->last_connect_ms = now;

        portEXIT_CRITICAL(&s_agg_lock);

        if (start_timer) {
            esp_timer_stop(s_agg_timers[index]);
            esp_timer_start_once(s_agg_timers[index],
                                 (uint64_t)cfg->toast_aggregation_window_s * 1000000);
        }

        /* Always log immediately even when toast is deferred */
        nina_event_log_add_fmt(EVENT_SEV_WARNING, index,
                               "%s disconnected", equipment_names[eq]);
        return;
    }

    /* Aggregation disabled — show disconnect toast immediately,
     * but only for equipment we've previously seen connect */
    if (!(s_ever_connected[index] & (1 << eq))) return;

    toast_severity_t sev = TOAST_WARNING;
    if (eq == EQ_SAFETY) {
        sev = TOAST_ERROR;
    } else {
        nina_client_t *data = ws_client_data[index];
        if (data && nina_client_lock(data, 50)) {
            if (strcmp(data->status, "RUNNING") == 0) sev = TOAST_ERROR;
            nina_client_unlock(data);
        }
    }

    if ((cfg->toast_notify_mask & (1 << 1)) && !cfg->toast_instance_muted[index]) {
        ws_toast_fmt(index, sev, "%s disconnected", equipment_names[eq]);
    }
    nina_event_log_add_fmt(sev == TOAST_ERROR ? EVENT_SEV_ERROR : EVENT_SEV_WARNING,
                           index, "%s disconnected", equipment_names[eq]);
}

/* Check if a notification category is enabled for this instance */
static bool toast_allowed(int index, int category_bit) {
    const app_config_t *cfg = app_config_get();
    return (cfg->toast_notify_mask & (1 << category_bit)) &&
           !cfg->toast_instance_muted[index];
}

/**
 * @brief Process incoming WebSocket JSON event from NINA
 */
static void handle_websocket_message(int index, const char *payload, int len) {
    nina_client_t *data = ws_client_data[index];
    if (!data || !payload || len <= 0) return;

    perf_counter_increment(&g_perf.ws_event_count);

    cJSON *json = cJSON_ParseWithLength(payload, len);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response) {
        cJSON_Delete(json);
        return;
    }

    cJSON *evt = cJSON_GetObjectItem(response, "Event");
    if (!evt || !evt->valuestring) {
        cJSON_Delete(json);
        return;
    }

    // IMAGE-SAVE: Capture full ImageStatistics for dashboard and info overlay
    if (strcmp(evt->valuestring, "IMAGE-SAVE") == 0) {
        ESP_LOGI(TAG, "WS[%d]: IMAGE-SAVE event received", index);

        cJSON *stats = cJSON_GetObjectItem(response, "ImageStatistics");
        if (stats) {
            // Extract all values from JSON before taking the lock
            float new_hfr = data->hfr;
            int new_stars = data->stars;
            float new_exp_total = data->exposure_total;
            const char *new_target = NULL;
            const char *new_telescope = NULL;

            cJSON *hfr = cJSON_GetObjectItem(stats, "HFR");
            if (hfr) new_hfr = (float)hfr->valuedouble;

            cJSON *stars = cJSON_GetObjectItem(stats, "Stars");
            if (stars) new_stars = stars->valueint;

            cJSON *exp = cJSON_GetObjectItem(stats, "ExposureTime");
            if (exp) new_exp_total = (float)exp->valuedouble;

            cJSON *target = cJSON_GetObjectItem(stats, "TargetName");
            if (target && target->valuestring && target->valuestring[0] != '\0')
                new_target = target->valuestring;

            cJSON *telescope = cJSON_GetObjectItem(stats, "TelescopeName");
            if (telescope && telescope->valuestring)
                new_telescope = telescope->valuestring;

            // Extract extended ImageStatistics for info overlay
            imagestats_detail_data_t img_stats = {0};
            img_stats.has_data = true;
            img_stats.hfr = new_hfr;
            img_stats.stars = new_stars;
            img_stats.exposure_time = new_exp_total;

            cJSON *hfr_stdev = cJSON_GetObjectItem(stats, "HFRStDev");
            if (hfr_stdev) img_stats.hfr_stdev = (float)hfr_stdev->valuedouble;

            cJSON *mean = cJSON_GetObjectItem(stats, "Mean");
            if (mean) img_stats.mean = (float)mean->valuedouble;

            cJSON *median = cJSON_GetObjectItem(stats, "Median");
            if (median) img_stats.median = (float)median->valuedouble;

            cJSON *stdev = cJSON_GetObjectItem(stats, "StDev");
            if (stdev) img_stats.stdev = (float)stdev->valuedouble;

            cJSON *min_val = cJSON_GetObjectItem(stats, "Min");
            if (min_val) img_stats.min_val = min_val->valueint;

            cJSON *max_val = cJSON_GetObjectItem(stats, "Max");
            if (max_val) img_stats.max_val = max_val->valueint;

            cJSON *gain = cJSON_GetObjectItem(stats, "Gain");
            if (gain) img_stats.gain = gain->valueint;

            cJSON *offset = cJSON_GetObjectItem(stats, "Offset");
            if (offset) img_stats.offset = offset->valueint;

            cJSON *temperature = cJSON_GetObjectItem(stats, "Temperature");
            if (temperature) img_stats.temperature = (float)temperature->valuedouble;

            cJSON *filter = cJSON_GetObjectItem(stats, "Filter");
            if (filter && filter->valuestring)
                strncpy(img_stats.filter, filter->valuestring, sizeof(img_stats.filter) - 1);

            if (new_target)
                strncpy(img_stats.camera_name, "", sizeof(img_stats.camera_name) - 1);

            cJSON *camera_name = cJSON_GetObjectItem(stats, "CameraName");
            if (camera_name && camera_name->valuestring)
                strncpy(img_stats.camera_name, camera_name->valuestring, sizeof(img_stats.camera_name) - 1);

            if (new_telescope)
                strncpy(img_stats.telescope_name, new_telescope, sizeof(img_stats.telescope_name) - 1);

            cJSON *focal_length = cJSON_GetObjectItem(stats, "FocalLength");
            if (focal_length) img_stats.focal_length = focal_length->valueint;

            cJSON *date = cJSON_GetObjectItem(stats, "Date");
            if (date && date->valuestring)
                strncpy(img_stats.date, date->valuestring, sizeof(img_stats.date) - 1);

            cJSON *filename = cJSON_GetObjectItem(stats, "Filename");
            if (filename && filename->valuestring)
                strncpy(img_stats.filename, filename->valuestring, sizeof(img_stats.filename) - 1);

            // Short critical section: write parsed values into the shared struct
            if (nina_client_lock(data, 50)) {
                data->hfr = new_hfr;
                data->stars = new_stars;
                data->exposure_total = new_exp_total;
                if (new_target) {
                    strncpy(data->target_name, new_target,
                            sizeof(data->target_name) - 1);
                }
                if (new_telescope) {
                    strncpy(data->telescope_name, new_telescope,
                            sizeof(data->telescope_name) - 1);
                }
                data->last_image_stats = img_stats;
                data->new_image_available = true;
                data->ui_refresh_needed = true;
                data->sequence_poll_needed = true;

                // Append to local HFR ring buffer (used for graph auto-refresh)
                if (data->hfr_ring.hfr && new_hfr > 0) {
                    int idx = data->hfr_ring.write_idx;
                    data->hfr_ring.hfr[idx]   = new_hfr;
                    data->hfr_ring.stars[idx]  = new_stars;
                    data->hfr_ring.write_idx   = (idx + 1) % HFR_RING_SIZE;
                    data->hfr_ring.count++;
                }

                nina_client_unlock(data);
            } else {
                ESP_LOGW(TAG, "WS[%d]: Could not acquire mutex for IMAGE-SAVE", index);
            }

            nina_event_log_add_fmt(EVENT_SEV_INFO, index,
                "Image #%d: %s, HFR %.2f, %d stars",
                data->exposure_count, img_stats.filter, new_hfr, new_stars);
            nina_session_stats_add_exposure(index, new_exp_total);

            ESP_LOGI(TAG, "WS[%d]: HFR=%.2f Stars=%d Filter=%s Target=%s",
                index, new_hfr, new_stars,
                img_stats.filter, data->target_name);
        }
    }
    // FILTERWHEEL-CHANGED: Replaces fetch_filter_robust_ex for current filter
    else if (strcmp(evt->valuestring, "FILTERWHEEL-CHANGED") == 0) {
        cJSON *new_f = cJSON_GetObjectItem(response, "New");
        if (new_f) {
            cJSON *name = cJSON_GetObjectItem(new_f, "Name");
            if (name && name->valuestring) {
                if (nina_client_lock(data, 50)) {
                    strncpy(data->current_filter, name->valuestring,
                            sizeof(data->current_filter) - 1);
                    data->ui_refresh_needed = true;
                    nina_client_unlock(data);
                }
                nina_event_log_add_fmt(EVENT_SEV_INFO, index,
                    "Filter changed to %s", name->valuestring);
                ESP_LOGI(TAG, "WS[%d]: Filter changed to %s", index, data->current_filter);
            }
        }
    }
    // SEQUENCE-FINISHED: Mark sequence as done
    else if (strcmp(evt->valuestring, "SEQUENCE-FINISHED") == 0) {
        if (nina_client_lock(data, 50)) {
            strcpy(data->status, "FINISHED");
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        if (toast_allowed(index, 2))
            ws_toast(index, TOAST_SUCCESS, "Sequence completed");
        nina_event_log_add(EVENT_SEV_SUCCESS, index, "Sequence completed");
        ESP_LOGI(TAG, "WS[%d]: Sequence finished", index);
    }
    // SEQUENCE-STARTING: Mark sequence as running
    else if (strcmp(evt->valuestring, "SEQUENCE-STARTING") == 0) {
        if (nina_client_lock(data, 50)) {
            strcpy(data->status, "RUNNING");
            data->is_waiting = false;
            data->ui_refresh_needed = true;
            data->sequence_poll_needed = true;
            nina_client_unlock(data);
        }
        if (toast_allowed(index, 2))
            ws_toast(index, TOAST_INFO, "Sequence started");
        nina_event_log_add(EVENT_SEV_INFO, index, "Sequence started");
        nina_session_stats_reset(index);
        ESP_LOGI(TAG, "WS[%d]: Sequence starting", index);
    }
    // GUIDER-DITHER: Flag dithering state
    else if (strcmp(evt->valuestring, "GUIDER-DITHER") == 0) {
        if (nina_client_lock(data, 50)) {
            data->is_dithering = true;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        nina_event_log_add(EVENT_SEV_INFO, index, "Dithering");
        ESP_LOGI(TAG, "WS[%d]: Dithering", index);
    }
    // GUIDER-START: Clear dithering flag
    else if (strcmp(evt->valuestring, "GUIDER-START") == 0) {
        if (nina_client_lock(data, 50)) {
            data->is_dithering = false;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        ESP_LOGI(TAG, "WS[%d]: Guiding started", index);
    }
    // TS-NEWTARGETSTART: New target started in sequence — instant target name update
    else if (strcmp(evt->valuestring, "TS-NEWTARGETSTART") == 0) {
        cJSON *target_name = cJSON_GetObjectItem(response, "TargetName");
        if (nina_client_lock(data, 50)) {
            if (target_name && target_name->valuestring && target_name->valuestring[0] != '\0') {
                strncpy(data->target_name, target_name->valuestring,
                        sizeof(data->target_name) - 1);
            }
            data->is_waiting = false;
            data->ui_refresh_needed = true;
            data->sequence_poll_needed = true;
            nina_client_unlock(data);
        }
        if (target_name && target_name->valuestring)
            nina_event_log_add_fmt(EVENT_SEV_INFO, index, "New target: %s", target_name->valuestring);
        ESP_LOGI(TAG, "WS[%d]: New target: %s", index,
                 target_name && target_name->valuestring ? target_name->valuestring : "(null)");
    }
    // AUTOFOCUS-STARTING: Clear V-curve buffer and mark AF running
    else if (strcmp(evt->valuestring, "AUTOFOCUS-STARTING") == 0) {
        if (nina_client_lock(data, 50)) {
            memset(&data->autofocus, 0, sizeof(data->autofocus));
            data->autofocus.af_running = true;
            data->autofocus.has_data = true;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        nina_event_log_add(EVENT_SEV_INFO, index, "Autofocus started");
        ESP_LOGI(TAG, "WS[%d]: Autofocus starting", index);
    }
    // AUTOFOCUS-FINISHED: Mark AF complete, find best point
    else if (strcmp(evt->valuestring, "AUTOFOCUS-FINISHED") == 0) {
        if (nina_client_lock(data, 50)) {
            data->autofocus.af_running = false;
            // Find best (minimum HFR) point
            float best_hfr = 999.0f;
            int best_pos = 0;
            for (int i = 0; i < data->autofocus.count; i++) {
                if (data->autofocus.points[i].hfr < best_hfr) {
                    best_hfr = data->autofocus.points[i].hfr;
                    best_pos = data->autofocus.points[i].position;
                }
            }
            data->autofocus.best_hfr = best_hfr;
            data->autofocus.best_position = best_pos;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        if (toast_allowed(index, 3))
            ws_toast(index, TOAST_SUCCESS, "Autofocus complete");
        nina_event_log_add_fmt(EVENT_SEV_SUCCESS, index,
            "Autofocus complete: pos %d, HFR %.2f",
            data->autofocus.best_position, data->autofocus.best_hfr);
        ESP_LOGI(TAG, "WS[%d]: Autofocus finished (%d points)", index, data->autofocus.count);
    }
    // AUTOFOCUS-POINT-ADDED: Add V-curve data point {Position, HFR}
    else if (strcmp(evt->valuestring, "AUTOFOCUS-POINT-ADDED") == 0) {
        cJSON *position = cJSON_GetObjectItem(response, "Position");
        cJSON *af_hfr = cJSON_GetObjectItem(response, "HFR");

        if (position && af_hfr) {
            int pos = position->valueint;
            float hfr_val = (float)af_hfr->valuedouble;

            if (nina_client_lock(data, 50)) {
                if (data->autofocus.count < MAX_AF_POINTS) {
                    int idx = data->autofocus.count;
                    data->autofocus.points[idx].position = pos;
                    data->autofocus.points[idx].hfr = hfr_val;
                    data->autofocus.count++;
                }
                data->ui_refresh_needed = true;
                nina_client_unlock(data);
            }
            ESP_LOGI(TAG, "WS[%d]: AF point: pos=%d HFR=%.2f (%d/%d)",
                     index, pos, hfr_val, data->autofocus.count, MAX_AF_POINTS);
        }
    }
    // ROTATOR-MOVED / ROTATOR-MOVED-MECHANICAL: Update rotator angle from WS
    else if (strcmp(evt->valuestring, "ROTATOR-MOVED") == 0 ||
             strcmp(evt->valuestring, "ROTATOR-MOVED-MECHANICAL") == 0) {
        cJSON *to = cJSON_GetObjectItem(response, "To");
        if (to && nina_client_lock(data, 50)) {
            data->rotator_angle = (float)to->valuedouble;
            data->rotator_connected = true;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        ESP_LOGI(TAG, "WS[%d]: Rotator moved to %.1f", index,
                 to ? (float)to->valuedouble : 0.0f);
    }
    // SAFETY-CHANGED: Update safety monitor state
    else if (strcmp(evt->valuestring, "SAFETY-CHANGED") == 0) {
        cJSON *is_safe = cJSON_GetObjectItem(response, "IsSafe");
        bool safe = is_safe && cJSON_IsTrue(is_safe);
        if (nina_client_lock(data, 50)) {
            data->safety_connected = true;
            data->safety_is_safe = safe;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        /* Toast + event log (thread-safe, no lock needed) */
        if (safe) {
            if (toast_allowed(index, 7))
                ws_toast(index, TOAST_SUCCESS, "Safe");
            nina_event_log_add(EVENT_SEV_SUCCESS, index, "Observatory is safe");
        } else {
            if (toast_allowed(index, 7))
                ws_toast(index, TOAST_ERROR, "UNSAFE");
            nina_event_log_add(EVENT_SEV_ERROR, index, "Observatory UNSAFE!");
            if (app_config_get()->alert_flash_enabled) {
                nina_alert_trigger(ALERT_SAFETY, index, 0);
            }
        }
        /* Safety state update (no display lock needed — state tracking only) */
        nina_safety_update(true, safe);
        ESP_LOGI(TAG, "WS[%d]: Safety changed: %s", index, safe ? "SAFE" : "UNSAFE");
    }
    // TS-WAITSTART: Sequence entering wait state
    else if (strcmp(evt->valuestring, "TS-WAITSTART") == 0) {
        if (nina_client_lock(data, 50)) {
            data->is_waiting = true;
            cJSON *wait_time = cJSON_GetObjectItem(response, "WaitStartTime");
            if (wait_time && wait_time->valuestring) {
                data->wait_start_epoch = parse_iso8601(wait_time->valuestring);
            }
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        if (toast_allowed(index, 2))
            ws_toast(index, TOAST_INFO, "Waiting for next target");
        nina_event_log_add(EVENT_SEV_INFO, index, "Waiting for next target");
        ESP_LOGI(TAG, "WS[%d]: Waiting for next target", index);
    }
    // MOUNT-BEFORE-FLIP: Meridian flip starting
    else if (strcmp(evt->valuestring, "MOUNT-BEFORE-FLIP") == 0) {
        if (nina_client_lock(data, 50)) {
            strncpy(data->meridian_flip, "FLIPPING", sizeof(data->meridian_flip) - 1);
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        ESP_LOGI(TAG, "WS[%d]: Meridian flip starting", index);
    }
    // MOUNT-AFTER-FLIP: Meridian flip completed
    else if (strcmp(evt->valuestring, "MOUNT-AFTER-FLIP") == 0) {
        if (nina_client_lock(data, 50)) {
            strncpy(data->meridian_flip, "--", sizeof(data->meridian_flip) - 1);
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        if (toast_allowed(index, 5))
            ws_toast(index, TOAST_INFO, "Meridian flip completed");
        nina_event_log_add(EVENT_SEV_INFO, index, "Meridian flip completed");
        ESP_LOGI(TAG, "WS[%d]: Meridian flip completed", index);
    }
    // GUIDER-STOP: Guider stopped — clear RMS values
    else if (strcmp(evt->valuestring, "GUIDER-STOP") == 0) {
        if (nina_client_lock(data, 50)) {
            data->guider.rms_total = 0;
            data->guider.rms_ra = 0;
            data->guider.rms_dec = 0;
            data->is_dithering = false;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        if (toast_allowed(index, 6))
            ws_toast(index, TOAST_WARNING, "Guider stopped");
        nina_event_log_add(EVENT_SEV_WARNING, index, "Guider stopped");
        ESP_LOGI(TAG, "WS[%d]: Guider stopped", index);
    }
    // PROFILE-CHANGED: Invalidate cached static data to force re-fetch
    else if (strcmp(evt->valuestring, "PROFILE-CHANGED") == 0) {
        ESP_LOGI(TAG, "WS[%d]: Profile changed, will re-fetch static data", index);
        if (nina_client_lock(data, 50)) {
            data->profile_refresh_needed = true;
            data->sequence_poll_needed = true;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        /* Reset equipment mask — new profile may have different equipment */
        s_ever_connected[index] = 0;
        if (toast_allowed(index, 9))
            ws_toast(index, TOAST_INFO, "Profile changed");
        nina_event_log_add(EVENT_SEV_INFO, index, "Profile changed");
    }
    // AUTOFOCUS-FAILED: AF did not converge
    else if (strcmp(evt->valuestring, "AUTOFOCUS-FAILED") == 0) {
        if (nina_client_lock(data, 50)) {
            data->autofocus.af_running = false;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        if (toast_allowed(index, 3) || toast_allowed(index, 8))
            ws_toast(index, TOAST_ERROR, "Autofocus failed");
        nina_event_log_add(EVENT_SEV_ERROR, index, "Autofocus failed");
        ESP_LOGW(TAG, "WS[%d]: Autofocus failed", index);
    }
    // CAMERA-CONNECTED: Camera connected — use aggregation
    else if (strcmp(evt->valuestring, "CAMERA-CONNECTED") == 0) {
        record_connect_event(index, EQ_CAMERA);
        ESP_LOGI(TAG, "WS[%d]: Camera connected", index);
    }
    // CAMERA-DISCONNECTED: Direct disconnect via aggregation system
    else if (strcmp(evt->valuestring, "CAMERA-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_CAMERA);
        ESP_LOGW(TAG, "WS[%d]: Camera disconnected", index);
    }
    // MOUNT-SLEWING: Mount is slewing to target
    else if (strcmp(evt->valuestring, "MOUNT-SLEWING") == 0) {
        if (toast_allowed(index, 4))
            ws_toast(index, TOAST_INFO, "Mount slewing to target");
        ESP_LOGI(TAG, "WS[%d]: Mount slewing", index);
    }
    // MOUNT-PARKED: Mount parked
    else if (strcmp(evt->valuestring, "MOUNT-PARKED") == 0) {
        if (toast_allowed(index, 4))
            ws_toast(index, TOAST_INFO, "Mount parked");
        nina_event_log_add(EVENT_SEV_INFO, index, "Mount parked");
        ESP_LOGI(TAG, "WS[%d]: Mount parked", index);
    }
    // MOUNT-HOMED: Mount homed
    else if (strcmp(evt->valuestring, "MOUNT-HOMED") == 0) {
        if (toast_allowed(index, 4))
            ws_toast(index, TOAST_INFO, "Mount homed");
        ESP_LOGI(TAG, "WS[%d]: Mount homed", index);
    }
    // MOUNT-TRACKING-ON: Tracking started
    else if (strcmp(evt->valuestring, "MOUNT-TRACKING-ON") == 0) {
        if (toast_allowed(index, 4))
            ws_toast(index, TOAST_SUCCESS, "Tracking started");
        ESP_LOGI(TAG, "WS[%d]: Tracking started", index);
    }
    // MOUNT-TRACKING-OFF: Tracking stopped
    else if (strcmp(evt->valuestring, "MOUNT-TRACKING-OFF") == 0) {
        if (toast_allowed(index, 4))
            ws_toast(index, TOAST_WARNING, "Tracking stopped");
        nina_event_log_add(EVENT_SEV_WARNING, index, "Tracking stopped");
        ESP_LOGW(TAG, "WS[%d]: Tracking stopped", index);
    }
    // ERROR*: Any error event
    else if (strncmp(evt->valuestring, "ERROR", 5) == 0) {
        if (toast_allowed(index, 8))
            ws_toast_fmt(index, TOAST_ERROR, "Error: %s", evt->valuestring);
        nina_event_log_add_fmt(EVENT_SEV_ERROR, index, "Error: %s", evt->valuestring);
        ESP_LOGE(TAG, "WS[%d]: Error event: %s", index, evt->valuestring);
    }
    // Equipment connect events — aggregated
    else if (strcmp(evt->valuestring, "GUIDER-CONNECTED") == 0) {
        record_connect_event(index, EQ_GUIDER);
    }
    else if (strcmp(evt->valuestring, "GUIDER-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_GUIDER);
    }
    else if (strcmp(evt->valuestring, "MOUNT-CONNECTED") == 0) {
        record_connect_event(index, EQ_MOUNT);
    }
    else if (strcmp(evt->valuestring, "MOUNT-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_MOUNT);
    }
    else if (strcmp(evt->valuestring, "MOUNT-UNPARKED") == 0) {
        if (toast_allowed(index, 4))
            ws_toast(index, TOAST_INFO, "Mount unparked");
        nina_event_log_add(EVENT_SEV_INFO, index, "Mount unparked");
    }
    else if (strcmp(evt->valuestring, "FOCUSER-CONNECTED") == 0) {
        record_connect_event(index, EQ_FOCUSER);
    }
    else if (strcmp(evt->valuestring, "FOCUSER-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_FOCUSER);
    }
    else if (strcmp(evt->valuestring, "FILTERWHEEL-CONNECTED") == 0) {
        record_connect_event(index, EQ_FILTERWHEEL);
    }
    else if (strcmp(evt->valuestring, "FILTERWHEEL-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_FILTERWHEEL);
    }
    else if (strcmp(evt->valuestring, "ROTATOR-CONNECTED") == 0) {
        record_connect_event(index, EQ_ROTATOR);
    }
    else if (strcmp(evt->valuestring, "ROTATOR-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_ROTATOR);
    }
    else if (strcmp(evt->valuestring, "SAFETY-CONNECTED") == 0) {
        record_connect_event(index, EQ_SAFETY);
    }
    else if (strcmp(evt->valuestring, "SAFETY-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_SAFETY);
    }
    else if (strcmp(evt->valuestring, "DOME-CONNECTED") == 0) {
        record_connect_event(index, EQ_DOME);
    }
    else if (strcmp(evt->valuestring, "DOME-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_DOME);
    }
    else if (strcmp(evt->valuestring, "DOME-SHUTTER-OPENED") == 0) {
        if (toast_allowed(index, 10))
            ws_toast(index, TOAST_INFO, "Dome shutter opened");
        nina_event_log_add(EVENT_SEV_INFO, index, "Dome shutter opened");
    }
    else if (strcmp(evt->valuestring, "DOME-SHUTTER-CLOSED") == 0) {
        if (toast_allowed(index, 10))
            ws_toast(index, TOAST_INFO, "Dome shutter closed");
        nina_event_log_add(EVENT_SEV_INFO, index, "Dome shutter closed");
    }
    else if (strcmp(evt->valuestring, "FLAT-CONNECTED") == 0) {
        record_connect_event(index, EQ_FLAT);
    }
    else if (strcmp(evt->valuestring, "FLAT-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_FLAT);
    }
    else if (strcmp(evt->valuestring, "FLAT-LIGHT-TOGGLED") == 0) {
        if (toast_allowed(index, 11))
            ws_toast(index, TOAST_INFO, "Flat light toggled");
        nina_event_log_add(EVENT_SEV_INFO, index, "Flat light toggled");
    }
    else if (strcmp(evt->valuestring, "FLAT-COVER-OPENED") == 0) {
        if (toast_allowed(index, 11))
            ws_toast(index, TOAST_INFO, "Flat cover opened");
        nina_event_log_add(EVENT_SEV_INFO, index, "Flat cover opened");
    }
    else if (strcmp(evt->valuestring, "FLAT-COVER-CLOSED") == 0) {
        if (toast_allowed(index, 11))
            ws_toast(index, TOAST_INFO, "Flat cover closed");
        nina_event_log_add(EVENT_SEV_INFO, index, "Flat cover closed");
    }
    else if (strcmp(evt->valuestring, "SWITCH-CONNECTED") == 0) {
        record_connect_event(index, EQ_SWITCH);
    }
    else if (strcmp(evt->valuestring, "SWITCH-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_SWITCH);
    }
    else if (strcmp(evt->valuestring, "WEATHER-CONNECTED") == 0) {
        record_connect_event(index, EQ_WEATHER);
    }
    else if (strcmp(evt->valuestring, "WEATHER-DISCONNECTED") == 0) {
        record_disconnect_event(index, EQ_WEATHER);
    }
    else if (strcmp(evt->valuestring, "CAMERA-DOWNLOAD-TIMEOUT") == 0) {
        if (toast_allowed(index, 1) || toast_allowed(index, 8))
            ws_toast(index, TOAST_ERROR, "Camera download timeout");
        nina_event_log_add(EVENT_SEV_ERROR, index, "Camera download timeout");
    }
    else if (strcmp(evt->valuestring, "SEQUENCE-ENTITY-FAILED") == 0) {
        cJSON *entity = cJSON_GetObjectItem(response, "Entity");
        cJSON *error_msg = cJSON_GetObjectItem(response, "Error");
        char msg[128];
        if (entity && entity->valuestring && error_msg && error_msg->valuestring) {
            snprintf(msg, sizeof(msg), "%s: %s", entity->valuestring, error_msg->valuestring);
        } else {
            snprintf(msg, sizeof(msg), "Sequence entity failed");
        }
        if (toast_allowed(index, 2) || toast_allowed(index, 8))
            ws_toast(index, TOAST_ERROR, msg);
        nina_event_log_add_fmt(EVENT_SEV_ERROR, index, "Entity failed: %s", msg);
    }
    else {
        ESP_LOGD(TAG, "WS[%d]: Unhandled event: %s", index, evt->valuestring);
    }

    // Record timestamp for WS-to-UI latency measurement
    if (g_perf.enabled) g_perf.last_ws_event_time_us = esp_timer_get_time();

    cJSON_Delete(json);

    // Wake UI coordinator for immediate UI refresh
    if (data_task_handle) {
        xTaskNotifyGive(data_task_handle);
    }
    // Wake the relevant poll task for immediate re-poll (e.g., sequence_poll_needed)
    if (index >= 0 && index < MAX_NINA_INSTANCES && poll_task_handles[index]) {
        xTaskNotifyGive(poll_task_handles[index]);
    }
}

/**
 * @brief WebSocket event handler callback.
 * handler_args carries the instance index as (void *)(intptr_t)index.
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    int index = (int)(intptr_t)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS[%d]: Connected to NINA", index);
        ws_backoff_ms[index] = WS_BACKOFF_INITIAL_MS;
        ws_needs_reconnect[index] = false;
        if (ws_client_data[index]) {
            if (nina_client_lock(ws_client_data[index], 50)) {
                ws_client_data[index]->websocket_connected = true;
                nina_connection_report_ws(index, true);
                nina_client_unlock(ws_client_data[index]);
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS[%d]: Disconnected from NINA", index);
        ws_disconnect_time_ms[index] = esp_timer_get_time() / 1000;
        ws_needs_reconnect[index] = true;
        if (ws_client_data[index]) {
            if (nina_client_lock(ws_client_data[index], 50)) {
                ws_client_data[index]->websocket_connected = false;
                nina_connection_report_ws(index, false);
                nina_client_unlock(ws_client_data[index]);
            }
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 && data->data_len > 0) {  // Text frame
            handle_websocket_message(index, (const char *)data->data_ptr, data->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS[%d]: Error", index);
        break;

    default:
        break;
    }
}

/**
 * @brief Build WebSocket URL from HTTP API base URL.
 * Converts "http://host:1888/v2/api/" -> "ws://host:1888/v2/socket"
 */
static bool build_ws_url(const char *base_url, char *ws_url, size_t ws_url_size) {
    const char *host_start = strstr(base_url, "://");
    if (!host_start) return false;
    host_start += 3;  // skip "://"

    // Find the path after host:port (look for /v2)
    const char *path_start = strstr(host_start, "/v2");
    if (!path_start) {
        path_start = strchr(host_start, '/');
    }

    if (path_start) {
        int host_len = path_start - host_start;
        snprintf(ws_url, ws_url_size, "ws://%.*s/v2/socket", host_len, host_start);
    } else {
        snprintf(ws_url, ws_url_size, "ws://%s/v2/socket", host_start);
    }

    return true;
}

void nina_websocket_start(int index, const char *base_url, nina_client_t *data) {
    if (index < 0 || index >= MAX_NINA_INSTANCES) return;

    if (ws_clients[index]) {
        ESP_LOGW(TAG, "WS[%d]: Already running, stopping first", index);
        nina_websocket_stop(index);
    }

    char ws_url[192];
    if (!build_ws_url(base_url, ws_url, sizeof(ws_url))) {
        ESP_LOGE(TAG, "WS[%d]: Failed to build WebSocket URL from %s", index, base_url);
        return;
    }

    ESP_LOGI(TAG, "WS[%d]: Connecting to %s", index, ws_url);

    ws_client_data[index] = data;
    ws_backoff_ms[index] = WS_BACKOFF_INITIAL_MS;
    ws_needs_reconnect[index] = false;

    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .reconnect_timeout_ms = 86400000, // Effectively disabled; reconnect managed externally with backoff
        .network_timeout_ms = 10000,
        .buffer_size = 2048,            // Reduced from 4096 to save heap; largest WS events ~1.2KB
    };

    ws_clients[index] = esp_websocket_client_init(&ws_cfg);
    if (!ws_clients[index]) {
        ESP_LOGE(TAG, "WS[%d]: Failed to init client", index);
        ws_client_data[index] = NULL;
        return;
    }

    esp_websocket_register_events(ws_clients[index], WEBSOCKET_EVENT_ANY,
                                   websocket_event_handler, (void *)(intptr_t)index);
    esp_websocket_client_start(ws_clients[index]);

    // Create aggregation timer if not yet created
    if (!s_agg_timers[index]) {
        esp_timer_create_args_t timer_args = {
            .callback = agg_timer_cb,
            .arg = (void *)(intptr_t)index,
            .name = "ws_agg"
        };
        esp_timer_create(&timer_args, &s_agg_timers[index]);
    }
    memset(&s_agg[index], 0, sizeof(connect_agg_state_t));
    /* Don't reset s_ever_connected here — it's seeded by the poll task
     * from /equipment/info and persists across WS reconnects. */
}

void nina_websocket_stop(int index) {
    if (index < 0 || index >= MAX_NINA_INSTANCES) return;

    if (ws_clients[index]) {
        esp_websocket_client_stop(ws_clients[index]);
        esp_websocket_client_destroy(ws_clients[index]);
        ws_clients[index] = NULL;
    }
    if (ws_client_data[index]) {
        if (nina_client_lock(ws_client_data[index], 50)) {
            ws_client_data[index]->websocket_connected = false;
            nina_connection_report_ws(index, false);
            nina_client_unlock(ws_client_data[index]);
        }
        ws_client_data[index] = NULL;
    }

    // Stop aggregation timer and reset state
    if (s_agg_timers[index]) {
        esp_timer_stop(s_agg_timers[index]);
    }
    memset(&s_agg[index], 0, sizeof(connect_agg_state_t));
}

void nina_websocket_stop_all(void) {
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        nina_websocket_stop(i);
    }
}

void nina_websocket_check_reconnect(int index, const char *base_url, nina_client_t *data) {
    if (index < 0 || index >= MAX_NINA_INSTANCES) return;
    if (!ws_needs_reconnect[index]) return;

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - ws_disconnect_time_ms[index] < ws_backoff_ms[index]) return;

    ESP_LOGD(TAG, "WS[%d]: Attempting reconnect (backoff: %d ms)", index, ws_backoff_ms[index]);

    // Double backoff for next failure, cap at 60 s
    ws_backoff_ms[index] = ws_backoff_ms[index] * 2;
    if (ws_backoff_ms[index] > WS_BACKOFF_MAX_MS)
        ws_backoff_ms[index] = WS_BACKOFF_MAX_MS;

    nina_websocket_stop(index);
    nina_websocket_start(index, base_url, data);
}

void nina_websocket_check_deferred_alerts(int index) {
    (void)index;
    // Legacy: camera disconnect grace period removed in toast overhaul.
    // Aggregation window now handles false disconnect→reconnect patterns.
}

bool nina_websocket_is_running(int index) {
    if (index < 0 || index >= MAX_NINA_INSTANCES) return false;
    return ws_clients[index] != NULL;
}

void nina_websocket_update_equipment_mask(int index, uint16_t connected_mask) {
    if (index < 0 || index >= MAX_NINA_INSTANCES) return;
    /* OR — accumulate bits. Equipment that was ever connected keeps its bit
     * even after disconnecting, so deferred disconnect toasts still fire.
     * Reset happens only on profile change or instance stop. */
    s_ever_connected[index] |= connected_mask;
}
