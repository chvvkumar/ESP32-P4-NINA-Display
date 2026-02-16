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
    if (!str) {
        ESP_LOGW(TAG, "parse_iso8601: NULL string");
        return 0;
    }
    
    ESP_LOGD(TAG, "Parsing ISO8601: %s", str);
    
    struct tm tm = {0};
    int tz_hours = 0, tz_mins = 0;
    char tz_sign = '+';
    
    // Format: 2026-02-13T19:15:42.2124088-06:00 or 2024-08-09T15:12:38.425Z
    // Try with timezone offset first
    int parsed = sscanf(str, "%d-%d-%dT%d:%d:%d.%*d%c%d:%d", 
        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
        &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
        &tz_sign, &tz_hours, &tz_mins);
    
    if (parsed < 6) {
        // Try without subseconds
        parsed = sscanf(str, "%d-%d-%dT%d:%d:%d%c%d:%d", 
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
            &tz_sign, &tz_hours, &tz_mins);
    }
    
    if (parsed >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        
        time_t utc_time = my_timegm(&tm);
        
        // Apply timezone offset to convert to UTC
        if (parsed >= 9) {
            int tz_offset_seconds = (tz_hours * 3600 + tz_mins * 60);
            if (tz_sign == '-') {
                utc_time += tz_offset_seconds;  // If time is UTC-06:00, add 6 hours to get UTC
            } else {
                utc_time -= tz_offset_seconds;  // If time is UTC+06:00, subtract 6 hours to get UTC
            }
            ESP_LOGD(TAG, "  Parsed with timezone: %c%02d:%02d, UTC timestamp: %ld", 
                     tz_sign, tz_hours, tz_mins, (long)utc_time);
        } else {
            ESP_LOGD(TAG, "  Parsed without timezone, UTC timestamp: %ld", (long)utc_time);
        }
        
        return utc_time;
    }
    
    ESP_LOGW(TAG, "Failed to parse ISO8601 date: %s", str);
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
        
        // Exposure timing (Use IsExposing and ExposureEndTime if available)
        cJSON *is_exposing = cJSON_GetObjectItem(response, "IsExposing");
        cJSON *exp_end = cJSON_GetObjectItem(response, "ExposureEndTime");
        
        if (is_exposing) {
            bool exposing = cJSON_IsTrue(is_exposing);
            time_t end_time = 0;
            
            if (exp_end && exp_end->valuestring) {
                end_time = parse_iso8601(exp_end->valuestring);
            }
            
            if (exposing && end_time > 0) {
                time_t now = time(NULL);
                
                // Check if system time is valid (after year 2020)
                if (now < 1577836800) {  // Jan 1, 2020 00:00:00 UTC
                    ESP_LOGE(TAG, "System time not set! Current time: %ld (before 2020). Cannot calculate exposure progress.", (long)now);
                    ESP_LOGE(TAG, "Please ensure NTP time synchronization is working.");
                    data->exposure_current = 0;
                } else {
                    double remaining = difftime(end_time, now);
                    
                    ESP_LOGD(TAG, "Exposure timing: now=%ld, end_time=%ld, diff=%.1fs", 
                             (long)now, (long)end_time, remaining);
                    
                    // Sanity check: remaining should be between 0 and 7200 seconds (2 hours)
                    if (remaining < 0 || remaining > 7200) {
                        ESP_LOGW(TAG, "Exposure time out of range (%.1fs), ignoring", remaining);
                        data->exposure_current = 0;
                    } else {
                        // We temporarily store negative remaining time in exposure_current
                        // This will be fixed up in nina_client_get_data after we fetch exposure_total
                        data->exposure_current = -remaining;
                        ESP_LOGI(TAG, "Camera exposing: %.1fs remaining", remaining);
                    }
                }
            } else {
                data->exposure_current = 0;
            }
        } else {
            // IsExposing field not available (older NINA API or different camera driver)
            ESP_LOGW(TAG, "Camera API does not provide IsExposing/ExposureEndTime fields");
            data->exposure_current = 0;
        }
    }
    
    cJSON_Delete(json);
}

static void traverse_sequence_state(cJSON *item, nina_client_t *data) {
    if (!item) return;

    // Check for "Take Exposure" instruction (actual imaging)
    cJSON *name = cJSON_GetObjectItem(item, "Name");
    cJSON *status = cJSON_GetObjectItem(item, "Status");
    
    // Debug: Log each item we process
    if (name && name->valuestring) {
        ESP_LOGD(TAG, "Processing item: %s (Status: %s)", 
                 name->valuestring, 
                 (status && status->valuestring) ? status->valuestring : "UNKNOWN");
    }

    // Check for "Smart Exposure_Container" to get loop iterations
    // Must match exactly "Smart Exposure_Container" (with underscore and Container suffix)
    if (name && name->valuestring && 
        (strcmp(name->valuestring, "Smart Exposure_Container") == 0 ||
         strstr(name->valuestring, "Smart Exposure_Container") != NULL)) {
        ESP_LOGI(TAG, "Found Smart Exposure_Container: %s", name->valuestring);
        
        // Debug: Print the full JSON object (first 500 chars)
        char *json_str = cJSON_PrintUnformatted(item);
        if (json_str) {
            int len = strlen(json_str);
            if (len > 500) {
                char temp[501];
                strncpy(temp, json_str, 500);
                temp[500] = '\0';
                ESP_LOGI(TAG, "  Item JSON (truncated): %s", temp);
            } else {
                ESP_LOGI(TAG, "  Item JSON: %s", json_str);
            }
            free(json_str);
        }
        
        // Get CompletedIterations from the Loop condition
        cJSON *conditions = cJSON_GetObjectItem(item, "Conditions");
        ESP_LOGI(TAG, "  Conditions pointer: %p, is array: %d", 
                 conditions, conditions ? cJSON_IsArray(conditions) : 0);
        
        if (conditions && cJSON_IsArray(conditions)) {
            ESP_LOGI(TAG, "  Has %d conditions", cJSON_GetArraySize(conditions));
            cJSON *cond = NULL;
            cJSON_ArrayForEach(cond, conditions) {
                cJSON *cond_name = cJSON_GetObjectItem(cond, "Name");
                if (cond_name && cond_name->valuestring) {
                    ESP_LOGI(TAG, "    Condition: %s", cond_name->valuestring);
                }

                // Check for loop iterations in any condition (name may be "_Condition" or "Loop For Iterations")
                cJSON *completed = cJSON_GetObjectItem(cond, "CompletedIterations");
                cJSON *iterations = cJSON_GetObjectItem(cond, "Iterations");

                if (completed && cJSON_IsNumber(completed) && iterations && cJSON_IsNumber(iterations)) {
                    data->exposure_count = completed->valueint;
                    data->exposure_iterations = iterations->valueint;
                    ESP_LOGI(TAG, "      Loop iterations: %d/%d", data->exposure_count, data->exposure_iterations);
                }
            }
        } else {
            ESP_LOGI(TAG, "  No conditions array found");
        }
    }
    
    // Check for unnamed items with ExposureTime (from sequence/state)
    // These items have Name: null but contain ExposureTime and ExposureCount fields
    cJSON *exp_time_field = cJSON_GetObjectItem(item, "ExposureTime");
    cJSON *exp_count_field = cJSON_GetObjectItem(item, "ExposureCount");

    if (exp_time_field && cJSON_IsNumber(exp_time_field) && exp_count_field) {
        // This is an exposure instruction item (unnamed)
        bool is_running = (status && status->valuestring && strcmp(status->valuestring, "RUNNING") == 0);

        ESP_LOGI(TAG, "Found exposure item (unnamed): ExposureTime=%.1f, ExposureCount=%d, Status=%s",
                 exp_time_field->valuedouble,
                 exp_count_field->valueint,
                 status && status->valuestring ? status->valuestring : "UNKNOWN");

        // Prioritize RUNNING status, but also accept CREATED/FINISHED as fallback
        bool should_parse = is_running || (data->exposure_total == 0);

        if (should_parse) {
            data->exposure_total = (float)exp_time_field->valuedouble;
            ESP_LOGI(TAG, "  Set exposure_total = %.1fs (from unnamed item)", data->exposure_total);
        }
    }

    // Legacy check for named "Take Exposure" instruction (keep for compatibility)
    if (name && name->valuestring && strcmp(name->valuestring, "Take Exposure") == 0) {
        ESP_LOGI(TAG, "Found 'Take Exposure' instruction");

        bool is_running = (status && status->valuestring && strcmp(status->valuestring, "RUNNING") == 0);
        bool should_parse = is_running || (data->exposure_total == 0);

        if (should_parse) {
            cJSON *exp_time = cJSON_GetObjectItem(item, "ExposureTime");
            if (exp_time && cJSON_IsNumber(exp_time)) {
                data->exposure_total = (float)exp_time->valuedouble;
                ESP_LOGI(TAG, "  Set exposure_total = %.1fs", data->exposure_total);
            }
        }
    }
    
    // Check for "Switch Filter" instruction to get filter name
    if (name && name->valuestring && strcmp(name->valuestring, "Switch Filter") == 0) {
        cJSON *filter = cJSON_GetObjectItem(item, "Filter");
        if (filter) {
            cJSON *filter_name = cJSON_GetObjectItem(filter, "_name");
            if (filter_name && filter_name->valuestring) {
                strncpy(data->current_filter, filter_name->valuestring, sizeof(data->current_filter) - 1);
                ESP_LOGI(TAG, "Found filter: %s", data->current_filter);
            }
        }
    }

    // Check for loop conditions with RemainingTime
    cJSON *conditions = cJSON_GetObjectItem(item, "Conditions");
    if (conditions && cJSON_IsArray(conditions)) {
        cJSON *cond = NULL;
        cJSON_ArrayForEach(cond, conditions) {
            cJSON *cond_name = cJSON_GetObjectItem(cond, "Name");
            if (cond_name && cond_name->valuestring &&
                strstr(cond_name->valuestring, "Loop Until Time") != NULL) {
                cJSON *remaining = cJSON_GetObjectItem(cond, "RemainingTime");
                if (remaining && remaining->valuestring) {
                    // Format: "04:33:57.7989913" -> Copy first 8 chars (HH:MM:SS)
                    strncpy(data->time_remaining, remaining->valuestring,
                           sizeof(data->time_remaining) - 1);
                    // Truncate at first '.' if present
                    char *dot = strchr(data->time_remaining, '.');
                    if (dot) *dot = '\0';
                }
            }
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
    
    ESP_LOGI(TAG, "Fetching sequence/state...");
    cJSON *json = http_get_json(url);
    if (!json) {
        ESP_LOGW(TAG, "Failed to fetch sequence/state");
        return;
    }
    
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response && cJSON_IsArray(response)) {
        int array_size = cJSON_GetArraySize(response);
        ESP_LOGI(TAG, "sequence/state Response has %d items", array_size);
        
        cJSON *item = NULL;
        int index = 0;
        cJSON_ArrayForEach(item, response) {
            ESP_LOGD(TAG, "Processing response array item %d", index++);
            traverse_sequence_state(item, data);
        }
    } else {
        ESP_LOGW(TAG, "sequence/state Response is not an array or missing");
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
                    ESP_LOGI(TAG, "Found Targets_Container with %d items", cJSON_GetArraySize(items));

                    // Get the first item as the target container (e.g., "M 31_Container")
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
                            ESP_LOGI(TAG, "Target: %s", data->target_name);
                        }

                        // Recursively search for RUNNING Smart Exposure
                        cJSON *running_exp = find_running_smart_exposure(target_container);

                        if (running_exp) {
                            ESP_LOGI(TAG, "Found RUNNING Smart Exposure");

                            // Extract filter name
                            cJSON *filter = cJSON_GetObjectItem(running_exp, "Filter");
                            if (filter && filter->valuestring) {
                                strncpy(data->current_filter, filter->valuestring, sizeof(data->current_filter) - 1);
                                ESP_LOGI(TAG, "  Filter: %s", data->current_filter);
                            }

                            // Get exposure time (duration of a single exposure in seconds)
                            cJSON *exp_time = cJSON_GetObjectItem(running_exp, "ExposureTime");
                            if (exp_time && cJSON_IsNumber(exp_time)) {
                                data->exposure_total = (float)exp_time->valuedouble;
                                ESP_LOGI(TAG, "  ExposureTime: %.1fs", data->exposure_total);
                            }

                            // Get completed iterations (how many exposures completed)
                            cJSON *completed = cJSON_GetObjectItem(running_exp, "CompletedIterations");
                            if (completed && cJSON_IsNumber(completed)) {
                                data->exposure_count = completed->valueint;
                                ESP_LOGI(TAG, "  CompletedIterations: %d", data->exposure_count);
                            }

                            // Get total iterations (total planned exposures)
                            cJSON *iterations = cJSON_GetObjectItem(running_exp, "Iterations");
                            if (iterations && cJSON_IsNumber(iterations)) {
                                data->exposure_iterations = iterations->valueint;
                                ESP_LOGI(TAG, "  Iterations: %d", data->exposure_iterations);
                            }
                        } else {
                            ESP_LOGW(TAG, "No RUNNING Smart Exposure found");
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
                    ESP_LOGI(TAG, "Active profile: %s", data->profile_name);
                }
                break;
            }
        }
    }
    cJSON_Delete(json);
}

static void fetch_filter_info(const char *base_url, nina_client_t *data) {
    // Only fetch filter from filterwheel if we don't have it from sequence
    if (data->current_filter[0] != '\0' && strcmp(data->current_filter, "--") != 0) {
        return; // Already have filter from Smart Exposure
    }

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
    data->target_time_remaining[0] = '\0';
    
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
    
    ESP_LOGI(TAG, "NINA Data: connected=%d, profile=%s, target=%s, status=%s",
        data->connected, data->profile_name, data->target_name, data->status);
    ESP_LOGI(TAG, "  Filter: %s, Exposure: %.1fs (%.1f/%.1f), Count: %d/%d",
        data->current_filter, data->exposure_total, data->exposure_current, data->exposure_total,
        data->exposure_count, data->exposure_iterations);
    ESP_LOGI(TAG, "  Camera: %.1fC, Guiding: %.2f\", HFR: %.2f, Stars: %d",
        data->camera.temp, data->guider.rms_total, data->hfr, data->stars);
}
