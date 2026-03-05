/**
 * @file allsky_client.c
 * @brief AllSky API HTTP client — polls /all endpoint and extracts configured field values.
 *
 * Uses esp_http_client in standalone mode (no keep-alive reuse) since AllSky
 * polling runs at low frequency (5-60 s) and only hits a single endpoint.
 */

#include "allsky_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "allsky_client";

/* Maximum response size for /all endpoint (16 KB should be plenty) */
#define ALLSKY_RESPONSE_BUF_SIZE  16384

/* HTTP timeout for AllSky requests */
#define ALLSKY_HTTP_TIMEOUT_MS    10000

// =============================================================================
// Mutex Helpers
// =============================================================================

void allsky_data_init(allsky_data_t *data) {
    memset(data, 0, sizeof(allsky_data_t));
    data->connected = false;
    data->last_poll_ms = 0;
    data->mutex = xSemaphoreCreateMutex();
}

bool allsky_data_lock(allsky_data_t *data, int timeout_ms) {
    if (!data || !data->mutex) return false;
    return xSemaphoreTake(data->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void allsky_data_unlock(allsky_data_t *data) {
    if (data && data->mutex) {
        xSemaphoreGive(data->mutex);
    }
}

// =============================================================================
// JSON Key Resolution — walks dot-notation paths into nested cJSON objects
// =============================================================================

/**
 * Resolve a dot-notation key path (e.g. "pistatus.AS_CPUTEMP") into
 * a cJSON tree and return the value as a string.
 *
 * Supports: cJSON_String, cJSON_Number, cJSON_True/False.
 * Returns NULL if the path does not exist or the root is NULL.
 *
 * Uses a function-local static buffer for number formatting — safe because
 * this is only called from a single task context (the AllSky poll task).
 */
static const char *resolve_json_key(cJSON *root, const char *dotpath) {
    static char num_buf[32];

    if (!root || !dotpath || dotpath[0] == '\0') return NULL;

    /* Work on a mutable copy of the dotpath so we can split on '.' */
    char path_copy[128];
    size_t len = strlen(dotpath);
    if (len >= sizeof(path_copy)) return NULL;
    memcpy(path_copy, dotpath, len + 1);

    cJSON *node = root;
    char *token = path_copy;
    char *dot;

    while ((dot = strchr(token, '.')) != NULL) {
        *dot = '\0';
        node = cJSON_GetObjectItem(node, token);
        if (!node) return NULL;
        token = dot + 1;
    }

    /* Final segment */
    node = cJSON_GetObjectItem(node, token);
    if (!node) return NULL;

    if (cJSON_IsString(node)) {
        return node->valuestring;
    }

    if (cJSON_IsNumber(node)) {
        double val = node->valuedouble;
        if (val == (double)(int)val) {
            snprintf(num_buf, sizeof(num_buf), "%d", (int)val);
        } else {
            snprintf(num_buf, sizeof(num_buf), "%.2f", val);
        }
        return num_buf;
    }

    if (cJSON_IsBool(node)) {
        return cJSON_IsTrue(node) ? "true" : "false";
    }

    return NULL;
}

// =============================================================================
// Field Extraction — maps quadrant config to field indices
// =============================================================================

/**
 * Quadrant-to-field-index mapping table.
 *
 * Each entry maps a quadrant name + sub-field name to an ALLSKY_F_* index.
 * The "key" JSON field within each sub-field provides the dot-notation path
 * to resolve against the AllSky API response.
 */
typedef struct {
    const char *quadrant;
    const char *field;
    int         index;
} field_map_entry_t;

static const field_map_entry_t s_field_map[] = {
    { "thermal", "main", ALLSKY_F_THERMAL_MAIN },
    { "thermal", "sub1", ALLSKY_F_THERMAL_SUB1 },
    { "thermal", "sub2", ALLSKY_F_THERMAL_SUB2 },
    { "sqm",     "main", ALLSKY_F_SQM_MAIN     },
    { "sqm",     "sub1", ALLSKY_F_SQM_SUB1     },
    { "sqm",     "sub2", ALLSKY_F_SQM_SUB2     },
    { "ambient", "main", ALLSKY_F_AMBIENT_MAIN  },
    { "ambient", "sub1", ALLSKY_F_AMBIENT_SUB1  },
    { "ambient", "sub2", ALLSKY_F_AMBIENT_SUB2  },
    { "ambient", "dot1", ALLSKY_F_AMBIENT_DOT1  },
    { "ambient", "dot2", ALLSKY_F_AMBIENT_DOT2  },
    { "sqm",     "dot1", ALLSKY_F_SQM_DOT1      },
    { "power",   "main", ALLSKY_F_POWER_MAIN    },
    { "power",   "sub1", ALLSKY_F_POWER_SUB1    },
    { "power",   "sub2", ALLSKY_F_POWER_SUB2    },
};

#define FIELD_MAP_COUNT  (sizeof(s_field_map) / sizeof(s_field_map[0]))

/**
 * Extract field values from the AllSky API response JSON using the
 * field_config_json mapping.
 *
 * field_config_json format:
 * {
 *   "thermal": { "main": {"key":"pistatus.AS_CPUTEMP"}, "sub1": {"key":"..."}, "sub2": {"key":"..."} },
 *   "sqm":     { "main": {"key":"..."}, ... },
 *   "ambient": { "main": {"key":"..."}, "sub1": {...}, "sub2": {...}, "dot1": {...}, "dot2": {...} },
 *   "power":   { "main": {"key":"..."}, ... }
 * }
 */
static void extract_fields(cJSON *api_data, const char *field_config_json, allsky_data_t *data) {
    if (!field_config_json || field_config_json[0] == '\0') {
        ESP_LOGD(TAG, "No field config JSON provided, skipping extraction");
        return;
    }

    cJSON *config = cJSON_Parse(field_config_json);
    if (!config) {
        ESP_LOGW(TAG, "Failed to parse field_config_json");
        return;
    }

    for (size_t i = 0; i < FIELD_MAP_COUNT; i++) {
        int idx = s_field_map[i].index;

        /* Navigate: config[quadrant][field] */
        cJSON *quadrant_obj = cJSON_GetObjectItem(config, s_field_map[i].quadrant);
        if (!quadrant_obj) {
            data->field_values[idx][0] = '\0';
            continue;
        }

        cJSON *field_obj = cJSON_GetObjectItem(quadrant_obj, s_field_map[i].field);
        if (!field_obj) {
            data->field_values[idx][0] = '\0';
            continue;
        }

        /* Extract the "key" string — the dot-notation path into api_data */
        cJSON *key_item = cJSON_GetObjectItem(field_obj, "key");
        if (!key_item || !cJSON_IsString(key_item) || key_item->valuestring[0] == '\0') {
            data->field_values[idx][0] = '\0';
            continue;
        }

        const char *value = resolve_json_key(api_data, key_item->valuestring);
        if (value) {
            snprintf(data->field_values[idx], sizeof(data->field_values[idx]), "%s", value);
        } else {
            data->field_values[idx][0] = '\0';
            ESP_LOGD(TAG, "Key '%s' not found in API response", key_item->valuestring);
        }
    }

    cJSON_Delete(config);
}

// =============================================================================
// Public API — Poll AllSky endpoint
// =============================================================================

void allsky_client_poll(const char *hostname, const char *field_config_json, allsky_data_t *data) {
    if (!hostname || hostname[0] == '\0' || !data) {
        return;
    }

    /* Construct URL: http://{hostname}/all */
    char url[256];
    snprintf(url, sizeof(url), "http://%s/all", hostname);

    ESP_LOGD(TAG, "Polling AllSky: %s", url);

    /* Configure HTTP client — standalone mode (no keep-alive reuse) */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = ALLSKY_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGW(TAG, "Failed to init HTTP client for AllSky");
        if (allsky_data_lock(data, 100)) {
            data->connected = false;
            allsky_data_unlock(data);
        }
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AllSky HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (allsky_data_lock(data, 100)) {
            data->connected = false;
            allsky_data_unlock(data);
        }
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGW(TAG, "AllSky HTTP fetch headers failed");
        esp_http_client_cleanup(client);
        if (allsky_data_lock(data, 100)) {
            data->connected = false;
            allsky_data_unlock(data);
        }
        return;
    }

    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "AllSky HTTP %d for %s", status, url);
        esp_http_client_cleanup(client);
        if (allsky_data_lock(data, 100)) {
            data->connected = false;
            allsky_data_unlock(data);
        }
        return;
    }

    /* Determine buffer size: use content_length if available, else fixed max */
    int buf_size = (content_length > 0) ? (content_length + 1) : ALLSKY_RESPONSE_BUF_SIZE;
    if (buf_size > ALLSKY_RESPONSE_BUF_SIZE) {
        buf_size = ALLSKY_RESPONSE_BUF_SIZE;
    }

    char *buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for AllSky response", buf_size);
        esp_http_client_cleanup(client);
        if (allsky_data_lock(data, 100)) {
            data->connected = false;
            allsky_data_unlock(data);
        }
        return;
    }

    /* Read response body */
    int total_read = 0, read_len;
    int max_read = buf_size - 1;  /* Leave room for null terminator */
    while (total_read < max_read) {
        read_len = esp_http_client_read(client, buffer + total_read, max_read - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    buffer[total_read] = '\0';

    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGW(TAG, "AllSky: empty response body");
        free(buffer);
        if (allsky_data_lock(data, 100)) {
            data->connected = false;
            allsky_data_unlock(data);
        }
        return;
    }

    /* Parse JSON response */
    cJSON *json = cJSON_Parse(buffer);
    free(buffer);

    if (!json) {
        ESP_LOGW(TAG, "AllSky: failed to parse JSON response");
        if (allsky_data_lock(data, 100)) {
            data->connected = false;
            allsky_data_unlock(data);
        }
        return;
    }

    /* Extract field values into a local copy, then copy under mutex */
    allsky_data_t local_data;
    memset(&local_data, 0, sizeof(local_data));
    extract_fields(json, field_config_json, &local_data);

    cJSON_Delete(json);

    /* Update shared data under mutex */
    if (allsky_data_lock(data, 200)) {
        data->connected = true;
        data->last_poll_ms = esp_timer_get_time() / 1000;
        memcpy(data->field_values, local_data.field_values, sizeof(data->field_values));
        allsky_data_unlock(data);
        ESP_LOGD(TAG, "AllSky poll OK — %d bytes parsed", total_read);
    } else {
        ESP_LOGW(TAG, "AllSky: failed to acquire mutex for data update");
    }
}
