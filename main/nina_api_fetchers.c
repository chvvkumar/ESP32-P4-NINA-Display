/**
 * @file nina_api_fetchers.c
 * @brief Individual REST API endpoint fetch functions for NINA.
 *
 * Each function fetches data from a single NINA API endpoint and
 * populates the corresponding fields in nina_client_t.
 */

#include "nina_api_fetchers.h"
#include "nina_client_internal.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nina_fetch";

/**
 * @brief Fetch camera info - ALWAYS WORKS
 * Provides: IsExposing, ExposureEndTime, Temperature, CoolerPower, CameraState
 */
void fetch_camera_info_robust(const char *base_url, nina_client_t *data) {
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
void fetch_filter_robust_ex(const char *base_url, nina_client_t *data, bool fetch_available) {
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
void fetch_image_history_robust(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%simage-history", base_url);

    // Snapshot previous values to detect new images
    float prev_hfr = data->hfr;
    int prev_stars = data->stars;

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response && cJSON_IsArray(response) && cJSON_GetArraySize(response) > 0) {
        cJSON *latest = cJSON_GetArrayItem(response, 0);
        if (latest) {
            // Target name from last saved image (only if non-empty)
            cJSON *target = cJSON_GetObjectItem(latest, "TargetName");
            if (target && target->valuestring && target->valuestring[0] != '\0') {
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

            // Detect new image (HFR or stars changed)
            if (data->hfr != prev_hfr || data->stars != prev_stars) {
                data->new_image_available = true;
            }

            ESP_LOGI(TAG, "Image stats: HFR=%.2f, Stars=%d", data->hfr, data->stars);
        }
    }

    cJSON_Delete(json);
}

/**
 * @brief Fetch profile info - ALWAYS WORKS
 */
void fetch_profile_robust(const char *base_url, nina_client_t *data) {
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
void fetch_guider_robust(const char *base_url, nina_client_t *data) {
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
void fetch_mount_robust(const char *base_url, nina_client_t *data) {
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

/**
 * @brief Fetch focuser info
 */
void fetch_focuser_robust(const char *base_url, nina_client_t *data) {
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
 */
void fetch_switch_info(const char *base_url, nina_client_t *data) {
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

            // Voltage
            if (strcasecmp(n, "Input Voltage") == 0 || strstr(d, "oltage") || strstr(d, "Volts")) {
                data->power.input_voltage = (float)value->valuedouble;
            }
            // Current
            else if (strcasecmp(n, "Total Current") == 0 || strcasecmp(n, "Amp") == 0 ||
                     strstr(d, "urrent") || strstr(d, "Ampere")) {
                data->power.total_amps = (float)value->valuedouble;
                strncpy(data->power.amps_name, n, sizeof(data->power.amps_name) - 1);
            }
            // PWM readbacks (must check BEFORE watts/power)
            else if ((strncasecmp(n, "pwm", 3) == 0 || strstr(d, "PWM") ||
                      strstr(d, "power output")) && data->power.pwm_count < 4) {
                int idx = data->power.pwm_count;
                data->power.pwm[idx] = (float)value->valuedouble;
                strncpy(data->power.pwm_names[idx], n, sizeof(data->power.pwm_names[idx]) - 1);
                data->power.pwm_count++;
            }
            // Power/Watts
            else if (strcasecmp(n, "Total Power") == 0 || strcasecmp(n, "Watt") == 0 ||
                     strstr(d, "Watt")) {
                data->power.total_watts = (float)value->valuedouble;
                strncpy(data->power.watts_name, n, sizeof(data->power.watts_name) - 1);
            }
        }
    }

    // Parse WritableSwitches for dew heaters
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

            // Skip duplicates already in PWM list
            bool duplicate = false;
            for (int i = 0; i < data->power.pwm_count; i++) {
                if (strcasecmp(data->power.pwm_names[i], n) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            if (max_val == 100 && data->power.pwm_count < 4) {
                int idx = data->power.pwm_count;
                data->power.pwm[idx] = (float)value->valuedouble;
                strncpy(data->power.pwm_names[idx], n, sizeof(data->power.pwm_names[idx]) - 1);
                data->power.pwm_count++;
            } else if (max_val > 1 &&
                       (strstr(d, "Dew") || strstr(d, "PWM") || strstr(d, "pwm")) &&
                       data->power.pwm_count < 4) {
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
