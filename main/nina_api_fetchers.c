/**
 * @file nina_api_fetchers.c
 * @brief Individual REST API endpoint fetch functions for NINA.
 *
 * Each function fetches data from a single NINA API endpoint and
 * populates the corresponding fields in nina_client_t.
 */

#include "nina_api_fetchers.h"
#include "nina_client_internal.h"
#include "json_get.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "nina_fetch";

/* Client-mutex write timeout for fetcher commits. The poll task is the primary
 * writer; the WS handler and UI take the same lock. On timeout we skip the write
 * for this cycle (data stays one cycle stale) rather than write lock-free. */
#define FETCH_LOCK_MS 100

/* Commit an offline state (connected=false, status=OFFLINE) under the lock. */
static void nina_fetch_set_offline(nina_client_t *data) {
    if (nina_client_lock(data, FETCH_LOCK_MS)) {
        data->connected = false;
        strcpy(data->status, "OFFLINE");
        nina_client_unlock(data);
    }
}

/**
 * @brief Fetch camera info - ALWAYS WORKS
 * Provides: IsExposing, ExposureEndTime, Temperature, CoolerPower, CameraState
 */
void fetch_camera_info_robust(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/camera/info", base_url);

    /* Capture the NINA PC's own clock from the HTTP Date header, and stamp
     * the device monotonic clock ONCE right after the fetch returns so the
     * (epoch, mono) pair describes the same instant. */
    int64_t date_epoch = 0;
    cJSON *json = http_get_json_dated(url, &date_epoch);
    int64_t fetch_mono_us = esp_timer_get_time();
    if (!json) {
        // Transport failure / non-2xx / empty body — API unreachable.
        nina_fetch_set_offline(data);
        return;
    }

    // Honor the application-level Success flag: connectivity requires Success==true,
    // not merely a non-NULL body. Recomputed every poll so a stuck `true` can't latch.
    if (!nina_api_envelope_ok(json)) {
        cJSON_Delete(json);
        nina_fetch_set_offline(data);
        return;
    }

    cJSON *response = nina_api_response(json);
    if (!response) {
        // Envelope OK but no Response object — treat as offline this poll
        // (symmetric with fetch_equipment_info_bundled).
        cJSON_Delete(json);
        nina_fetch_set_offline(data);
        return;
    }

    if (!nina_client_lock(data, FETCH_LOCK_MS)) {
        cJSON_Delete(json);
        return;
    }
    data->connected = true;
    if (date_epoch > 0) {
        data->nina_clock_epoch = date_epoch;
        data->nina_clock_mono_us = fetch_mono_us;
    }
    {
        JSON_GET_STR_NONEMPTY(response, "Name", data->camera_name);
        JSON_GET_STR(response, "CameraState", data->status);
        JSON_GET_FLOAT(response, "Temperature", data->camera.temp);
        JSON_GET_FLOAT(response, "CoolerPower", data->camera.cooler_power);

        // Exposure time (total length per frame)
        cJSON *exp_time = cJSON_GetObjectItem(response, "ExposureTime");
        if (exp_time && exp_time->valuedouble > 0) {
            data->exposure_total = (float)exp_time->valuedouble;
        }

        // Exposure status
        cJSON *is_exposing = cJSON_GetObjectItem(response, "IsExposing");
        cJSON *exp_end = cJSON_GetObjectItem(response, "ExposureEndTime");

        data->is_exposing = (is_exposing && cJSON_IsTrue(is_exposing));

        if (data->is_exposing && exp_end && exp_end->valuestring) {
            time_t end_time = parse_iso8601(exp_end->valuestring);
            /* Do the remaining-time math in the NINA clock domain: Date-derived
             * "now" needs no boot-clock sanity guard; the time(NULL) fallback
             * keeps the >2020 guard against an unset SNTP clock. */
            int64_t now_nina = (date_epoch > 0) ? date_epoch : (int64_t)time(NULL);
            bool now_valid = (date_epoch > 0) || (now_nina > 1577836800);

            if (now_valid && end_time > 0) {
                int64_t remaining = (int64_t)end_time - now_nina;
                if (remaining >= 0 && remaining <= 7200) {
                    data->exposure_current = -(float)remaining;
                    data->exposure_end_epoch = (int64_t)end_time;
                    if (data->exposure_total == 0 && remaining > 0) {
                        data->exposure_total = (float)remaining;
                    }
                    ESP_LOGI(TAG, "Camera exposing: %llds remaining", (long long)remaining);
                }
            }
        }
        // Do NOT clear exposure_end_epoch when !is_exposing -- UI uses it to detect completion
    }
    nina_client_unlock(data);

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
    if (response && nina_client_lock(data, FETCH_LOCK_MS)) {
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
        nina_client_unlock(data);
    }
    cJSON_Delete(json);
}

/**
 * @brief Lightweight image count check via /image-history?count=true (~50 bytes).
 * Returns the number of images, or -1 on error.
 * Used as a change-detection gate to skip the full /image-history fetch
 * when the image count hasn't changed (eliminates ~95% of redundant fetches).
 */
int fetch_image_count(const char *base_url) {
    char url[256];
    snprintf(url, sizeof(url), "%simage-history?count=true", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return -1;

    int count = -1;
    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (response && cJSON_IsNumber(response)) {
        count = response->valueint;
    }
    cJSON_Delete(json);
    return count;
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
        if (latest && nina_client_lock(data, FETCH_LOCK_MS)) {
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

            JSON_GET_FLOAT(latest, "HFR", data->hfr);
            JSON_GET_INT(latest, "Stars", data->stars);

            // Detect new image (HFR or stars changed)
            if (data->hfr != prev_hfr || data->stars != prev_stars) {
                data->new_image_available = true;
            }

            ESP_LOGI(TAG, "Image stats: HFR=%.2f, Stars=%d", data->hfr, data->stars);
            nina_client_unlock(data);
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
    if (response && cJSON_IsArray(response) && nina_client_lock(data, FETCH_LOCK_MS)) {
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
        nina_client_unlock(data);
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
    if (response && nina_client_lock(data, FETCH_LOCK_MS)) {
        cJSON *connected = cJSON_GetObjectItem(response, "Connected");
        cJSON *rms = cJSON_GetObjectItem(response, "RMSError");
        // Guider disconnected or no RMS payload: zero the values so the UI does
        // not render stale guiding numbers for a guider that is no longer guiding.
        if ((connected && !cJSON_IsTrue(connected)) || !rms) {
            data->guider.rms_total = 0;
            data->guider.rms_ra = 0;
            data->guider.rms_dec = 0;
        } else {
            /* Each axis under RMSError is {"Arcseconds": n}; the inner lookup is
             * NULL-safe, so a missing axis leaves the previous value in place. */
            JSON_GET_FLOAT(cJSON_GetObjectItem(rms, "Total"), "Arcseconds", data->guider.rms_total);
            JSON_GET_FLOAT(cJSON_GetObjectItem(rms, "RA"),    "Arcseconds", data->guider.rms_ra);
            JSON_GET_FLOAT(cJSON_GetObjectItem(rms, "Dec"),   "Arcseconds", data->guider.rms_dec);

            ESP_LOGI(TAG, "Guiding RMS - Total: %.2f\", RA: %.2f\", DEC: %.2f\"",
                data->guider.rms_total, data->guider.rms_ra, data->guider.rms_dec);
        }
        nina_client_unlock(data);
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
    if (response && nina_client_lock(data, FETCH_LOCK_MS)) {
        JSON_GET_STR(response, "TimeToMeridianFlipString", data->meridian_flip);
        nina_client_unlock(data);
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
    if (response && nina_client_lock(data, FETCH_LOCK_MS)) {
        JSON_GET_INT(response, "Position", data->focuser.position);
        nina_client_unlock(data);
    }
    cJSON_Delete(json);
}

/**
 * @brief Parse switch/power response JSON into nina_client_t power fields.
 * Shared by both fetch_switch_info() and fetch_equipment_info_bundled().
 * @param response  The switch info cJSON object (Response from individual, or Switch sub-object from bundle)
 * @param data      Client data to populate
 */
static void parse_switch_response(cJSON *response, nina_client_t *data) {
    cJSON *connected = cJSON_GetObjectItem(response, "Connected");
    if (!connected || !cJSON_IsTrue(connected)) {
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
    if (response && nina_client_lock(data, FETCH_LOCK_MS)) {
        parse_switch_response(response, data);
        nina_client_unlock(data);
    }

    cJSON_Delete(json);
}

/**
 * @brief Fetch safety monitor state (one-shot on connect).
 * Endpoint: GET {base_url}equipment/safetymonitor/info
 * Sets safety_connected and safety_is_safe in nina_client_t.
 */
void fetch_safety_monitor_info(const char *base_url, nina_client_t *data) {
    char url[256];
    snprintf(url, sizeof(url), "%sequipment/safetymonitor/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response) {
        cJSON_Delete(json);
        return;
    }

    cJSON *connected = cJSON_GetObjectItem(response, "Connected");
    if (connected && cJSON_IsTrue(connected) && nina_client_lock(data, FETCH_LOCK_MS)) {
        data->safety_connected = true;
        cJSON *is_safe = cJSON_GetObjectItem(response, "IsSafe");
        data->safety_is_safe = is_safe && cJSON_IsTrue(is_safe);
        ESP_LOGI(TAG, "Safety monitor: connected=%d, safe=%d",
                 data->safety_connected, data->safety_is_safe);
        nina_client_unlock(data);
    }

    cJSON_Delete(json);
}

/**
 * @brief Fetch all equipment info from the bundled /equipment/info endpoint.
 * ninaAPI 2.2.15+ returns Camera, FilterWheel, Focuser, Guider, Mount, Switch,
 * SafetyMonitor (and more) in a single HTTP response, replacing 7+ individual calls.
 *
 * @return 0 on success, -1 on HTTP failure (offline), -2 if endpoint unavailable
 */
int fetch_equipment_info_bundled(const char *base_url, nina_client_t *data, bool fetch_filter_list,
                                uint16_t *out_connected_mask) {
    if (out_connected_mask) *out_connected_mask = 0;

    char url[256];
    snprintf(url, sizeof(url), "%sequipment/info", base_url);

    /* Capture NINA's own clock (HTTP Date header) + a device monotonic stamp
     * taken ONCE right after the fetch returns (same pattern as
     * fetch_camera_info_robust). */
    int64_t date_epoch = 0;
    cJSON *json = http_get_json_dated(url, &date_epoch);
    int64_t fetch_mono_us = esp_timer_get_time();
    if (!json) {
        // Transport failure / non-2xx / empty body — API unreachable.
        nina_fetch_set_offline(data);
        return -1;
    }

    // Honor the application-level Success flag: a 2xx body with Success!=true
    // means the API is up but reported a failure — treat as offline this poll.
    if (!nina_api_envelope_ok(json)) {
        cJSON_Delete(json);
        nina_fetch_set_offline(data);
        return -1;
    }

    cJSON *response = nina_api_response(json);
    if (!response) {
        // Envelope OK but no Response object — may be a 404 or unsupported endpoint
        cJSON_Delete(json);
        nina_fetch_set_offline(data);
        return -2;
    }

    // Envelope is OK with a Response object — the API is reachable this poll.
    // Commit all parsed equipment fields atomically under the client lock. On
    // timeout, skip the write section but still return success (data one cycle stale).
    if (!nina_client_lock(data, FETCH_LOCK_MS)) {
        cJSON_Delete(json);
        return 0;
    }
    data->connected = true;
    if (date_epoch > 0) {
        data->nina_clock_epoch = date_epoch;
        data->nina_clock_mono_us = fetch_mono_us;
    }

    // ── Camera ──
    cJSON *camera = cJSON_GetObjectItem(response, "Camera");
    if (camera) {
        JSON_GET_STR_NONEMPTY(camera, "Name", data->camera_name);
        JSON_GET_STR(camera, "CameraState", data->status);
        JSON_GET_FLOAT(camera, "Temperature", data->camera.temp);
        JSON_GET_FLOAT(camera, "CoolerPower", data->camera.cooler_power);

        // Exposure time (total length per frame)
        cJSON *exp_time_cam = cJSON_GetObjectItem(camera, "ExposureTime");
        if (exp_time_cam && exp_time_cam->valuedouble > 0) {
            data->exposure_total = (float)exp_time_cam->valuedouble;
        }

        // Exposure timing
        cJSON *is_exposing = cJSON_GetObjectItem(camera, "IsExposing");
        cJSON *exp_end = cJSON_GetObjectItem(camera, "ExposureEndTime");

        data->is_exposing = (is_exposing && cJSON_IsTrue(is_exposing));

        if (data->is_exposing && exp_end && exp_end->valuestring) {
            time_t end_time = parse_iso8601(exp_end->valuestring);
            /* NINA-domain "now": Date-derived needs no boot-clock guard; the
             * time(NULL) fallback keeps the >2020 unset-SNTP guard. */
            int64_t now_nina = (date_epoch > 0) ? date_epoch : (int64_t)time(NULL);
            bool now_valid = (date_epoch > 0) || (now_nina > 1577836800);
            if (now_valid && end_time > 0) {
                int64_t remaining = (int64_t)end_time - now_nina;
                if (remaining >= 0 && remaining <= 7200) {
                    data->exposure_current = -(float)remaining;
                    data->exposure_end_epoch = (int64_t)end_time;
                    if (data->exposure_total == 0 && remaining > 0) {
                        data->exposure_total = (float)remaining;
                    }
                }
            }
        }
    }

    // ── Guider ──
    cJSON *guider = cJSON_GetObjectItem(response, "Guider");
    if (guider) {
        cJSON *guider_conn = cJSON_GetObjectItem(guider, "Connected");
        cJSON *rms = cJSON_GetObjectItem(guider, "RMSError");
        // Guider disconnected or no RMS payload: zero the values so the UI does
        // not render stale guiding numbers for a guider that is no longer guiding.
        if ((guider_conn && !cJSON_IsTrue(guider_conn)) || !rms) {
            data->guider.rms_total = 0;
            data->guider.rms_ra = 0;
            data->guider.rms_dec = 0;
        } else {
            JSON_GET_FLOAT(cJSON_GetObjectItem(rms, "Total"), "Arcseconds", data->guider.rms_total);
            JSON_GET_FLOAT(cJSON_GetObjectItem(rms, "RA"),    "Arcseconds", data->guider.rms_ra);
            JSON_GET_FLOAT(cJSON_GetObjectItem(rms, "Dec"),   "Arcseconds", data->guider.rms_dec);
            ESP_LOGI(TAG, "Guiding RMS - Total: %.2f\", RA: %.2f\", DEC: %.2f\"",
                data->guider.rms_total, data->guider.rms_ra, data->guider.rms_dec);
        }
    }

    // ── Filter Wheel ──
    cJSON *fw = cJSON_GetObjectItem(response, "FilterWheel");
    if (fw) {
        JSON_GET_STR(cJSON_GetObjectItem(fw, "SelectedFilter"), "Name", data->current_filter);

        if (fetch_filter_list) {
            cJSON *avail = cJSON_GetObjectItem(fw, "AvailableFilters");
            if (avail && cJSON_IsArray(avail)) {
                int count = cJSON_GetArraySize(avail);
                if (count > MAX_FILTERS) count = MAX_FILTERS;
                data->filter_count = 0;
                for (int i = 0; i < count; i++) {
                    cJSON *f = cJSON_GetArrayItem(avail, i);
                    if (f) {
                        cJSON *fn = cJSON_GetObjectItem(f, "Name");
                        cJSON *fi = cJSON_GetObjectItem(f, "Id");
                        if (fn && fn->valuestring) {
                            strncpy(data->filters[i].name, fn->valuestring,
                                    sizeof(data->filters[i].name) - 1);
                            data->filters[i].id = fi ? fi->valueint : i;
                            data->filter_count++;
                        }
                    }
                }
                ESP_LOGI(TAG, "Found %d available filters (bundled)", data->filter_count);
            }
        }
    }

    // ── Focuser ──
    JSON_GET_INT(cJSON_GetObjectItem(response, "Focuser"), "Position", data->focuser.position);

    // ── Mount ──
    JSON_GET_STR(cJSON_GetObjectItem(response, "Mount"), "TimeToMeridianFlipString",
                 data->meridian_flip);

    // ── Switch ──
    cJSON *sw = cJSON_GetObjectItem(response, "Switch");
    if (sw) {
        parse_switch_response(sw, data);
    }

    // ── Safety Monitor ──
    cJSON *safety = cJSON_GetObjectItem(response, "SafetyMonitor");
    if (safety) {
        cJSON *conn = cJSON_GetObjectItem(safety, "Connected");
        if (conn && cJSON_IsTrue(conn)) {
            data->safety_connected = true;
            cJSON *is_safe = cJSON_GetObjectItem(safety, "IsSafe");
            data->safety_is_safe = is_safe && cJSON_IsTrue(is_safe);
        }
    }
    nina_client_unlock(data);

    /* Build equipment connected bitmask from Connected fields.
     * Bit positions match equipment_type_t in nina_websocket.c:
     * 0=Camera, 1=Mount, 2=Guider, 3=Focuser, 4=Filterwheel,
     * 5=Rotator, 6=Safety, 7=Dome, 8=Flat, 9=Switch, 10=Weather */
    if (out_connected_mask) {
        uint16_t mask = 0;
        static const struct { const char *key; int bit; } eq_map[] = {
            {"Camera",        0}, {"Mount",         1}, {"Guider",       2},
            {"Focuser",       3}, {"FilterWheel",   4}, {"Rotator",      5},
            {"SafetyMonitor", 6}, {"Dome",          7}, {"FlatDevice",   8},
            {"Switch",        9}, {"WeatherData",  10},
        };
        for (int i = 0; i < (int)(sizeof(eq_map) / sizeof(eq_map[0])); i++) {
            cJSON *eq = cJSON_GetObjectItem(response, eq_map[i].key);
            if (eq) {
                cJSON *conn = cJSON_GetObjectItem(eq, "Connected");
                if (conn && cJSON_IsTrue(conn)) {
                    mask |= (1 << eq_map[i].bit);
                }
            }
        }
        *out_connected_mask = mask;
    }

    cJSON_Delete(json);
    return 0;
}

/* ── Info overlay detail fetchers ───────────────────────────────────── */

#include "ui/info_overlay_types.h"

/**
 * @brief Fetch detailed camera info for the camera info overlay.
 * Endpoint: GET {base_url}equipment/camera/info
 */
void fetch_camera_details(const char *base_url, camera_detail_data_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(camera_detail_data_t));

    char url[256];
    snprintf(url, sizeof(url), "%sequipment/camera/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response) { cJSON_Delete(json); return; }

    JSON_GET_STR(response, "Name", out->name);

    // Sensor
    JSON_GET_INT(response, "XSize", out->x_size);
    JSON_GET_INT(response, "YSize", out->y_size);
    JSON_GET_FLOAT(response, "PixelSize", out->pixel_size);
    JSON_GET_INT(response, "BitDepth", out->bit_depth);
    JSON_GET_STR(response, "SensorType", out->sensor_type);

    // Cooling
    JSON_GET_FLOAT(response, "Temperature", out->temperature);
    JSON_GET_FLOAT(response, "TemperatureSetPoint", out->target_temp);
    JSON_GET_FLOAT(response, "CoolerPower", out->cooler_power);
    JSON_GET_BOOL(response, "CoolerOn", out->cooler_on);
    JSON_GET_BOOL(response, "AtTargetTemp", out->at_target);
    JSON_GET_BOOL(response, "DewHeaterOn", out->dew_heater_on);

    JSON_GET_STR(response, "CameraState", out->camera_state);
    JSON_GET_FLOAT(response, "LastDownloadTime", out->last_download_time);

    // Gain / offset
    JSON_GET_INT(response, "Gain", out->gain);
    JSON_GET_INT(response, "GainMin", out->gain_min);
    JSON_GET_INT(response, "GainMax", out->gain_max);
    JSON_GET_INT(response, "Offset", out->offset);
    JSON_GET_INT(response, "OffsetMin", out->offset_min);
    JSON_GET_INT(response, "OffsetMax", out->offset_max);

    // Readout mode — index maps to name from ReadoutModes array
    cJSON *readout_idx = cJSON_GetObjectItem(response, "ReadoutMode");
    cJSON *readout_modes = cJSON_GetObjectItem(response, "ReadoutModes");
    if (readout_idx && readout_modes && cJSON_IsArray(readout_modes)) {
        int idx = readout_idx->valueint;
        cJSON *mode = cJSON_GetArrayItem(readout_modes, idx);
        if (mode && mode->valuestring) {
            strncpy(out->readout_mode, mode->valuestring, sizeof(out->readout_mode) - 1);
        }
    } else if (readout_idx) {
        snprintf(out->readout_mode, sizeof(out->readout_mode), "Mode %d", readout_idx->valueint);
    }

    JSON_GET_INT(response, "USBLimit", out->usb_limit);
    JSON_GET_INT(response, "Battery", out->battery);
    JSON_GET_INT(response, "BinX", out->bin_x);
    JSON_GET_INT(response, "BinY", out->bin_y);

    ESP_LOGI(TAG, "Camera details: %s %dx%d %.2fum %dbit",
             out->name, out->x_size, out->y_size, out->pixel_size, out->bit_depth);

    cJSON_Delete(json);
}

/**
 * @brief Fetch weather info and populate weather fields in camera_detail_data_t.
 * Endpoint: GET {base_url}equipment/weather/info
 */
void fetch_weather_details(const char *base_url, camera_detail_data_t *out) {
    if (!out) return;

    char url[256];
    snprintf(url, sizeof(url), "%sequipment/weather/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response) { cJSON_Delete(json); return; }

    cJSON *connected = cJSON_GetObjectItem(response, "Connected");
    if (!connected || !cJSON_IsTrue(connected)) {
        out->weather_connected = false;
        cJSON_Delete(json);
        return;
    }

    out->weather_connected = true;

    JSON_GET_FLOAT(response, "Temperature", out->weather_temp);
    JSON_GET_FLOAT(response, "DewPoint", out->dew_point);
    JSON_GET_FLOAT(response, "Humidity", out->humidity);
    JSON_GET_FLOAT(response, "Pressure", out->pressure);
    JSON_GET_FLOAT(response, "WindSpeed", out->wind_speed);
    JSON_GET_INT(response, "WindDirection", out->wind_direction);
    JSON_GET_INT(response, "CloudCover", out->cloud_cover);

    cJSON *sqm = cJSON_GetObjectItem(response, "SkyQuality");
    if (sqm) {
        snprintf(out->sky_quality, sizeof(out->sky_quality), "%.1f", sqm->valuedouble);
    }

    ESP_LOGI(TAG, "Weather: %.1fC, %.0f%% humidity, %.1f hPa, wind %.1f",
             out->weather_temp, out->humidity, out->pressure, out->wind_speed);

    cJSON_Delete(json);
}

/**
 * @brief Fetch detailed mount info for the mount info overlay.
 * Endpoint: GET {base_url}equipment/mount/info
 */
void fetch_mount_details(const char *base_url, mount_detail_data_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(mount_detail_data_t));

    char url[256];
    snprintf(url, sizeof(url), "%sequipment/mount/info", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response) { cJSON_Delete(json); return; }

    JSON_GET_BOOL(response, "Connected", out->connected);
    JSON_GET_STR(response, "Name", out->name);

    // Coordinates (sub-object; lookup is NULL-safe when absent)
    cJSON *coords = cJSON_GetObjectItem(response, "Coordinates");
    JSON_GET_STR(coords, "RAString", out->ra_string);
    JSON_GET_STR(coords, "DecString", out->dec_string);
    JSON_GET_FLOAT(coords, "RADegrees", out->ra_degrees);
    JSON_GET_FLOAT(coords, "Dec", out->dec_degrees);

    // Pointing
    JSON_GET_FLOAT(response, "Altitude", out->altitude);
    JSON_GET_FLOAT(response, "Azimuth", out->azimuth);
    JSON_GET_STR(response, "SideOfPier", out->pier_side);
    JSON_GET_STR(response, "AlignmentMode", out->alignment_mode);
    JSON_GET_STR(response, "TrackingMode", out->tracking_mode);
    JSON_GET_BOOL(response, "TrackingEnabled", out->tracking_enabled);
    JSON_GET_STR(response, "SiderealTimeString", out->sidereal_time);
    JSON_GET_STR(response, "TimeToMeridianFlipString", out->flip_time);

    // Site
    JSON_GET_FLOAT(response, "SiteLatitude", out->latitude);
    JSON_GET_FLOAT(response, "SiteLongitude", out->longitude);
    JSON_GET_FLOAT(response, "SiteElevation", out->elevation);

    // Status booleans
    JSON_GET_BOOL(response, "AtPark", out->at_park);
    JSON_GET_BOOL(response, "AtHome", out->at_home);
    JSON_GET_BOOL(response, "Slewing", out->slewing);

    ESP_LOGI(TAG, "Mount details: %s RA=%s DEC=%s Alt=%.1f Az=%.1f",
             out->name, out->ra_string, out->dec_string, out->altitude, out->azimuth);

    cJSON_Delete(json);
}

/**
 * @brief Fetch detailed sequence data for the sequence info overlay.
 * Endpoint: GET {base_url}sequence/json
 *
 * Walks the recursive sequence tree to extract per-filter breakdown and totals.
 */
void fetch_sequence_details(const char *base_url, sequence_detail_data_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(sequence_detail_data_t));

    char url[256];
    snprintf(url, sizeof(url), "%ssequence/json", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response || !cJSON_IsArray(response)) {
        cJSON_Delete(json);
        return;
    }

    // Find Targets_Container in the top-level array
    cJSON *targets_container = NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, response) {
        cJSON *name = cJSON_GetObjectItem(item, "Name");
        if (name && name->valuestring && strcmp(name->valuestring, "Targets_Container") == 0) {
            targets_container = item;
            break;
        }
    }

    if (!targets_container) {
        cJSON_Delete(json);
        return;
    }

    // Find the active target container (RUNNING preferred, otherwise last FINISHED)
    cJSON *target_items = cJSON_GetObjectItem(targets_container, "Items");
    if (!target_items || !cJSON_IsArray(target_items)) {
        cJSON_Delete(json);
        return;
    }

    cJSON *active_target = NULL;
    cJSON *last_finished = NULL;
    cJSON *target = NULL;
    cJSON_ArrayForEach(target, target_items) {
        cJSON *status = cJSON_GetObjectItem(target, "Status");
        if (!status || !status->valuestring) continue;
        if (strcmp(status->valuestring, "RUNNING") == 0) {
            active_target = target;
            break;
        }
        if (strcmp(status->valuestring, "FINISHED") == 0) {
            last_finished = target;
        }
    }
    if (!active_target) active_target = last_finished;
    if (!active_target) active_target = cJSON_GetArrayItem(target_items, 0);
    if (!active_target) {
        cJSON_Delete(json);
        return;
    }

    out->has_data = true;

    // Target name (strip _Container suffix)
    JSON_GET_STR(active_target, "Name", out->target_name);
    {
        char *suffix = strstr(out->target_name, "_Container");
        if (suffix) *suffix = '\0';
    }

    // Walk the target's children to find container name and running step
    cJSON *target_children = cJSON_GetObjectItem(active_target, "Items");
    if (target_children && cJSON_IsArray(target_children)) {
        // Find the active sub-container (e.g., LRGB_Container)
        cJSON *child = NULL;
        cJSON *active_child = NULL;
        cJSON *last_finished_child = NULL;
        cJSON_ArrayForEach(child, target_children) {
            cJSON *child_status = cJSON_GetObjectItem(child, "Status");
            cJSON *child_items = cJSON_GetObjectItem(child, "Items");
            if (!child_status || !child_status->valuestring) continue;
            if (!child_items || !cJSON_IsArray(child_items)) continue;

            if (strcmp(child_status->valuestring, "RUNNING") == 0) {
                active_child = child;
                break;
            }
            if (strcmp(child_status->valuestring, "FINISHED") == 0) {
                last_finished_child = child;
            }
        }
        if (!active_child) active_child = last_finished_child;

        if (active_child) {
            JSON_GET_STR(active_child, "Name", out->container_name);
            char *suffix = strstr(out->container_name, "_Container");
            if (suffix) *suffix = '\0';
        }
    }

    // Recursive walk to find all Smart Exposure nodes and build per-filter breakdown
    // We search the entire active target tree for Smart Exposure items
    // Use a simple iterative approach with a stack
    cJSON *stack[32];
    int stack_top = 0;
    stack[stack_top++] = active_target;

    out->filter_count = 0;
    out->total_completed = 0;
    out->total_total = 0;

    // Also track the running Smart Exposure for current step info
    cJSON *running_smart_exp = NULL;
    cJSON *running_step = NULL;

    while (stack_top > 0) {
        cJSON *node = stack[--stack_top];
        cJSON *node_items = cJSON_GetObjectItem(node, "Items");
        if (!node_items || !cJSON_IsArray(node_items)) continue;

        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node_items) {
            cJSON *child_name = cJSON_GetObjectItem(child, "Name");
            cJSON *child_status = cJSON_GetObjectItem(child, "Status");

            if (child_name && child_name->valuestring &&
                strcmp(child_name->valuestring, "Smart Exposure") == 0) {
                // This is a Smart Exposure node — extract filter and counts
                int comp = json_int_or(child, "CompletedIterations", 0);
                int total = json_int_or(child, "Iterations", 0);

                // Look for filter name in the parent or sibling context
                // Smart Exposure items typically sit inside a container with filter in its name,
                // or have a Filter property
                cJSON *filter_prop = cJSON_GetObjectItem(child, "Filter");
                const char *filter_name = NULL;
                if (filter_prop && filter_prop->valuestring) {
                    filter_name = filter_prop->valuestring;
                }

                // If no Filter property, try the parent container name
                if (!filter_name) {
                    cJSON *parent_name = cJSON_GetObjectItem(node, "Name");
                    if (parent_name && parent_name->valuestring) {
                        filter_name = parent_name->valuestring;
                    }
                }

                // Add to per-filter breakdown (aggregate by filter name)
                if (filter_name && out->filter_count < MAX_SEQ_FILTERS) {
                    // Check if filter already in list
                    int existing = -1;
                    for (int i = 0; i < out->filter_count; i++) {
                        if (strcmp(out->filters[i].name, filter_name) == 0) {
                            existing = i;
                            break;
                        }
                    }
                    if (existing >= 0) {
                        out->filters[existing].completed += comp;
                        out->filters[existing].total += total;
                    } else {
                        strncpy(out->filters[out->filter_count].name, filter_name,
                                sizeof(out->filters[out->filter_count].name) - 1);
                        out->filters[out->filter_count].completed = comp;
                        out->filters[out->filter_count].total = total;
                        out->filter_count++;
                    }
                }

                out->total_completed += comp;
                out->total_total += total;

                // Track running Smart Exposure
                if (child_status && child_status->valuestring &&
                    strcmp(child_status->valuestring, "RUNNING") == 0) {
                    running_smart_exp = child;
                }
            } else {
                // Not a Smart Exposure — push onto stack to search deeper
                cJSON *child_items = cJSON_GetObjectItem(child, "Items");
                if (child_items && cJSON_IsArray(child_items) && stack_top < 32) {
                    stack[stack_top++] = child;
                }

                // Track running step (leaf instruction)
                if (child_status && child_status->valuestring &&
                    strcmp(child_status->valuestring, "RUNNING") == 0) {
                    cJSON *sub_items = cJSON_GetObjectItem(child, "Items");
                    if (!sub_items || !cJSON_IsArray(sub_items) || cJSON_GetArraySize(sub_items) == 0) {
                        running_step = child;
                    }
                }
            }
        }
    }

    // Fill current step info from the running Smart Exposure
    if (running_smart_exp) {
        out->current_completed     = json_int_or(running_smart_exp, "CompletedIterations", 0);
        out->current_total         = json_int_or(running_smart_exp, "Iterations", 0);
        out->current_exposure_time = json_num_or(running_smart_exp, "ExposureTime", 0);
        JSON_GET_STR(running_smart_exp, "Filter", out->current_filter);

        strncpy(out->step_name, "Smart Exposure", sizeof(out->step_name) - 1);
    } else if (running_step) {
        JSON_GET_STR(running_step, "Name", out->step_name);
    }

    // Time remaining — look for OverallRemainingTime or similar at sequence level
    // Walk top-level conditions for time info
    cJSON *seq_conditions = cJSON_GetObjectItem(active_target, "Conditions");
    if (seq_conditions && cJSON_IsArray(seq_conditions)) {
        cJSON *cond = NULL;
        cJSON_ArrayForEach(cond, seq_conditions) {
            cJSON *rem = cJSON_GetObjectItem(cond, "RemainingTime");
            if (rem && rem->valuestring && rem->valuestring[0] != '\0') {
                strncpy(out->time_remaining, rem->valuestring, sizeof(out->time_remaining) - 1);
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Sequence details: target=%s container=%s step=%s filters=%d total=%d/%d",
             out->target_name, out->container_name, out->step_name,
             out->filter_count, out->total_completed, out->total_total);

    cJSON_Delete(json);
}

/* ── Graph data fetchers ────────────────────────────────────────────── */

#include "ui/nina_graph_overlay.h"
#include <math.h>

/**
 * @brief Fetch guider graph history from /equipment/guider/graph
 * Populates graph_rms_data_t with RA/DEC raw distance values and RMS summary.
 */
void fetch_guider_graph(const char *base_url, graph_rms_data_t *out, int max_points) {
    if (!out) return;
    memset(out, 0, sizeof(graph_rms_data_t));

    char url[256];
    snprintf(url, sizeof(url), "%sequipment/guider/graph", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response) { cJSON_Delete(json); return; }

    /* Parse RMS summary (sub-object; lookup is NULL-safe when absent) */
    cJSON *rms = cJSON_GetObjectItem(response, "RMS");
    JSON_GET_FLOAT(rms, "RA", out->rms_ra);
    JSON_GET_FLOAT(rms, "Dec", out->rms_dec);
    JSON_GET_FLOAT(rms, "Total", out->rms_total);
    JSON_GET_FLOAT(rms, "PeakRA", out->peak_ra);
    JSON_GET_FLOAT(rms, "PeakDec", out->peak_dec);
    JSON_GET_FLOAT(rms, "Scale", out->pixel_scale);

    /* Parse pixel scale from response level (overrides RMS.Scale) */
    JSON_GET_FLOAT(response, "PixelScale", out->pixel_scale);

    /* Parse guide steps array */
    cJSON *steps = cJSON_GetObjectItem(response, "GuideSteps");
    if (steps && cJSON_IsArray(steps)) {
        int total_steps = cJSON_GetArraySize(steps);
        /* Take last max_points steps (most recent) */
        int start = 0;
        if (total_steps > max_points) start = total_steps - max_points;

        /* Single forward pass: cJSON arrays are linked lists, so indexed
         * access re-walks the list per element (O(n^2) over thousands of
         * guide steps). Order is unchanged: array order, oldest-first. */
        int idx = 0;
        int i = 0;
        cJSON *step = NULL;
        cJSON_ArrayForEach(step, steps) {
            if (i++ < start) continue;
            if (idx >= GRAPH_MAX_POINTS) break;

            out->ra[idx]  = json_num_or(step, "RADistanceRaw", 0);
            out->dec[idx] = json_num_or(step, "DECDistanceRaw", 0);
            /* Total computed from RA and DEC */
            out->total[idx] = sqrtf(out->ra[idx] * out->ra[idx] +
                                    out->dec[idx] * out->dec[idx]);
            idx++;
        }
        out->count = idx;
    }

    ESP_LOGI(TAG, "Guider graph: %d steps, RMS=%.2f\"", out->count, out->rms_total);
    cJSON_Delete(json);
}

/**
 * @brief Fetch HFR history from /image-history?all=true&imageType=LIGHT
 * Populates graph_hfr_data_t with HFR values from each captured image.
 */
void fetch_hfr_history(const char *base_url, graph_hfr_data_t *out, int max_points) {
    if (!out) return;
    memset(out, 0, sizeof(graph_hfr_data_t));

    char url[256];
    snprintf(url, sizeof(url), "%simage-history?all=true&imageType=LIGHT", base_url);

    cJSON *json = http_get_json(url);
    if (!json) return;

    cJSON *response = cJSON_GetObjectItem(json, "Response");
    if (!response || !cJSON_IsArray(response)) { cJSON_Delete(json); return; }

    int total_images = cJSON_GetArraySize(response);
    if (total_images <= 0) { cJSON_Delete(json); return; }

    /* The API returns images OLDEST-first, which is also what the chart wants,
     * so emit in array order. Keep the last max_points images (most recent).
     * Single forward pass: cJSON arrays are linked lists, so indexed access
     * re-walks the list per element (O(n^2); ?all=true grows all session). */
    if (max_points > GRAPH_MAX_POINTS) max_points = GRAPH_MAX_POINTS;
    int start = 0;
    if (total_images > max_points) start = total_images - max_points;

    int count = 0;
    int i = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, response) {
        if (i++ < start) continue;
        if (count >= GRAPH_MAX_POINTS) break;

        float hfr_val = json_num_or(item, "HFR", 0);
        if (hfr_val <= 0) continue;  /* Skip images with no HFR data */

        out->hfr[count] = hfr_val;
        out->stars[count] = json_int_or(item, "Stars", 0);
        count++;
    }
    out->count = count;

    ESP_LOGI(TAG, "HFR history: %d images", out->count);
    cJSON_Delete(json);
}

/**
 * @brief Build HFR graph data from the local ring buffer (no HTTP fetch).
 * Extracts the most recent entries from the per-instance HFR ring buffer
 * populated by IMAGE-SAVE WebSocket events. Returns the data in oldest-first
 * order matching the format expected by the graph overlay.
 */
void build_hfr_from_ring(const nina_client_t *client, graph_hfr_data_t *out, int max_points) {
    if (!out || !client || !client->hfr_ring.hfr) return;
    memset(out, 0, sizeof(graph_hfr_data_t));

    int total = client->hfr_ring.count;
    if (total <= 0) return;

    /* Number of valid entries in the ring */
    int available = (total < HFR_RING_SIZE) ? total : HFR_RING_SIZE;
    int use = (available < max_points) ? available : max_points;
    if (use > GRAPH_MAX_POINTS) use = GRAPH_MAX_POINTS;

    /* Read oldest-first: start reading 'use' entries back from write_idx */
    int read_start;
    if (total < HFR_RING_SIZE) {
        /* Ring hasn't wrapped yet — data starts at index 0 */
        read_start = (available > use) ? (available - use) : 0;
    } else {
        /* Ring has wrapped — oldest valid entry is at write_idx */
        read_start = (client->hfr_ring.write_idx + (available - use)) % HFR_RING_SIZE;
    }

    for (int i = 0; i < use; i++) {
        int ring_idx = (read_start + i) % HFR_RING_SIZE;
        out->hfr[i]   = client->hfr_ring.hfr[ring_idx];
        out->stars[i]  = client->hfr_ring.stars[ring_idx];
    }
    out->count = use;
    ESP_LOGI(TAG, "HFR from ring buffer: %d points (total captured: %d)", use, total);
}
