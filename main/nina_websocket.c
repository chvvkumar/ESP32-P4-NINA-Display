/**
 * @file nina_websocket.c
 * @brief WebSocket client lifecycle and event handling for NINA.
 *
 * Handles IMAGE-SAVE, FILTERWHEEL-CHANGED, SEQUENCE-FINISHED,
 * SEQUENCE-STARTING, GUIDER-DITHER, and GUIDER-START events.
 */

#include "nina_websocket.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "nina_ws";

static esp_websocket_client_handle_t ws_client = NULL;
static nina_client_t *ws_client_data = NULL;

/**
 * @brief Process incoming WebSocket JSON event from NINA
 */
static void handle_websocket_message(const char *payload, int len) {
    if (!ws_client_data || !payload || len <= 0) return;

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
        ESP_LOGI(TAG, "WS: IMAGE-SAVE event received");

        cJSON *stats = cJSON_GetObjectItem(response, "ImageStatistics");
        if (stats) {
            cJSON *hfr = cJSON_GetObjectItem(stats, "HFR");
            if (hfr) ws_client_data->hfr = (float)hfr->valuedouble;

            cJSON *stars = cJSON_GetObjectItem(stats, "Stars");
            if (stars) ws_client_data->stars = stars->valueint;

            cJSON *exp = cJSON_GetObjectItem(stats, "ExposureTime");
            if (exp) ws_client_data->exposure_total = (float)exp->valuedouble;

            // NOTE: Do NOT update current_filter from IMAGE-SAVE.
            // The filter wheel may have already moved to the next filter
            // by the time the image is saved. FILTERWHEEL-CHANGED is the
            // authoritative source for the current filter.

            cJSON *target = cJSON_GetObjectItem(stats, "TargetName");
            if (target && target->valuestring && target->valuestring[0] != '\0') {
                strncpy(ws_client_data->target_name, target->valuestring,
                        sizeof(ws_client_data->target_name) - 1);
            }

            cJSON *telescope = cJSON_GetObjectItem(stats, "TelescopeName");
            if (telescope && telescope->valuestring) {
                strncpy(ws_client_data->telescope_name, telescope->valuestring,
                        sizeof(ws_client_data->telescope_name) - 1);
            }

            ws_client_data->new_image_available = true;

            ESP_LOGI(TAG, "WS: HFR=%.2f Stars=%d Filter=%s Target=%s",
                ws_client_data->hfr, ws_client_data->stars,
                ws_client_data->current_filter, ws_client_data->target_name);
        }
    }
    // FILTERWHEEL-CHANGED: Replaces fetch_filter_robust_ex for current filter
    else if (strcmp(evt->valuestring, "FILTERWHEEL-CHANGED") == 0) {
        cJSON *new_f = cJSON_GetObjectItem(response, "New");
        if (new_f) {
            cJSON *name = cJSON_GetObjectItem(new_f, "Name");
            if (name && name->valuestring) {
                strncpy(ws_client_data->current_filter, name->valuestring,
                        sizeof(ws_client_data->current_filter) - 1);
                ESP_LOGI(TAG, "WS: Filter changed to %s", ws_client_data->current_filter);
            }
        }
    }
    // SEQUENCE-FINISHED: Mark sequence as done
    else if (strcmp(evt->valuestring, "SEQUENCE-FINISHED") == 0) {
        strcpy(ws_client_data->status, "FINISHED");
        ESP_LOGI(TAG, "WS: Sequence finished");
    }
    // SEQUENCE-STARTING: Mark sequence as running
    else if (strcmp(evt->valuestring, "SEQUENCE-STARTING") == 0) {
        strcpy(ws_client_data->status, "RUNNING");
        ESP_LOGI(TAG, "WS: Sequence starting");
    }
    // GUIDER-DITHER: Flag dithering state
    else if (strcmp(evt->valuestring, "GUIDER-DITHER") == 0) {
        ws_client_data->is_dithering = true;
        ESP_LOGI(TAG, "WS: Dithering");
    }
    // GUIDER-START: Clear dithering flag
    else if (strcmp(evt->valuestring, "GUIDER-START") == 0) {
        ws_client_data->is_dithering = false;
        ESP_LOGI(TAG, "WS: Guiding started");
    }
    else {
        ESP_LOGD(TAG, "WS: Unhandled event: %s", evt->valuestring);
    }

    cJSON_Delete(json);
}

/**
 * @brief WebSocket event handler callback
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS: Connected to NINA");
        if (ws_client_data) {
            ws_client_data->websocket_connected = true;
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS: Disconnected from NINA");
        if (ws_client_data) {
            ws_client_data->websocket_connected = false;
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 && data->data_len > 0) {  // Text frame
            handle_websocket_message((const char *)data->data_ptr, data->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS: Error");
        break;

    default:
        break;
    }
}

/**
 * @brief Build WebSocket URL from HTTP API base URL
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

void nina_websocket_start(const char *base_url, nina_client_t *data) {
    if (ws_client) {
        ESP_LOGW(TAG, "WS: Already running, stopping first");
        nina_websocket_stop();
    }

    char ws_url[192];
    if (!build_ws_url(base_url, ws_url, sizeof(ws_url))) {
        ESP_LOGE(TAG, "WS: Failed to build WebSocket URL from %s", base_url);
        return;
    }

    ESP_LOGI(TAG, "WS: Connecting to %s", ws_url);

    ws_client_data = data;

    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };

    ws_client = esp_websocket_client_init(&ws_cfg);
    if (!ws_client) {
        ESP_LOGE(TAG, "WS: Failed to init client");
        return;
    }

    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
                                   websocket_event_handler, NULL);
    esp_websocket_client_start(ws_client);
}

void nina_websocket_stop(void) {
    if (ws_client) {
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
    }
    if (ws_client_data) {
        ws_client_data->websocket_connected = false;
        ws_client_data = NULL;
    }
}
