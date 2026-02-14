#include "nina_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

static const char *TAG = "nina_client";

// =============================================================================
// HTTP Helper Functions
// =============================================================================

static time_t my_timegm(struct tm *tm) {
    long t = 0;
    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon; // 0-11
    
    // Days in each month
    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    // Seconds per year
    for (int y = 1970; y < year; y++) {
        t += 365 * 24 * 3600;
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
            t += 24 * 3600; // Leap day
        }
    }
    
    // Seconds in current year
    for (int m = 0; m < mon; m++) {
        t += days_per_month[m] * 24 * 3600;
        if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
            t += 24 * 3600; // Leap day in Feb
        }
    }
    
    t += (tm->tm_mday - 1) * 24 * 3600;
    t += tm->tm_hour * 3600;
    t += tm->tm_min * 60;
    t += tm->tm_sec;
    
    return (time_t)t;
}

static time_t parse_iso8601(const char *str) {
    if (!str) return 0;
    struct tm tm = {0};
    // Format: 2024-08-09T15:12:38.425Z (we ignore sub-seconds for now)
    if (sscanf(str, "%d-%d-%dT%d:%d:%d", 
        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
        &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        return my_timegm(&tm); 
    }
    return 0;
}

static cJSON *http_get_json(const char *url) {
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "HTTP client fetch headers failed");
        esp_http_client_cleanup(client);
        return NULL;
    }

    char *buffer = malloc(content_length + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for HTTP response");
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total_read_len = 0, read_len;
    while (total_read_len < content_length) {
        read_len = esp_http_client_read(client, buffer + total_read_len, content_length - total_read_len);
        if (read_len <= 0) break;
        total_read_len += read_len;
    }
    buffer[total_read_len] = '\0';

    esp_http_client_cleanup(client);

    cJSON *json = cJSON_Parse(buffer);
    free(buffer);
    return json;
}

// =============================================================================
// Data Fetchers for Orbital Theme
// =============================================================================

static void fetch_camera_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/camera/info", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) {
        strcpy(data->status, "OFFLINE");
        return;
    }
    
    data->connected = true;
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        // Camera State
        cJSON *state = cJSON_GetObjectItem(response, "CameraState");
        if (state && state->valuestring) {
            strncpy(data->status, state->valuestring, sizeof(data->status) - 1);
            // Check if dithering
            data->is_dithering = (strcmp(state->valuestring, "Dithering") == 0);
        }
        
        // Temperature
        cJSON *temp = cJSON_GetObjectItem(response, "Temperature");
        if (temp) data->camera.temp = (float)temp->valuedouble;
        
        // Cooler Power
        cJSON *cooler = cJSON_GetObjectItem(response, "CoolerPower");
        if (cooler) data->camera.cooler_power = (float)cooler->valuedouble;
        
        // Exposure timing (Use IsExposing and ExposureEndTime)
        cJSON *is_exposing = cJSON_GetObjectItem(response, "IsExposing");
        cJSON *exp_end = cJSON_GetObjectItem(response, "ExposureEndTime");
        
        bool exposing = is_exposing && cJSON_IsTrue(is_exposing);
        time_t end_time = 0;
        
        if (exp_end && exp_end->valuestring) {
            end_time = parse_iso8601(exp_end->valuestring);
        }
        
        if (exposing && end_time > 0) {
            time_t now = time(NULL);
            double remaining = difftime(end_time, now);
            if (remaining < 0) remaining = 0;
            
            // We temporarily store negative remaining time in exposure_current
            // This will be fixed up in nina_client_get_data after we fetch exposure_total
            data->exposure_current = -remaining;
        } else {
            data->exposure_current = 0;
            // If not exposing, ensure total is 0 unless we find it elsewhere (which we will)
        }
    }
    
    cJSON_Delete(json);
}

static void traverse_sequence_state(cJSON *item, nina_client_t *data) {
    if (!item) return;

    // Check if this item has exposure info and seems active/relevant
    // We look for "ExposureTime" > 0.
    cJSON *exp_time = cJSON_GetObjectItem(item, "ExposureTime");
    cJSON *exp_count = cJSON_GetObjectItem(item, "ExposureCount");
    cJSON *iterations = cJSON_GetObjectItem(item, "DitherTargetExposures");
    if (!iterations) iterations = cJSON_GetObjectItem(item, "Iterations"); // Try Iterations too

    if (exp_time && exp_time->valuedouble > 0) {
        // Check if this item is likely the active one.
        // The active item in NINA sequence state usually doesn't have a simple "IsActive" flag
        // but we can check if it has progress.
        // For now, we take the last one we find (deepest) or one that matches our assumption.
        // Better heuristic: if we are exposing, this item might be the one.
        
        data->exposure_total = (float)exp_time->valuedouble;
        
        if (exp_count) {
            // NINA 0-based index? usually "Exposure 1 of 10" means index 0 -> 1.
            data->exposure_count = exp_count->valueint + 1; 
        }
        
        if (iterations) {
            data->exposure_iterations = iterations->valueint;
        }
    }

    // Recursively check "Items"
    cJSON *items = cJSON_GetObjectItem(item, "Items");
    if (items && cJSON_IsArray(items)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, items) {
             traverse_sequence_state(child, data);
        }
    }
}

static void fetch_sequence_state(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%ssequence/state", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        traverse_sequence_state(response, data);
    }
    
    cJSON_Delete(json);
}

static void fetch_sequence_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%ssequence/json", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response && cJSON_IsArray(response)) {
        // Search for Targets_Container
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, response) {
            cJSON *name = cJSON_GetObjectItem(item, "Name");
            if (name && name->valuestring && strcmp(name->valuestring, "Targets_Container") == 0) {
                // Found the targets container
                cJSON *items = cJSON_GetObjectItem(item, "Items");
                if (items && cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
                    // The first item in Targets_Container is usually the active target container
                    cJSON *target_container = cJSON_GetArrayItem(items, 0);
                    if (target_container) {
                        cJSON *target_name = cJSON_GetObjectItem(target_container, "Name");
                        if (target_name && target_name->valuestring) {
                            strncpy(data->target_name, target_name->valuestring, sizeof(data->target_name) - 1);
                            
                            // Strip "_Container" suffix if present
                            char *suffix = strstr(data->target_name, "_Container");
                            if (suffix) {
                                *suffix = '\0';
                            }
                        }
                    }
                }
                break; // Found targets, stop searching
            }
        }
    }
    
    cJSON_Delete(json);
}

static void fetch_guider_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/guider/info", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        cJSON *rms = cJSON_GetObjectItem(response, "RMSError");
        if (rms) {
            cJSON *total = cJSON_GetObjectItem(rms, "Total");
            if (total) {
                cJSON *arcsec = cJSON_GetObjectItem(total, "Arcseconds");
                if (arcsec) {
                    data->guider.rms_total = (float)arcsec->valuedouble;
                }
            }
        }
    }
    
    cJSON_Delete(json);
}

static void fetch_mount_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/mount/info", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        // Meridian flip time
        cJSON *flip_time = cJSON_GetObjectItem(response, "TimeToMeridianFlipString");
        if (flip_time && flip_time->valuestring) {
            strncpy(data->meridian_flip, flip_time->valuestring, sizeof(data->meridian_flip) - 1);
        } else {
            strcpy(data->meridian_flip, "--");
        }
    }
    
    cJSON_Delete(json);
}

static void fetch_image_history(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%simage-history", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response && cJSON_IsArray(response) && cJSON_GetArraySize(response) > 0) {
        // Get the most recent image (index 0)
        cJSON *latest = cJSON_GetArrayItem(response, 0);
        if (latest) {
            cJSON *hfr = cJSON_GetObjectItem(latest, "HFR");
            if (hfr) data->hfr = (float)hfr->valuedouble;
            
            cJSON *stars = cJSON_GetObjectItem(latest, "Stars");
            if (stars) data->stars = stars->valueint;
        }
    }
    
    cJSON_Delete(json);
}

static void fetch_profile_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sprofile/show", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response && cJSON_IsArray(response)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, response) {
            cJSON *isActive = cJSON_GetObjectItem(item, "IsActive");
            if (isActive && cJSON_IsTrue(isActive)) {
                cJSON *name = cJSON_GetObjectItem(item, "Name");
                if (name && name->valuestring) {
                    strncpy(data->profile_name, name->valuestring, sizeof(data->profile_name) - 1);
                }
                break;
            }
        }
    }
    cJSON_Delete(json);
}

static void fetch_filter_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/filterwheel/info", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        cJSON *selectedFilter = cJSON_GetObjectItem(response, "SelectedFilter");
        if (selectedFilter) {
            cJSON *name = cJSON_GetObjectItem(selectedFilter, "Name");
            if (name && name->valuestring) {
                strncpy(data->current_filter, name->valuestring, sizeof(data->current_filter) - 1);
            }
        }
    }
    cJSON_Delete(json);
}

static void fetch_focuser_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/focuser/info", base_url);
    
    cJSON *json = http_get_json(url);
    if (!json) return;
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        cJSON *position = cJSON_GetObjectItem(response, "Position");
        if (position) {
            data->focuser.position = position->valueint;
        }
    }
    cJSON_Delete(json);
}

// =============================================================================
// Public API
// =============================================================================

void nina_client_get_data(const char *base_url, nina_client_t *data) {
    // Initialize data structure
    memset(data, 0, sizeof(nina_client_t));
    data->connected = false;
    strcpy(data->target_name, "No Target");
    strcpy(data->status, "IDLE");
    strcpy(data->meridian_flip, "--");
    strcpy(data->profile_name, "NINA");
    strcpy(data->current_filter, "--");
    strcpy(data->time_remaining, "--");
    data->saturated_pixels = -1; // Not available in standard API
    
    // Fetch all data from NINA API
    fetch_camera_info(base_url, data);
    
    if (data->connected) {
        fetch_sequence_info(base_url, data);
        fetch_sequence_state(base_url, data);
        fetch_guider_info(base_url, data);
        fetch_mount_info(base_url, data);
        fetch_image_history(base_url, data);
        fetch_profile_info(base_url, data);
        fetch_filter_info(base_url, data);
        fetch_focuser_info(base_url, data);
        
        // Finalize exposure calculation
        // fetch_camera_info sets exposure_current to -remaining if exposing
        if (data->exposure_current < 0) {
            float remaining = -data->exposure_current;
            if (data->exposure_total > 0) {
                data->exposure_current = data->exposure_total - remaining;
            } else {
                data->exposure_current = 0;
            }
            
            // Format time remaining for target (simple approximation for now)
            // If we had a sequence remaining time, we'd use it.
            // For now, just show the exposure remaining time if exposing.
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
    
    ESP_LOGI(TAG, "NINA Data: connected=%d, profile=%s, target=%s, status=%s, filter=%s, temp=%.1f, rms=%.2f, hfr=%.2f, exp=%.1f/%.1f",
        data->connected, data->profile_name, data->target_name, data->status, data->current_filter, data->camera.temp, data->guider.rms_total, data->hfr, data->exposure_current, data->exposure_total);
}
