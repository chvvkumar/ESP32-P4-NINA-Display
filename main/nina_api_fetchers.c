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
                    data->exposure_end_epoch = (int64_t)end_time;
                    ESP_LOGI(TAG, "Camera exposing: %.1fs remaining", remaining);
                }
            }
        } else {
            data->exposure_end_epoch = 0;  // Not exposing — clear for interpolation timer
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
    if (connected && cJSON_IsTrue(connected)) {
        data->safety_connected = true;
        cJSON *is_safe = cJSON_GetObjectItem(response, "IsSafe");
        data->safety_is_safe = is_safe && cJSON_IsTrue(is_safe);
        ESP_LOGI(TAG, "Safety monitor: connected=%d, safe=%d",
                 data->safety_connected, data->safety_is_safe);
    }

    cJSON_Delete(json);
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

    // Name
    cJSON *name = cJSON_GetObjectItem(response, "Name");
    if (name && name->valuestring)
        strncpy(out->name, name->valuestring, sizeof(out->name) - 1);

    // Sensor dimensions
    cJSON *xsize = cJSON_GetObjectItem(response, "XSize");
    if (xsize) out->x_size = xsize->valueint;
    cJSON *ysize = cJSON_GetObjectItem(response, "YSize");
    if (ysize) out->y_size = ysize->valueint;

    // Pixel size
    cJSON *pixel = cJSON_GetObjectItem(response, "PixelSize");
    if (pixel) out->pixel_size = (float)pixel->valuedouble;

    // Bit depth
    cJSON *bits = cJSON_GetObjectItem(response, "BitDepth");
    if (bits) out->bit_depth = bits->valueint;

    // Sensor type
    cJSON *sensor = cJSON_GetObjectItem(response, "SensorType");
    if (sensor && sensor->valuestring)
        strncpy(out->sensor_type, sensor->valuestring, sizeof(out->sensor_type) - 1);

    // Temperature
    cJSON *temp = cJSON_GetObjectItem(response, "Temperature");
    if (temp) out->temperature = (float)temp->valuedouble;

    // Target temperature (from CoolingTargetTemp or TemperatureSetPoint)
    cJSON *target_temp = cJSON_GetObjectItem(response, "TemperatureSetPoint");
    if (target_temp) out->target_temp = (float)target_temp->valuedouble;

    // Cooler power
    cJSON *cooler_pwr = cJSON_GetObjectItem(response, "CoolerPower");
    if (cooler_pwr) out->cooler_power = (float)cooler_pwr->valuedouble;

    // Cooler on
    cJSON *cooler_on = cJSON_GetObjectItem(response, "CoolerOn");
    if (cooler_on) out->cooler_on = cJSON_IsTrue(cooler_on);

    // At target temp
    cJSON *at_target = cJSON_GetObjectItem(response, "AtTargetTemp");
    if (at_target) out->at_target = cJSON_IsTrue(at_target);

    // Dew heater
    cJSON *dew = cJSON_GetObjectItem(response, "DewHeaterOn");
    if (dew) out->dew_heater_on = cJSON_IsTrue(dew);

    // Camera state
    cJSON *state = cJSON_GetObjectItem(response, "CameraState");
    if (state && state->valuestring)
        strncpy(out->camera_state, state->valuestring, sizeof(out->camera_state) - 1);

    // Last download time
    cJSON *download = cJSON_GetObjectItem(response, "LastDownloadTime");
    if (download) out->last_download_time = (float)download->valuedouble;

    // Gain
    cJSON *gain = cJSON_GetObjectItem(response, "Gain");
    if (gain) out->gain = gain->valueint;
    cJSON *gain_min = cJSON_GetObjectItem(response, "GainMin");
    if (gain_min) out->gain_min = gain_min->valueint;
    cJSON *gain_max = cJSON_GetObjectItem(response, "GainMax");
    if (gain_max) out->gain_max = gain_max->valueint;

    // Offset
    cJSON *offset = cJSON_GetObjectItem(response, "Offset");
    if (offset) out->offset = offset->valueint;
    cJSON *offset_min = cJSON_GetObjectItem(response, "OffsetMin");
    if (offset_min) out->offset_min = offset_min->valueint;
    cJSON *offset_max = cJSON_GetObjectItem(response, "OffsetMax");
    if (offset_max) out->offset_max = offset_max->valueint;

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

    // USB limit
    cJSON *usb = cJSON_GetObjectItem(response, "USBLimit");
    if (usb) out->usb_limit = usb->valueint;

    // Battery
    cJSON *battery = cJSON_GetObjectItem(response, "Battery");
    if (battery) out->battery = battery->valueint;

    // Binning
    cJSON *binx = cJSON_GetObjectItem(response, "BinX");
    if (binx) out->bin_x = binx->valueint;
    cJSON *biny = cJSON_GetObjectItem(response, "BinY");
    if (biny) out->bin_y = biny->valueint;

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

    cJSON *temp = cJSON_GetObjectItem(response, "Temperature");
    if (temp) out->weather_temp = (float)temp->valuedouble;

    cJSON *dew = cJSON_GetObjectItem(response, "DewPoint");
    if (dew) out->dew_point = (float)dew->valuedouble;

    cJSON *humidity = cJSON_GetObjectItem(response, "Humidity");
    if (humidity) out->humidity = (float)humidity->valuedouble;

    cJSON *pressure = cJSON_GetObjectItem(response, "Pressure");
    if (pressure) out->pressure = (float)pressure->valuedouble;

    cJSON *wind = cJSON_GetObjectItem(response, "WindSpeed");
    if (wind) out->wind_speed = (float)wind->valuedouble;

    cJSON *wind_dir = cJSON_GetObjectItem(response, "WindDirection");
    if (wind_dir) out->wind_direction = wind_dir->valueint;

    cJSON *cloud = cJSON_GetObjectItem(response, "CloudCover");
    if (cloud) out->cloud_cover = cloud->valueint;

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

    // Connected
    cJSON *conn = cJSON_GetObjectItem(response, "Connected");
    if (conn) out->connected = cJSON_IsTrue(conn);

    // Name
    cJSON *name = cJSON_GetObjectItem(response, "Name");
    if (name && name->valuestring)
        strncpy(out->name, name->valuestring, sizeof(out->name) - 1);

    // Coordinates
    cJSON *coords = cJSON_GetObjectItem(response, "Coordinates");
    if (coords) {
        cJSON *ra_str = cJSON_GetObjectItem(coords, "RAString");
        if (ra_str && ra_str->valuestring)
            strncpy(out->ra_string, ra_str->valuestring, sizeof(out->ra_string) - 1);

        cJSON *dec_str = cJSON_GetObjectItem(coords, "DecString");
        if (dec_str && dec_str->valuestring)
            strncpy(out->dec_string, dec_str->valuestring, sizeof(out->dec_string) - 1);

        cJSON *ra_deg = cJSON_GetObjectItem(coords, "RADegrees");
        if (ra_deg) out->ra_degrees = (float)ra_deg->valuedouble;

        cJSON *dec_deg = cJSON_GetObjectItem(coords, "Dec");
        if (dec_deg) out->dec_degrees = (float)dec_deg->valuedouble;
    }

    // Altitude / Azimuth
    cJSON *alt = cJSON_GetObjectItem(response, "Altitude");
    if (alt) out->altitude = (float)alt->valuedouble;

    cJSON *az = cJSON_GetObjectItem(response, "Azimuth");
    if (az) out->azimuth = (float)az->valuedouble;

    // Side of pier
    cJSON *pier = cJSON_GetObjectItem(response, "SideOfPier");
    if (pier && pier->valuestring)
        strncpy(out->pier_side, pier->valuestring, sizeof(out->pier_side) - 1);

    // Alignment mode
    cJSON *alignment = cJSON_GetObjectItem(response, "AlignmentMode");
    if (alignment && alignment->valuestring)
        strncpy(out->alignment_mode, alignment->valuestring, sizeof(out->alignment_mode) - 1);

    // Tracking mode
    cJSON *track_mode = cJSON_GetObjectItem(response, "TrackingMode");
    if (track_mode && track_mode->valuestring)
        strncpy(out->tracking_mode, track_mode->valuestring, sizeof(out->tracking_mode) - 1);

    // Tracking enabled
    cJSON *tracking = cJSON_GetObjectItem(response, "TrackingEnabled");
    if (tracking) out->tracking_enabled = cJSON_IsTrue(tracking);

    // Sidereal time
    cJSON *sidereal = cJSON_GetObjectItem(response, "SiderealTimeString");
    if (sidereal && sidereal->valuestring)
        strncpy(out->sidereal_time, sidereal->valuestring, sizeof(out->sidereal_time) - 1);

    // Meridian flip time
    cJSON *flip = cJSON_GetObjectItem(response, "TimeToMeridianFlipString");
    if (flip && flip->valuestring)
        strncpy(out->flip_time, flip->valuestring, sizeof(out->flip_time) - 1);

    // Site coordinates
    cJSON *lat = cJSON_GetObjectItem(response, "SiteLatitude");
    if (lat) out->latitude = (float)lat->valuedouble;

    cJSON *lon = cJSON_GetObjectItem(response, "SiteLongitude");
    if (lon) out->longitude = (float)lon->valuedouble;

    cJSON *elev = cJSON_GetObjectItem(response, "SiteElevation");
    if (elev) out->elevation = (float)elev->valuedouble;

    // Status booleans
    cJSON *at_park = cJSON_GetObjectItem(response, "AtPark");
    if (at_park) out->at_park = cJSON_IsTrue(at_park);

    cJSON *at_home = cJSON_GetObjectItem(response, "AtHome");
    if (at_home) out->at_home = cJSON_IsTrue(at_home);

    cJSON *slewing = cJSON_GetObjectItem(response, "Slewing");
    if (slewing) out->slewing = cJSON_IsTrue(slewing);

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
    cJSON *target_name = cJSON_GetObjectItem(active_target, "Name");
    if (target_name && target_name->valuestring) {
        strncpy(out->target_name, target_name->valuestring, sizeof(out->target_name) - 1);
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
            cJSON *child_name = cJSON_GetObjectItem(active_child, "Name");
            if (child_name && child_name->valuestring) {
                strncpy(out->container_name, child_name->valuestring, sizeof(out->container_name) - 1);
                char *suffix = strstr(out->container_name, "_Container");
                if (suffix) *suffix = '\0';
            }
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
                cJSON *completed = cJSON_GetObjectItem(child, "CompletedIterations");
                cJSON *iterations = cJSON_GetObjectItem(child, "Iterations");
                int comp = completed ? completed->valueint : 0;
                int total = iterations ? iterations->valueint : 0;

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
        cJSON *completed = cJSON_GetObjectItem(running_smart_exp, "CompletedIterations");
        cJSON *iterations = cJSON_GetObjectItem(running_smart_exp, "Iterations");
        cJSON *exp_time = cJSON_GetObjectItem(running_smart_exp, "ExposureTime");
        cJSON *filter_prop = cJSON_GetObjectItem(running_smart_exp, "Filter");

        out->current_completed = completed ? completed->valueint : 0;
        out->current_total = iterations ? iterations->valueint : 0;
        out->current_exposure_time = exp_time ? (float)exp_time->valuedouble : 0;
        if (filter_prop && filter_prop->valuestring) {
            strncpy(out->current_filter, filter_prop->valuestring, sizeof(out->current_filter) - 1);
        }

        strncpy(out->step_name, "Smart Exposure", sizeof(out->step_name) - 1);
    } else if (running_step) {
        cJSON *step_name = cJSON_GetObjectItem(running_step, "Name");
        if (step_name && step_name->valuestring) {
            strncpy(out->step_name, step_name->valuestring, sizeof(out->step_name) - 1);
        }
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

    /* Parse RMS summary */
    cJSON *rms = cJSON_GetObjectItem(response, "RMS");
    if (rms) {
        cJSON *ra = cJSON_GetObjectItem(rms, "RA");
        cJSON *dec = cJSON_GetObjectItem(rms, "Dec");
        cJSON *total = cJSON_GetObjectItem(rms, "Total");
        cJSON *peak_ra = cJSON_GetObjectItem(rms, "PeakRA");
        cJSON *peak_dec = cJSON_GetObjectItem(rms, "PeakDec");
        cJSON *pscale = cJSON_GetObjectItem(rms, "Scale");

        if (ra) out->rms_ra = (float)ra->valuedouble;
        if (dec) out->rms_dec = (float)dec->valuedouble;
        if (total) out->rms_total = (float)total->valuedouble;
        if (peak_ra) out->peak_ra = (float)peak_ra->valuedouble;
        if (peak_dec) out->peak_dec = (float)peak_dec->valuedouble;
        if (pscale) out->pixel_scale = (float)pscale->valuedouble;
    }

    /* Parse pixel scale from response level */
    cJSON *ps = cJSON_GetObjectItem(response, "PixelScale");
    if (ps) out->pixel_scale = (float)ps->valuedouble;

    /* Parse guide steps array */
    cJSON *steps = cJSON_GetObjectItem(response, "GuideSteps");
    if (steps && cJSON_IsArray(steps)) {
        int total_steps = cJSON_GetArraySize(steps);
        /* Take last max_points steps (most recent) */
        int start = 0;
        if (total_steps > max_points) start = total_steps - max_points;
        if (max_points > GRAPH_MAX_POINTS) max_points = GRAPH_MAX_POINTS;

        int idx = 0;
        for (int i = start; i < total_steps && idx < GRAPH_MAX_POINTS; i++) {
            cJSON *step = cJSON_GetArrayItem(steps, i);
            if (!step) continue;

            cJSON *ra_raw = cJSON_GetObjectItem(step, "RADistanceRaw");
            cJSON *dec_raw = cJSON_GetObjectItem(step, "DECDistanceRaw");

            out->ra[idx] = ra_raw ? (float)ra_raw->valuedouble : 0;
            out->dec[idx] = dec_raw ? (float)dec_raw->valuedouble : 0;
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

    /* The API returns images newest-first; we want oldest-first for the chart.
     * Take last max_points images and reverse into the output array. */
    int start = 0;
    if (total_images > max_points) start = total_images - max_points;
    if (max_points > GRAPH_MAX_POINTS) max_points = GRAPH_MAX_POINTS;

    /* First pass: collect from start..end into temp positions */
    int count = 0;
    for (int i = total_images - 1; i >= start && count < GRAPH_MAX_POINTS; i--) {
        cJSON *item = cJSON_GetArrayItem(response, i);
        if (!item) continue;

        cJSON *hfr = cJSON_GetObjectItem(item, "HFR");
        cJSON *stars = cJSON_GetObjectItem(item, "Stars");

        float hfr_val = hfr ? (float)hfr->valuedouble : 0;
        if (hfr_val <= 0) continue;  /* Skip images with no HFR data */

        out->hfr[count] = hfr_val;
        out->stars[count] = stars ? stars->valueint : 0;
        count++;
    }
    out->count = count;

    ESP_LOGI(TAG, "HFR history: %d images", out->count);
    cJSON_Delete(json);
}
