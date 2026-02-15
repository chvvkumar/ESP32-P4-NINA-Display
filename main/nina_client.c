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
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_timer.h"
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
 * Provides: Current filter name from hardware AND available filters list
 */
static void fetch_filter_robust_ex(const char *base_url, nina_client_t *data, bool fetch_available) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/filterwheel/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response) {
        // Get current filter
        cJSON *selectedFilter = cJSON_GetObjectItem(response, "SelectedFilter");
        if (selectedFilter) {
            cJSON *name = cJSON_GetObjectItem(selectedFilter, "Name");
            if (name && name->valuestring) {
                strncpy(data->current_filter, name->valuestring, sizeof(data->current_filter) - 1);
                ESP_LOGI(TAG, "Filter (hardware): %s", data->current_filter);
            }
        }

        // Get available filters (only on first call)
        cJSON *availableFilters = fetch_available ? cJSON_GetObjectItem(response, "AvailableFilters") : NULL;
        if (availableFilters && cJSON_IsArray(availableFilters)) {
            int count = cJSON_GetArraySize(availableFilters);
            if (count > MAX_FILTERS) count = MAX_FILTERS;

            data->filter_count = 0;
            for (int i = 0; i < count; i++) {
                cJSON *filter = cJSON_GetArrayItem(availableFilters, i);
                if (filter) {
                    cJSON *filter_name = cJSON_GetObjectItem(filter, "Name");
                    cJSON *filter_id = cJSON_GetObjectItem(filter, "Id");

                    if (filter_name && filter_name->valuestring) {
                        strncpy(data->filters[i].name, filter_name->valuestring,
                                sizeof(data->filters[i].name) - 1);
                        data->filters[i].id = filter_id ? filter_id->valueint : i;
                        data->filter_count++;
                    }
                }
            }
            ESP_LOGI(TAG, "Found %d available filters", data->filter_count);
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

            // Telescope name - from image metadata
            cJSON *telescope = cJSON_GetObjectItem(latest, "TelescopeName");
            if (telescope && telescope->valuestring) {
                strncpy(data->telescope_name, telescope->valuestring, sizeof(data->telescope_name) - 1);
                ESP_LOGI(TAG, "Telescope (from image): %s", data->telescope_name);
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

// Find the active target container (RUNNING preferred, otherwise last FINISHED)
static cJSON* find_active_target_container(cJSON *targets_container) {
    cJSON *items = cJSON_GetObjectItem(targets_container, "Items");
    if (!items || !cJSON_IsArray(items)) return NULL;

    cJSON *running = NULL;
    cJSON *last_finished = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *status = cJSON_GetObjectItem(item, "Status");
        if (!status || !status->valuestring) continue;
        if (strcmp(status->valuestring, "RUNNING") == 0) {
            running = item;
            break;
        }
        if (strcmp(status->valuestring, "FINISHED") == 0) {
            last_finished = item;
        }
    }
    return running ? running : last_finished;
}

// Find the deepest RUNNING container name, or fall back to last FINISHED container
// A "container" is an item that has an "Items" array (not a leaf action)
static void find_active_container_name(cJSON *parent, char *out, size_t out_size) {
    if (!parent) return;

    cJSON *items = cJSON_GetObjectItem(parent, "Items");
    if (!items || !cJSON_IsArray(items)) return;

    // First pass: look for a RUNNING container child
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        cJSON *item_items = cJSON_GetObjectItem(item, "Items");
        if (!item_status || !item_status->valuestring || !item_name || !item_name->valuestring) continue;
        if (!item_items || !cJSON_IsArray(item_items)) continue;

        if (strcmp(item_status->valuestring, "RUNNING") == 0) {
            strncpy(out, item_name->valuestring, out_size - 1);
            out[out_size - 1] = '\0';
            char *suffix = strstr(out, "_Container");
            if (suffix) *suffix = '\0';
            // Try to find a deeper RUNNING container
            char deeper[64] = {0};
            find_active_container_name(item, deeper, sizeof(deeper));
            if (deeper[0] != '\0') {
                strncpy(out, deeper, out_size - 1);
                out[out_size - 1] = '\0';
            }
            return;
        }
    }

    // Second pass: no RUNNING container, use last FINISHED container
    cJSON *last_finished = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");
        cJSON *item_items = cJSON_GetObjectItem(item, "Items");
        if (!item_status || !item_status->valuestring) continue;
        if (!item_items || !cJSON_IsArray(item_items)) continue;
        if (strcmp(item_status->valuestring, "FINISHED") == 0) {
            last_finished = item;
        }
    }
    if (last_finished) {
        cJSON *item_name = cJSON_GetObjectItem(last_finished, "Name");
        if (item_name && item_name->valuestring) {
            strncpy(out, item_name->valuestring, out_size - 1);
            out[out_size - 1] = '\0';
            char *suffix = strstr(out, "_Container");
            if (suffix) *suffix = '\0';
        }
    }
}

// Find the currently running step (leaf instruction) within a container tree
// A "leaf" is an item that has no "Items" array (it's an action, not a container)
static void find_running_step_name(cJSON *parent, char *out, size_t out_size) {
    if (!parent) return;

    cJSON *items = cJSON_GetObjectItem(parent, "Items");
    if (!items || !cJSON_IsArray(items)) return;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_status = cJSON_GetObjectItem(item, "Status");
        cJSON *item_name = cJSON_GetObjectItem(item, "Name");
        if (!item_status || !item_status->valuestring || !item_name || !item_name->valuestring) continue;

        if (strcmp(item_status->valuestring, "RUNNING") == 0) {
            cJSON *child_items = cJSON_GetObjectItem(item, "Items");
            if (child_items && cJSON_IsArray(child_items) && cJSON_GetArraySize(child_items) > 0) {
                // This is a container - recurse deeper
                find_running_step_name(item, out, out_size);
                // If we found a deeper step, use that; otherwise use this container's name
                if (out[0] == '\0') {
                    strncpy(out, item_name->valuestring, out_size - 1);
                    out[out_size - 1] = '\0';
                }
            } else {
                // Leaf instruction - this is the current step
                strncpy(out, item_name->valuestring, out_size - 1);
                out[out_size - 1] = '\0';
            }
            return;
        }
    }
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
                // Find the active target container (RUNNING or last FINISHED)
                cJSON *target_container = find_active_target_container(item);
                if (!target_container) target_container = cJSON_GetArrayItem(items, 0);

                // Find the active container name (e.g., "LRGBSHO2")
                find_active_container_name(target_container, data->container_name, sizeof(data->container_name));
                if (data->container_name[0] != '\0') {
                    ESP_LOGI(TAG, "Active container: %s", data->container_name);
                }

                // Find the currently running step/instruction name
                data->container_step[0] = '\0';
                find_running_step_name(target_container, data->container_step, sizeof(data->container_step));
                if (data->container_step[0] != '\0') {
                    ESP_LOGI(TAG, "Active step: %s", data->container_step);
                }

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

/**
 * @brief Fetch switch/power info - Reads voltage, amps, watts, and PWM/dew heater outputs
 * Works with both SV241 (ReadonlySwitches: "Total Current", "Total Power", "pwm1", "pwm2")
 * and PPBA (ReadonlySwitches: "Amp", "Watt"; WritableSwitches: "Dew A"/"Dew B" with Max=100)
 */
static void fetch_switch_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/switch/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response) {
        cJSON_Delete(json);
        return;
    }

    cJSON *connected = cJSON_GetObjectItem(response, "Connected");
    if (!connected || !cJSON_IsTrue(connected)) {
        cJSON_Delete(json);
        return;
    }

    data->power.switch_connected = true;
    data->power.pwm_count = 0;

    // Parse ReadonlySwitches for voltage, amps, watts, and PWM readbacks
    // Order matters: check voltage, current, then PWM (by name OR description)
    // before watts/power to avoid PWM descriptions containing "power" being
    // misclassified as the total watts sensor.
    cJSON *readonly = cJSON_GetObjectItem(response, "ReadonlySwitches");
    if (readonly && cJSON_IsArray(readonly)) {
        cJSON *sw = NULL;
        cJSON_ArrayForEach(sw, readonly) {
            cJSON *name = cJSON_GetObjectItem(sw, "Name");
            cJSON *desc = cJSON_GetObjectItem(sw, "Description");
            cJSON *value = cJSON_GetObjectItem(sw, "Value");
            if (!name || !name->valuestring || !value) continue;

            const char *n = name->valuestring;
            const char *d = desc && desc->valuestring ? desc->valuestring : "";

            // Voltage: "Input Voltage" or description contains "Voltage"
            if (strcasecmp(n, "Input Voltage") == 0 || strstr(d, "oltage") || strstr(d, "Volts")) {
                data->power.input_voltage = (float)value->valuedouble;
            }
            // Current: "Total Current" or "Amp" or description contains "Current" or "Ampere"
            else if (strcasecmp(n, "Total Current") == 0 || strcasecmp(n, "Amp") == 0 ||
                     strstr(d, "urrent") || strstr(d, "Ampere")) {
                data->power.total_amps = (float)value->valuedouble;
                strncpy(data->power.amps_name, n, sizeof(data->power.amps_name) - 1);
            }
            // PWM readbacks: name starts with "pwm" OR description indicates PWM output
            // Must check BEFORE watts/power since PWM descriptions contain "power"
            else if ((strncasecmp(n, "pwm", 3) == 0 || strstr(d, "PWM") ||
                      strstr(d, "power output")) && data->power.pwm_count < 4) {
                int idx = data->power.pwm_count;
                data->power.pwm[idx] = (float)value->valuedouble;
                strncpy(data->power.pwm_names[idx], n, sizeof(data->power.pwm_names[idx]) - 1);
                data->power.pwm_count++;
            }
            // Power/Watts: "Total Power" or "Watt" or description contains "Watt"
            // Note: only match "Watt" not "ower"/"Power" to avoid catching PWM descriptions
            else if (strcasecmp(n, "Total Power") == 0 || strcasecmp(n, "Watt") == 0 ||
                     strstr(d, "Watt")) {
                data->power.total_watts = (float)value->valuedouble;
                strncpy(data->power.watts_name, n, sizeof(data->power.watts_name) - 1);
            }
        }
    }

    // Parse WritableSwitches for dew heaters, skipping names already seen in ReadonlySwitches
    cJSON *writable = cJSON_GetObjectItem(response, "WritableSwitches");
    if (writable && cJSON_IsArray(writable)) {
        cJSON *sw = NULL;
        cJSON_ArrayForEach(sw, writable) {
            cJSON *name = cJSON_GetObjectItem(sw, "Name");
            cJSON *desc = cJSON_GetObjectItem(sw, "Description");
            cJSON *value = cJSON_GetObjectItem(sw, "Value");
            cJSON *maximum = cJSON_GetObjectItem(sw, "Maximum");
            if (!name || !name->valuestring || !value || !maximum) continue;

            const char *n = name->valuestring;
            const char *d = desc && desc->valuestring ? desc->valuestring : "";
            int max_val = maximum->valueint;

            // Skip if this name already exists in the PWM list (ReadonlySwitches readback)
            bool duplicate = false;
            for (int i = 0; i < data->power.pwm_count; i++) {
                if (strcasecmp(data->power.pwm_names[i], n) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            if (max_val == 100 && data->power.pwm_count < 4) {
                // Percentage-based output (dew heater / PWM)
                int idx = data->power.pwm_count;
                data->power.pwm[idx] = (float)value->valuedouble;
                strncpy(data->power.pwm_names[idx], n, sizeof(data->power.pwm_names[idx]) - 1);
                data->power.pwm_count++;
            } else if (max_val > 1 &&
                       (strstr(d, "Dew") || strstr(d, "PWM") || strstr(d, "pwm")) &&
                       data->power.pwm_count < 4) {
                // Non-boolean output with PWM/Dew description — normalize to %
                int idx = data->power.pwm_count;
                data->power.pwm[idx] = (float)value->valuedouble * 100.0f / max_val;
                strncpy(data->power.pwm_names[idx], n, sizeof(data->power.pwm_names[idx]) - 1);
                data->power.pwm_count++;
            }
        }
    }

    ESP_LOGI(TAG, "Switch: %.1fV, %.2fA, %.1fW, %d PWM outputs",
        data->power.input_voltage, data->power.total_amps,
        data->power.total_watts, data->power.pwm_count);

    cJSON_Delete(json);
}

// =============================================================================
// WebSocket Client - Event-driven updates for IMAGE-SAVE, FILTERWHEEL-CHANGED
// =============================================================================

static esp_websocket_client_handle_t ws_client = NULL;
static nina_client_t *ws_client_data = NULL;

/**
 * @brief Process incoming WebSocket JSON event from NINA
 * Handles IMAGE-SAVE (replaces image-history polling) and
 * FILTERWHEEL-CHANGED (replaces filter polling) events.
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
            if (target && target->valuestring) {
                strncpy(ws_client_data->target_name, target->valuestring,
                        sizeof(ws_client_data->target_name) - 1);
            }

            cJSON *telescope = cJSON_GetObjectItem(stats, "TelescopeName");
            if (telescope && telescope->valuestring) {
                strncpy(ws_client_data->telescope_name, telescope->valuestring,
                        sizeof(ws_client_data->telescope_name) - 1);
            }

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
    // Extract host:port from base_url
    // base_url format: "http://hostname:port/v2/api/"
    const char *host_start = strstr(base_url, "://");
    if (!host_start) return false;
    host_start += 3;  // skip "://"

    // Find the path after host:port (look for /v2)
    const char *path_start = strstr(host_start, "/v2");
    if (!path_start) {
        // Try just finding first / after host
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
    data->filter_count = 0;

    ESP_LOGI(TAG, "=== Fetching NINA data (robust method) ===");

    // ALWAYS WORK - Structure independent
    fetch_camera_info_robust(base_url, data);

    if (data->connected) {
        fetch_filter_robust_ex(base_url, data, true);   // Hardware state + available filters
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

// =============================================================================
// Tiered Polling API
// =============================================================================

void nina_poll_state_init(nina_poll_state_t *state) {
    memset(state, 0, sizeof(nina_poll_state_t));
}

void nina_client_poll(const char *base_url, nina_client_t *data, nina_poll_state_t *state) {
    int64_t now_ms = esp_timer_get_time() / 1000;

    // Reset only volatile fields (preserve persistent data between polls)
    data->connected = false;
    data->exposure_current = 0;
    strcpy(data->status, "IDLE");
    strcpy(data->time_remaining, "--");
    data->saturated_pixels = -1;

    // On very first call, set defaults for persistent fields
    if (!state->static_fetched) {
        strcpy(data->target_name, "No Target");
        strcpy(data->meridian_flip, "--");
        strcpy(data->profile_name, "NINA");
        strcpy(data->current_filter, "--");
        data->filter_count = 0;
    }

    ESP_LOGI(TAG, "=== Polling NINA data (tiered) ===");

    // --- ALWAYS: Camera heartbeat ---
    fetch_camera_info_robust(base_url, data);

    if (!data->connected) {
        // Reset static cache on disconnect so we re-fetch on reconnect
        state->static_fetched = false;
        return;
    }

    // --- ONCE: Static data (profile, available filters, initial image history, switch) ---
    if (!state->static_fetched) {
        fetch_profile_robust(base_url, data);
        fetch_filter_robust_ex(base_url, data, true);  // Full: selected + available filters
        fetch_image_history_robust(base_url, data);
        fetch_switch_info(base_url, data);

        // Cache static data
        snprintf(state->cached_profile, sizeof(state->cached_profile), "%s", data->profile_name);
        snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
        memcpy(state->cached_filters, data->filters, sizeof(state->cached_filters));
        state->cached_filter_count = data->filter_count;

        state->static_fetched = true;
        state->last_slow_poll_ms = now_ms;
        state->last_sequence_poll_ms = now_ms;
    } else {
        // Restore cached static data
        snprintf(data->profile_name, sizeof(data->profile_name), "%s", state->cached_profile);
        if (state->cached_telescope[0] != '\0') {
            snprintf(data->telescope_name, sizeof(data->telescope_name), "%s", state->cached_telescope);
        }
        memcpy(data->filters, state->cached_filters, sizeof(data->filters));
        data->filter_count = state->cached_filter_count;
    }

    // --- FAST: Guider RMS - always poll (server won't push this) ---
    fetch_guider_robust(base_url, data);

    // --- CONDITIONAL: Only poll these if WebSocket is NOT handling them ---
    if (!data->websocket_connected) {
        // Image history (HFR, stars, target) - replaced by WS IMAGE-SAVE event
        fetch_image_history_robust(base_url, data);
        // Update cached telescope if image history provided a new one
        if (data->telescope_name[0] != '\0') {
            snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
        }
        // Filter current position - replaced by WS FILTERWHEEL-CHANGED event
        fetch_filter_robust_ex(base_url, data, false);
    } else {
        // When WS is connected, still update the telescope cache if WS provided it
        if (data->telescope_name[0] != '\0') {
            snprintf(state->cached_telescope, sizeof(state->cached_telescope), "%s", data->telescope_name);
        }
    }

    // --- SLOW: Focuser + Mount + Switch (every 30s) ---
    if (now_ms - state->last_slow_poll_ms >= NINA_POLL_SLOW_MS) {
        fetch_focuser_robust(base_url, data);
        fetch_mount_robust(base_url, data);
        fetch_switch_info(base_url, data);
        state->last_slow_poll_ms = now_ms;
    }

    // --- SLOW: Sequence counts (every 10s) ---
    if (now_ms - state->last_sequence_poll_ms >= NINA_POLL_SEQUENCE_MS) {
        fetch_sequence_counts_optional(base_url, data);
        state->last_sequence_poll_ms = now_ms;
    }

    // Fix exposure current/total calculation (same as original)
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

    ESP_LOGI(TAG, "=== Poll Summary ===");
    ESP_LOGI(TAG, "Connected: %d, Profile: %s", data->connected, data->profile_name);
    ESP_LOGI(TAG, "Target: %s, Filter: %s", data->target_name, data->current_filter);
    ESP_LOGI(TAG, "Exposure: %.1fs (%.1f/%.1f)", data->exposure_total, data->exposure_current, data->exposure_total);
    ESP_LOGI(TAG, "Guiding: %.2f\", HFR: %.2f, Stars: %d", data->guider.rms_total, data->hfr, data->stars);
}

void nina_client_poll_heartbeat(const char *base_url, nina_client_t *data) {
    // Background instances: only check camera info to maintain connection status
    data->connected = false;
    fetch_camera_info_robust(base_url, data);
    ESP_LOGD(TAG, "Heartbeat: connected=%d", data->connected);
}
