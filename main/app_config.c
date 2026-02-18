#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_config";
static app_config_t s_config;
static const char *NVS_NAMESPACE = "app_conf";

// Default RMS thresholds: good <= 0.5", ok <= 1.0", bad > 1.0" - DIMMED for Night Vision
static const char *DEFAULT_RMS_THRESHOLDS =
    "{\"good_max\":0.5,\"ok_max\":1.0,"
    "\"good_color\":\"#15803d\",\"ok_color\":\"#ca8a04\",\"bad_color\":\"#b91c1c\"}";

// Default HFR thresholds: good <= 2.0, ok <= 3.5, bad > 3.5 - DIMMED for Night Vision
static const char *DEFAULT_HFR_THRESHOLDS =
    "{\"good_max\":2.0,\"ok_max\":3.5,"
    "\"good_color\":\"#15803d\",\"ok_color\":\"#ca8a04\",\"bad_color\":\"#b91c1c\"}";


static void set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(app_config_t));
    strcpy(cfg->api_url_1, "http://astromele2.lan:1888/v2/api/");
    strcpy(cfg->api_url_2, "http://astromele3.lan:1888/v2/api/");
    strcpy(cfg->ntp_server, "pool.ntp.org");
    strcpy(cfg->filter_colors_1, "{}");
    strcpy(cfg->filter_colors_2, "{}");
    strcpy(cfg->filter_colors_3, "{}");
    strcpy(cfg->rms_thresholds_1, DEFAULT_RMS_THRESHOLDS);
    strcpy(cfg->rms_thresholds_2, DEFAULT_RMS_THRESHOLDS);
    strcpy(cfg->rms_thresholds_3, DEFAULT_RMS_THRESHOLDS);
    strcpy(cfg->hfr_thresholds_1, DEFAULT_HFR_THRESHOLDS);
    strcpy(cfg->hfr_thresholds_2, DEFAULT_HFR_THRESHOLDS);
    strcpy(cfg->hfr_thresholds_3, DEFAULT_HFR_THRESHOLDS);
    cfg->brightness = 50;
    cfg->color_brightness = 100;
    strcpy(cfg->mqtt_broker_url, "mqtt://192.168.1.250");
    strcpy(cfg->mqtt_topic_prefix, "ninadisplay");
    cfg->mqtt_port = 1883;
}

void app_config_init(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle! Using defaults.", esp_err_to_name(err));
        set_defaults(&s_config);
        return;
    }

    size_t required_size = sizeof(app_config_t);
    err = nvs_get_blob(my_handle, "config", &s_config, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config not found in NVS, using defaults");
        set_defaults(&s_config);

        // Save defaults so we have them next time
        nvs_set_blob(my_handle, "config", &s_config, sizeof(app_config_t));
        nvs_commit(my_handle);
    } else {
        // Config loaded successfully, but check if fields are valid
        bool needs_save = false;
        if (s_config.rms_thresholds_1[0] == '\0') {
            strcpy(s_config.rms_thresholds_1, DEFAULT_RMS_THRESHOLDS);
            needs_save = true;
        }
        if (s_config.rms_thresholds_2[0] == '\0') {
            strcpy(s_config.rms_thresholds_2, DEFAULT_RMS_THRESHOLDS);
            needs_save = true;
        }
        if (s_config.rms_thresholds_3[0] == '\0') {
            strcpy(s_config.rms_thresholds_3, DEFAULT_RMS_THRESHOLDS);
            needs_save = true;
        }
        if (s_config.hfr_thresholds_1[0] == '\0') {
            strcpy(s_config.hfr_thresholds_1, DEFAULT_HFR_THRESHOLDS);
            needs_save = true;
        }
        if (s_config.hfr_thresholds_2[0] == '\0') {
            strcpy(s_config.hfr_thresholds_2, DEFAULT_HFR_THRESHOLDS);
            needs_save = true;
        }
        if (s_config.hfr_thresholds_3[0] == '\0') {
            strcpy(s_config.hfr_thresholds_3, DEFAULT_HFR_THRESHOLDS);
            needs_save = true;
        }
        if (s_config.color_brightness < 0 || s_config.color_brightness > 100) {
            s_config.color_brightness = 100;
            needs_save = true;
        }
        if (s_config.theme_index < 0 || s_config.theme_index > 20) {
             s_config.theme_index = 0;
             needs_save = true;
        }
        if (s_config.brightness < 0 || s_config.brightness > 100) {
             s_config.brightness = 50;
             needs_save = true;
        }
        if (s_config.mqtt_topic_prefix[0] == '\0') {
            strcpy(s_config.mqtt_topic_prefix, "ninadisplay");
            needs_save = true;
        }
        if (s_config.mqtt_port == 0) {
            s_config.mqtt_port = 1883;
            needs_save = true;
        }
        if (needs_save) {
            nvs_set_blob(my_handle, "config", &s_config, sizeof(app_config_t));
            nvs_commit(my_handle);
        }
    }

    nvs_close(my_handle);
}

app_config_t *app_config_get(void) {
    return &s_config;
}

void app_config_save(const app_config_t *config) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle for saving: %s", esp_err_to_name(err));
        return;
    }

    // Update static copy
    memcpy(&s_config, config, sizeof(app_config_t));

    err = nvs_set_blob(my_handle, "config", &s_config, sizeof(app_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Config saved");
        nvs_commit(my_handle);
    }
    nvs_close(my_handle);
}

void app_config_factory_reset(void) {
    ESP_LOGW(TAG, "Performing factory reset - erasing NVS partition");

    // Erase the entire NVS partition
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
        return;
    }

    // Re-initialize NVS
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-initialize NVS: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Factory reset complete - all settings erased");

    // Reinitialize config with defaults
    app_config_init();
}

/**
 * @brief Parse a hex color string from a cJSON object field
 */
static uint32_t parse_color_field(cJSON *root, const char *field, uint32_t fallback) {
    cJSON *item = cJSON_GetObjectItem(root, field);
    if (item && cJSON_IsString(item) && item->valuestring) {
        const char *hex = item->valuestring;
        if (hex[0] == '#') hex++;
        return (uint32_t)strtol(hex, NULL, 16);
    }
    return fallback;
}

/**
 * @brief Get pointer to the filter_colors field for a given instance index
 */
static char *get_filter_colors_field(int instance_index) {
    switch (instance_index) {
        case 1: return s_config.filter_colors_2;
        case 2: return s_config.filter_colors_3;
        default: return s_config.filter_colors_1;
    }
}

/**
 * @brief Apply brightness scaling to a color
 * @param color 0xRRGGBB color value
 * @param brightness Brightness percentage 0-100
 * @return Adjusted color
 */
uint32_t app_config_apply_brightness(uint32_t color, int brightness) {
    if (brightness >= 100) return color;
    if (brightness <= 0) return 0x000000;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    r = (uint8_t)((r * brightness) / 100);
    g = (uint8_t)((g * brightness) / 100);
    b = (uint8_t)((b * brightness) / 100);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/**
 * @brief Get the color for a specific filter
 * @param filter_name Name of the filter (e.g., "Ha", "L", "R")
 * @return 32-bit color value (0xRRGGBB) or default blue if not found
 */
uint32_t app_config_get_filter_color(const char *filter_name, int instance_index) {
    if (!filter_name || filter_name[0] == '\0' || strcmp(filter_name, "--") == 0) {
        return 0x3b82f6;  // Default blue for unknown filter
    }

    // Parse the per-instance filter_colors JSON string
    const char *json = get_filter_colors_field(instance_index);
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse filter colors JSON, using default");
        return 0x3b82f6;  // Default blue
    }

    // Look up the filter by name
    cJSON *color_item = cJSON_GetObjectItem(root, filter_name);
    uint32_t color = 0x3b82f6;  // Default blue

    if (color_item && cJSON_IsString(color_item) && color_item->valuestring) {
        // Parse hex color string (e.g., "#60a5fa" or "60a5fa")
        const char *hex = color_item->valuestring;
        if (hex[0] == '#') hex++;  // Skip the '#' if present

        // Convert hex string to integer
        color = (uint32_t)strtol(hex, NULL, 16);
        ESP_LOGD(TAG, "Filter '%s' -> color 0x%06x", filter_name, (unsigned int)color);
    } else {
        ESP_LOGD(TAG, "Filter '%s' not found in config, using default blue", filter_name);
    }

    cJSON_Delete(root);

    // Apply global color brightness
    int gb = s_config.color_brightness;
    if (gb < 0 || gb > 100) gb = 100;
    color = app_config_apply_brightness(color, gb);

    return color;
}

/**
 * @brief Get the color for a guiding RMS value based on per-instance configured thresholds
 * @param rms_value The current RMS value in arcseconds
 * @param instance_index NINA instance index (0-based)
 * @return 32-bit color value (0xRRGGBB)
 */
uint32_t app_config_get_rms_color(float rms_value, int instance_index) {
    const char *json = (instance_index == 1) ? s_config.rms_thresholds_2 :
                       (instance_index == 2) ? s_config.rms_thresholds_3 :
                                               s_config.rms_thresholds_1;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return 0xf43f5e;  // Fallback rose
    }

    cJSON *good_max = cJSON_GetObjectItem(root, "good_max");
    cJSON *ok_max = cJSON_GetObjectItem(root, "ok_max");

    uint32_t good_color = parse_color_field(root, "good_color", 0x10b981);
    uint32_t ok_color   = parse_color_field(root, "ok_color",   0xeab308);
    uint32_t bad_color  = parse_color_field(root, "bad_color",  0xef4444);

    float good_threshold = (good_max && cJSON_IsNumber(good_max)) ? (float)good_max->valuedouble : 0.5f;
    float ok_threshold   = (ok_max && cJSON_IsNumber(ok_max))     ? (float)ok_max->valuedouble   : 1.0f;

    uint32_t result;
    if (rms_value <= good_threshold) {
        result = good_color;
    } else if (rms_value <= ok_threshold) {
        result = ok_color;
    } else {
        result = bad_color;
    }

    cJSON_Delete(root);

    // Apply global color brightness
    int gb = s_config.color_brightness;
    if (gb < 0 || gb > 100) gb = 100;
    result = app_config_apply_brightness(result, gb);

    return result;
}

/**
 * @brief Get the color for an HFR value based on per-instance configured thresholds
 * @param hfr_value The current HFR value
 * @param instance_index NINA instance index (0-based)
 * @return 32-bit color value (0xRRGGBB)
 */
uint32_t app_config_get_hfr_color(float hfr_value, int instance_index) {
    const char *json = (instance_index == 1) ? s_config.hfr_thresholds_2 :
                       (instance_index == 2) ? s_config.hfr_thresholds_3 :
                                               s_config.hfr_thresholds_1;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return 0x10b981;  // Fallback emerald
    }

    cJSON *good_max = cJSON_GetObjectItem(root, "good_max");
    cJSON *ok_max = cJSON_GetObjectItem(root, "ok_max");

    uint32_t good_color = parse_color_field(root, "good_color", 0x10b981);
    uint32_t ok_color   = parse_color_field(root, "ok_color",   0xeab308);
    uint32_t bad_color  = parse_color_field(root, "bad_color",  0xef4444);

    float good_threshold = (good_max && cJSON_IsNumber(good_max)) ? (float)good_max->valuedouble : 2.0f;
    float ok_threshold   = (ok_max && cJSON_IsNumber(ok_max))     ? (float)ok_max->valuedouble   : 3.5f;

    uint32_t result;
    if (hfr_value <= good_threshold) {
        result = good_color;
    } else if (hfr_value <= ok_threshold) {
        result = ok_color;
    } else {
        result = bad_color;
    }

    cJSON_Delete(root);

    // Apply global color brightness
    int gb = s_config.color_brightness;
    if (gb < 0 || gb > 100) gb = 100;
    result = app_config_apply_brightness(result, gb);

    return result;
}

/**
 * @brief Check if a filter name exists in the API filter list
 */
static bool filter_in_api_list(const char *name, const char *filter_names[], int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(name, filter_names[i]) == 0) return true;
    }
    return false;
}

/**
 * @brief Sync NVS filter_colors and filter_brightness with the actual filter list from the NINA API.
 *
 * - Adds entries for any API filters missing from the config (default color / brightness)
 * - Removes stale entries for filters no longer in the API
 * - Saves to NVS only if something changed
 */
void app_config_sync_filters(const char *filter_names[], int count, int instance_index) {
    if (count <= 0) return;

    bool needs_save = false;
    char *field = get_filter_colors_field(instance_index);

    // --- Sync filter_colors for this instance ---
    cJSON *colors = cJSON_Parse(field);
    if (!colors) colors = cJSON_CreateObject();

    // Add missing filters
    for (int i = 0; i < count; i++) {
        if (!cJSON_GetObjectItem(colors, filter_names[i])) {
            cJSON_AddStringToObject(colors, filter_names[i], "#3b82f6");
            ESP_LOGI(TAG, "Filter sync: added color for '%s' (default blue)", filter_names[i]);
            needs_save = true;
        }
    }

    // Remove stale filters
    cJSON *child = colors->child;
    while (child) {
        cJSON *next = child->next;
        if (!filter_in_api_list(child->string, filter_names, count)) {
            ESP_LOGI(TAG, "Filter sync: removed stale color for '%s'", child->string);
            cJSON_DeleteItemFromObject(colors, child->string);
            needs_save = true;
        }
        child = next;
    }

    if (needs_save) {
        char *json_str = cJSON_PrintUnformatted(colors);
        if (json_str) {
            if (strlen(json_str) < 512) {
                strcpy(field, json_str);
            } else {
                ESP_LOGW(TAG, "Filter colors JSON too large (%d bytes), not updating", (int)strlen(json_str));
            }
            free(json_str);
        }
    }
    cJSON_Delete(colors);

    if (needs_save) {
        app_config_save(&s_config);
        ESP_LOGI(TAG, "Filter config synced with API and saved to NVS");
    } else {
        ESP_LOGI(TAG, "Filter config already in sync with API");
    }
}

int app_config_get_instance_count(void) {
    int count = 0;
    if (s_config.api_url_1[0] != '\0') count++;
    if (s_config.api_url_2[0] != '\0') count++;
    if (s_config.api_url_3[0] != '\0') count++;
    return count > 0 ? count : 1;  // Always at least 1
}

const char *app_config_get_instance_url(int index) {
    switch (index) {
        case 0: return s_config.api_url_1;
        case 1: return s_config.api_url_2;
        case 2: return s_config.api_url_3;
        default: return "";
    }
}
