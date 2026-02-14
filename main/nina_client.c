/**
 * @file nina_client.c
 * @brief Robust NINA Client - Structure-independent data fetching
 * @version 2.0
 *
 * This version uses simple, flat API endpoints that work regardless of
 * sequence structure complexity. Falls back to sequence parsing only for
 * non-critical data (exposure count/iterations).
 */

#include "nina_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

static const char *TAG = "nina_client";

// =============================================================================
// HTTP Helper Functions (unchanged)
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

static time_t parse_iso8601(const char *str) {
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

static cJSON *http_get_json(const char *url) {
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    char *buffer = malloc(content_length + 1);
    if (!buffer) {
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
// ROBUST DATA FETCHERS - Structure Independent
// =============================================================================

/**
 * @brief Fetch camera info - ALWAYS WORKS
 * Provides: IsExposing, ExposureEndTime, Temperature, CoolerPower, CameraState
 */
static void fetch_camera_info_robust(const char *base_url, nina_client_t *data) {
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
        // Camera state
        cJSON *state = cJSON_GetObjectItem(response, "CameraState");
        if (state && state->valuestring) {
            strncpy(data->status, state->valuestring, sizeof(data->status) - 1);
        }

        // Temperature
        cJSON *temp = cJSON_GetObjectItem(response, "Temperature");
        if (temp) data->camera.temp = (float)temp->valuedouble;

        // Cooler power
        cJSON *cooler = cJSON_GetObjectItem(response, "CoolerPower");
        if (cooler) data->camera.cooler_power = (float)cooler->valuedouble;

        // Exposure status
        cJSON *is_exposing = cJSON_GetObjectItem(response, "IsExposing");
        cJSON *exp_end = cJSON_GetObjectItem(response, "ExposureEndTime");

        if (is_exposing && cJSON_IsTrue(is_exposing) && exp_end && exp_end->valuestring) {
            time_t end_time = parse_iso8601(exp_end->valuestring);
            time_t now = time(NULL);

            if (now > 1577836800 && end_time > 0) {  // Year 2020+
                double remaining = difftime(end_time, now);
                if (remaining >= 0 && remaining <= 7200) {  // Sanity check: 0-2 hours
                    // Store negative remaining time (will be fixed after getting total exposure)
                    data->exposure_current = -remaining;
                    ESP_LOGI(TAG, "Camera exposing: %.1fs remaining", remaining);
                }
            }
        }
    }

    cJSON_Delete(json);
}

/**
 * @brief Fetch filter wheel info - ALWAYS WORKS
 * Provides: Current filter name from hardware
 */
static void fetch_filter_robust(const char *base_url, nina_client_t *data) {
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
                ESP_LOGI(TAG, "Filter (hardware): %s", data->current_filter);
            }
        }
    }
    cJSON_Delete(json);
}

/**
 * @brief Fetch image history - ALWAYS WORKS
 * Provides: TargetName, ExposureTime, Filter, HFR, Stars from last completed image
 */
static void fetch_image_history_robust(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%simage-history", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response && cJSON_IsArray(response) && cJSON_GetArraySize(response) > 0) {
        cJSON *latest = cJSON_GetArrayItem(response, 0);
        if (latest) {
            // Target name - RELIABLE!
            cJSON *target = cJSON_GetObjectItem(latest, "TargetName");
            if (target && target->valuestring) {
                strncpy(data->target_name, target->valuestring, sizeof(data->target_name) - 1);
                ESP_LOGI(TAG, "Target (from image): %s", data->target_name);
            }

            // Exposure time from last image (use as fallback if not exposing)
            cJSON *exp_time = cJSON_GetObjectItem(latest, "ExposureTime");
            if (exp_time && data->exposure_total == 0) {
                data->exposure_total = (float)exp_time->valuedouble;
                ESP_LOGI(TAG, "ExposureTime (from image): %.1fs", data->exposure_total);
            }

            // Filter name (fallback if filter wheel didn't provide it)
            if (data->current_filter[0] == '\0' || strcmp(data->current_filter, "--") == 0) {
                cJSON *filter = cJSON_GetObjectItem(latest, "Filter");
                if (filter && filter->valuestring) {
                    strncpy(data->current_filter, filter->valuestring, sizeof(data->current_filter) - 1);
                    ESP_LOGI(TAG, "Filter (from image): %s", data->current_filter);
                }
            }

            // HFR
            cJSON *hfr = cJSON_GetObjectItem(latest, "HFR");
            if (hfr) data->hfr = (float)hfr->valuedouble;

            // Stars
            cJSON *stars = cJSON_GetObjectItem(latest, "Stars");
            if (stars) data->stars = stars->valueint;

            ESP_LOGI(TAG, "Image stats: HFR=%.2f, Stars=%d", data->hfr, data->stars);
        }
    }

    cJSON_Delete(json);
}

/**
 * @brief Fetch profile info - ALWAYS WORKS
 */
static void fetch_profile_robust(const char *base_url, nina_client_t *data) {
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
                    ESP_LOGI(TAG, "Profile: %s", data->profile_name);
                }
                break;
            }
        }
    }
    cJSON_Delete(json);
}

/**
 * @brief Fetch guider info - ALWAYS WORKS
 */
static void fetch_guider_robust(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/guider/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        cJSON *rms = cJSON_GetObjectItem(response, "RMSError");
        if (rms) {
            // Total RMS
            cJSON *total = cJSON_GetObjectItem(rms, "Total");
            if (total) {
                cJSON *arcsec = cJSON_GetObjectItem(total, "Arcseconds");
                if (arcsec) {
                    data->guider.rms_total = (float)arcsec->valuedouble;
                }
            }

            // RA RMS
            cJSON *ra = cJSON_GetObjectItem(rms, "RA");
            if (ra) {
                cJSON *arcsec = cJSON_GetObjectItem(ra, "Arcseconds");
                if (arcsec) {
                    data->guider.rms_ra = (float)arcsec->valuedouble;
                }
            }

            // DEC RMS
            cJSON *dec = cJSON_GetObjectItem(rms, "Dec");
            if (dec) {
                cJSON *arcsec = cJSON_GetObjectItem(dec, "Arcseconds");
                if (arcsec) {
                    data->guider.rms_dec = (float)arcsec->valuedouble;
                }
            }

            ESP_LOGI(TAG, "Guiding RMS - Total: %.2f\", RA: %.2f\", DEC: %.2f\"",
                data->guider.rms_total, data->guider.rms_ra, data->guider.rms_dec);
        }
    }

    cJSON_Delete(json);
}

/**
 * @brief Fetch mount info - ALWAYS WORKS
 */
static void fetch_mount_robust(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/mount/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        cJSON *flip_time = cJSON_GetObjectItem(response, "TimeToMeridianFlipString");
        if (flip_time && flip_time->valuestring) {
            strncpy(data->meridian_flip, flip_time->valuestring, sizeof(data->meridian_flip) - 1);
        }
    }

    cJSON_Delete(json);
}

// Helper function to recursively search for RUNNING Smart Exposure
static cJSON* find_running_smart_exposure(cJSON *container) {
    if (!container) return NULL;

    cJSON *items = cJSON_GetObjectItem(container, "Items");
    if (!items || !cJSON_IsArray(items)) return NULL;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");

        // Check if this is a RUNNING Smart Exposure
        if (item_name && item_name->valuestring &&
            strcmp(item_name->valuestring, "Smart Exposure") == 0) {
            if (item_status && item_status->valuestring &&
                strcmp(item_status->valuestring, "RUNNING") == 0) {
                return item;  // Found it!
            }
        }

        // Recursively search in nested containers
        cJSON *nested = find_running_smart_exposure(item);
        if (nested) return nested;
    }

    return NULL;
}

/**
 * @brief Try to get exposure count/iterations/time from sequence (OPTIONAL - may fail)
 * This is the only part that depends on sequence structure
 * Provides: ExposureTime, CompletedIterations, Iterations from RUNNING Smart Exposure
 */
static void fetch_sequence_counts_optional(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%ssequence/json", base_url);

    cJSON *json = http_get_json(url);
    if (!json) {
        ESP_LOGW(TAG, "Sequence data unavailable - exposure counts will not be shown");
        return;
    }

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response || !cJSON_IsArray(response)) {
        cJSON_Delete(json);
        return;
    }

    // Search for Targets_Container
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, response) {
        cJSON *name = cJSON_GetObjectItem(item, "Name");
        if (name && name->valuestring && strcmp(name->valuestring, "Targets_Container") == 0) {
            cJSON *items = cJSON_GetObjectItem(item, "Items");
            if (items && cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
                // Get first target container
                cJSON *target_container = cJSON_GetArrayItem(items, 0);

                // Recursively search for RUNNING Smart Exposure
                cJSON *running_exp = find_running_smart_exposure(target_container);

                if (running_exp) {
                    // Get completed iterations
                    cJSON *completed = cJSON_GetObjectItem(running_exp, "CompletedIterations");
                    if (completed && cJSON_IsNumber(completed)) {
                        data->exposure_count = completed->valueint;
                    }

                    // Get total iterations
                    cJSON *iterations = cJSON_GetObjectItem(running_exp, "Iterations");
                    if (iterations && cJSON_IsNumber(iterations)) {
                        data->exposure_iterations = iterations->valueint;
                    }

                    // Get CURRENT exposure time from running exposure (overrides image history)
                    cJSON *exp_time = cJSON_GetObjectItem(running_exp, "ExposureTime");
                    if (exp_time && cJSON_IsNumber(exp_time)) {
                        data->exposure_total = (float)exp_time->valuedouble;
                        ESP_LOGI(TAG, "ExposureTime (from running sequence): %.1fs", data->exposure_total);
                    }

                    ESP_LOGI(TAG, "Exposure count: %d/%d", data->exposure_count, data->exposure_iterations);
                } else {
                    ESP_LOGD(TAG, "No RUNNING Smart Exposure found (sequence may be idle)");
                }
            }
            break;
        }
    }

    cJSON_Delete(json);
}

/**
 * @brief Fetch focuser info
 */
static void fetch_focuser_robust(const char *base_url, nina_client_t *data) {
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
    // Initialize
    memset(data, 0, sizeof(nina_client_t));
    data->connected = false;
    strcpy(data->target_name, "No Target");
    strcpy(data->status, "IDLE");
    strcpy(data->meridian_flip, "--");
    strcpy(data->profile_name, "NINA");
    strcpy(data->current_filter, "--");
    strcpy(data->time_remaining, "--");
    data->saturated_pixels = -1;

    ESP_LOGI(TAG, "=== Fetching NINA data (robust method) ===");

    // ALWAYS WORK - Structure independent
    fetch_camera_info_robust(base_url, data);

    if (data->connected) {
        fetch_filter_robust(base_url, data);           // Hardware state
        fetch_image_history_robust(base_url, data);    // Target, exposure time, HFR, stars
        fetch_profile_robust(base_url, data);          // Profile name
        fetch_guider_robust(base_url, data);           // Guiding RMS
        fetch_mount_robust(base_url, data);            // Meridian flip
        fetch_focuser_robust(base_url, data);          // Focuser position

        // Optional - may not work with all sequence types
        fetch_sequence_counts_optional(base_url, data);

        // Fix exposure current/total calculation
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

    ESP_LOGI(TAG, "=== Data Summary ===");
    ESP_LOGI(TAG, "Connected: %d, Profile: %s", data->connected, data->profile_name);
    ESP_LOGI(TAG, "Target: %s, Filter: %s", data->target_name, data->current_filter);
    ESP_LOGI(TAG, "Exposure: %.1fs (%.1f/%.1f)", data->exposure_total, data->exposure_current, data->exposure_total);
    ESP_LOGI(TAG, "Camera: %.1fC (%.0f%% power)", data->camera.temp, data->camera.cooler_power);
    ESP_LOGI(TAG, "Guiding: %.2f\", HFR: %.2f, Stars: %d", data->guider.rms_total, data->hfr, data->stars);
}
