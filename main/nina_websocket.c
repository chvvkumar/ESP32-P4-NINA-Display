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

// Exponential backoff state per instance
static int ws_backoff_ms[MAX_NINA_INSTANCES];
static int64_t ws_disconnect_time_ms[MAX_NINA_INSTANCES];
static bool ws_needs_reconnect[MAX_NINA_INSTANCES];

// Deferred camera-disconnect toast: suppress if CAMERA-CONNECTED follows quickly
#define CAMERA_DISCONNECT_GRACE_MS 3000
static int64_t s_camera_disconnect_pending_ms[MAX_NINA_INSTANCES] = {0};

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
            ws_toast(index, TOAST_SUCCESS, "Safe");
            nina_event_log_add(EVENT_SEV_SUCCESS, index, "Observatory is safe");
        } else {
            ws_toast(index, TOAST_ERROR, "UNSAFE");
            nina_event_log_add(EVENT_SEV_ERROR, index, "Observatory UNSAFE!");
            nina_alert_trigger(ALERT_SAFETY, index, 0);
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
        ws_toast(index, TOAST_WARNING, "Guider stopped");
        nina_event_log_add(EVENT_SEV_WARNING, index, "Guider stopped");
        ESP_LOGI(TAG, "WS[%d]: Guider stopped", index);
    }
    // PROFILE-CHANGED: Invalidate cached static data to force re-fetch
    else if (strcmp(evt->valuestring, "PROFILE-CHANGED") == 0) {
        ESP_LOGI(TAG, "WS[%d]: Profile changed, will re-fetch static data", index);
        if (nina_client_lock(data, 50)) {
            data->sequence_poll_needed = true;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
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
        ws_toast(index, TOAST_ERROR, "Autofocus failed");
        nina_event_log_add(EVENT_SEV_ERROR, index, "Autofocus failed");
        ESP_LOGW(TAG, "WS[%d]: Autofocus failed", index);
    }
    // CAMERA-CONNECTED: Camera connected — cancel any pending disconnect toast
    else if (strcmp(evt->valuestring, "CAMERA-CONNECTED") == 0) {
        s_camera_disconnect_pending_ms[index] = 0;
        ws_toast(index, TOAST_SUCCESS, "Camera connected");
        nina_event_log_add(EVENT_SEV_INFO, index, "Camera connected");
        ESP_LOGI(TAG, "WS[%d]: Camera connected", index);
    }
    // CAMERA-DISCONNECTED: Defer toast — NINA fires this before CAMERA-CONNECTED
    // during a fresh connect sequence. Only show if no connect follows within 3s.
    else if (strcmp(evt->valuestring, "CAMERA-DISCONNECTED") == 0) {
        s_camera_disconnect_pending_ms[index] = esp_timer_get_time() / 1000;
        ESP_LOGW(TAG, "WS[%d]: Camera disconnect pending (grace %dms)", index, CAMERA_DISCONNECT_GRACE_MS);
    }
    // MOUNT-SLEWING: Mount is slewing to target
    else if (strcmp(evt->valuestring, "MOUNT-SLEWING") == 0) {
        ws_toast(index, TOAST_INFO, "Mount slewing to target");
        ESP_LOGI(TAG, "WS[%d]: Mount slewing", index);
    }
    // MOUNT-PARKED: Mount parked
    else if (strcmp(evt->valuestring, "MOUNT-PARKED") == 0) {
        ws_toast(index, TOAST_INFO, "Mount parked");
        nina_event_log_add(EVENT_SEV_INFO, index, "Mount parked");
        ESP_LOGI(TAG, "WS[%d]: Mount parked", index);
    }
    // MOUNT-HOMED: Mount homed
    else if (strcmp(evt->valuestring, "MOUNT-HOMED") == 0) {
        ws_toast(index, TOAST_INFO, "Mount homed");
        ESP_LOGI(TAG, "WS[%d]: Mount homed", index);
    }
    // MOUNT-TRACKING-ON: Tracking started
    else if (strcmp(evt->valuestring, "MOUNT-TRACKING-ON") == 0) {
        ws_toast(index, TOAST_SUCCESS, "Tracking started");
        ESP_LOGI(TAG, "WS[%d]: Tracking started", index);
    }
    // MOUNT-TRACKING-OFF: Tracking stopped
    else if (strcmp(evt->valuestring, "MOUNT-TRACKING-OFF") == 0) {
        ws_toast(index, TOAST_WARNING, "Tracking stopped");
        nina_event_log_add(EVENT_SEV_WARNING, index, "Tracking stopped");
        ESP_LOGW(TAG, "WS[%d]: Tracking stopped", index);
    }
    // ERROR*: Any error event
    else if (strncmp(evt->valuestring, "ERROR", 5) == 0) {
        ws_toast_fmt(index, TOAST_ERROR, "Error: %s", evt->valuestring);
        nina_event_log_add_fmt(EVENT_SEV_ERROR, index, "Error: %s", evt->valuestring);
        ESP_LOGE(TAG, "WS[%d]: Error event: %s", index, evt->valuestring);
    }
    else {
        ESP_LOGD(TAG, "WS[%d]: Unhandled event: %s", index, evt->valuestring);
    }

    // Record timestamp for WS-to-UI latency measurement
#if PERF_MONITOR_ENABLED
    g_perf.last_ws_event_time_us = esp_timer_get_time();
#endif

    cJSON_Delete(json);

    // Wake data task for immediate UI refresh
    if (data_task_handle) {
        xTaskNotifyGive(data_task_handle);
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
        .reconnect_timeout_ms = 0,      // Disable auto-reconnect; managed externally with backoff
        .network_timeout_ms = 10000,
        .buffer_size = 4096,            // IMAGE-SAVE events with ImageStatistics can exceed 1KB default
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
    if (index < 0 || index >= MAX_NINA_INSTANCES) return;
    if (s_camera_disconnect_pending_ms[index] == 0) return;

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_camera_disconnect_pending_ms[index] >= CAMERA_DISCONNECT_GRACE_MS) {
        s_camera_disconnect_pending_ms[index] = 0;
        ws_toast(index, TOAST_ERROR, "Camera disconnected");
        nina_event_log_add(EVENT_SEV_ERROR, index, "Camera disconnected");
        ESP_LOGW(TAG, "WS[%d]: Camera disconnect confirmed (no reconnect within grace period)", index);
    }
}
