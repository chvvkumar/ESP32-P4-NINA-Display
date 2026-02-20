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
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include "perf_monitor.h"
#include "tasks.h"

static const char *TAG = "nina_ws";

#define WS_BACKOFF_INITIAL_MS  5000
#define WS_BACKOFF_MAX_MS      60000

static esp_websocket_client_handle_t ws_clients[MAX_NINA_INSTANCES];
static nina_client_t *ws_client_data[MAX_NINA_INSTANCES];

// Exponential backoff state per instance
static int ws_backoff_ms[MAX_NINA_INSTANCES];
static int64_t ws_disconnect_time_ms[MAX_NINA_INSTANCES];
static bool ws_needs_reconnect[MAX_NINA_INSTANCES];

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

    // IMAGE-SAVE: Replaces fetch_image_history_robust for HFR, Stars, Filter, Target
    if (strcmp(evt->valuestring, "IMAGE-SAVE") == 0) {
        ESP_LOGI(TAG, "WS[%d]: IMAGE-SAVE event received", index);

        cJSON *stats = cJSON_GetObjectItem(response, "ImageStatistics");
        if (stats) {
            // Extract values from JSON before taking the lock
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
                data->new_image_available = true;
                data->ui_refresh_needed = true;
                nina_client_unlock(data);
            } else {
                ESP_LOGW(TAG, "WS[%d]: Could not acquire mutex for IMAGE-SAVE", index);
            }

            ESP_LOGI(TAG, "WS[%d]: HFR=%.2f Stars=%d Filter=%s Target=%s",
                index, new_hfr, new_stars,
                data->current_filter, data->target_name);
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
        ESP_LOGI(TAG, "WS[%d]: Sequence finished", index);
    }
    // SEQUENCE-STARTING: Mark sequence as running
    else if (strcmp(evt->valuestring, "SEQUENCE-STARTING") == 0) {
        if (nina_client_lock(data, 50)) {
            strcpy(data->status, "RUNNING");
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        ESP_LOGI(TAG, "WS[%d]: Sequence starting", index);
    }
    // GUIDER-DITHER: Flag dithering state
    else if (strcmp(evt->valuestring, "GUIDER-DITHER") == 0) {
        if (nina_client_lock(data, 50)) {
            data->is_dithering = true;
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
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
            data->ui_refresh_needed = true;
            nina_client_unlock(data);
        }
        ESP_LOGI(TAG, "WS[%d]: New target: %s", index,
                 target_name && target_name->valuestring ? target_name->valuestring : "(null)");
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
        ws_client_data[index]->websocket_connected = false;
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
