/**
 * @file weather_client.c
 * @brief Provider-abstracted HTTP weather client.
 *
 * Polls weather data from one of three providers (OWM, Open-Meteo,
 * Weather Underground) on a dedicated FreeRTOS task pinned to Core 0.
 * Data is mutex-protected and copied out via weather_client_get_data().
 */

#include "weather_client.h"
#include "app_config.h"
#include "tasks.h"

#include "ui/nina_clock.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

static const char *TAG = "weather";

#define WEATHER_RESPONSE_BUF_SIZE  16384
#define WEATHER_HTTP_TIMEOUT_MS    15000
#define WEATHER_TASK_STACK_SIZE    16384
#define WEATHER_RETRY_INTERVAL_S  60

/* ── Static state ── */
static weather_data_t    s_data;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_task_handle;

// =============================================================================
// Mutex helpers
// =============================================================================

void weather_client_init(void) {
    memset(&s_data, 0, sizeof(s_data));
    s_data.valid = false;
    s_data.uv_index = -1.0f;
    s_mutex = xSemaphoreCreateMutex();
}

void weather_client_get_data(weather_data_t *out) {
    if (!out) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(out, &s_data, sizeof(weather_data_t));
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(weather_data_t));
    }
}

bool weather_client_has_valid_data(void) {
    bool valid = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        valid = s_data.valid;
        xSemaphoreGive(s_mutex);
    }
    return valid;
}

void weather_client_force_refresh(void) {
    if (s_task_handle) {
        xTaskNotifyGive(s_task_handle);
    }
}

void weather_client_invalidate(void) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(&s_data, 0, sizeof(s_data));
        s_data.valid = false;
        s_data.uv_index = -1.0f;
        xSemaphoreGive(s_mutex);
    }
}

// =============================================================================
// Helpers
// =============================================================================

/** Convert wind direction in degrees to compass string (N, NE, E, ...). */
static void deg_to_compass(float deg, char *out, size_t out_size) {
    static const char *dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    /* Normalise to 0-360 */
    while (deg < 0.0f)   deg += 360.0f;
    while (deg >= 360.0f) deg -= 360.0f;
    int idx = (int)((deg + 22.5f) / 45.0f) % 8;
    snprintf(out, out_size, "%s", dirs[idx]);
}

/** Uppercase a string in-place. */
static void str_to_upper(char *s) {
    for (; *s; s++) *s = toupper((unsigned char)*s);
}

// =============================================================================
// HTTP fetch helper — returns PSRAM-allocated buffer, caller must free()
// =============================================================================

/**
 * Perform an HTTP GET and return the response body as a null-terminated
 * PSRAM-allocated string.  Returns NULL on any error.
 */
static char *http_get_body(const char *url) {
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = WEATHER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "Failed to init HTTP client");
        return NULL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int buf_size = (content_length > 0 && content_length < WEATHER_RESPONSE_BUF_SIZE)
                       ? (content_length + 1)
                       : WEATHER_RESPONSE_BUF_SIZE;

    char *buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes", buf_size);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total = 0;
    int max_read = buf_size - 1;
    while (total < max_read) {
        int n = esp_http_client_read(client, buffer + total, max_read - total);
        if (n <= 0) break;
        total += n;
    }
    buffer[total] = '\0';

    esp_http_client_cleanup(client);

    if (total == 0) {
        ESP_LOGW(TAG, "Empty response body");
        free(buffer);
        return NULL;
    }

    return buffer;
}

// =============================================================================
// OpenWeatherMap 2.5
// =============================================================================

static bool fetch_owm(const app_config_t *cfg, weather_data_t *out) {
    const char *units = (cfg->weather_units == 0) ? "imperial" : "metric";

    /* ── Current weather ── */
    char url[320];
    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/weather?"
             "lat=%.4f&lon=%.4f&appid=%s&units=%s",
             cfg->weather_lat, cfg->weather_lon, cfg->weather_api_key, units);

    char *body = http_get_body(url);
    if (!body) return false;

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        ESP_LOGW(TAG, "OWM current: JSON parse failed");
        return false;
    }

    cJSON *main_obj = cJSON_GetObjectItem(json, "main");
    cJSON *wind_obj = cJSON_GetObjectItem(json, "wind");
    cJSON *weather_arr = cJSON_GetObjectItem(json, "weather");

    if (!main_obj) {
        ESP_LOGW(TAG, "OWM current: missing 'main'");
        cJSON_Delete(json);
        return false;
    }

    cJSON *temp_item = cJSON_GetObjectItem(main_obj, "temp");
    cJSON *temp_max  = cJSON_GetObjectItem(main_obj, "temp_max");
    cJSON *temp_min  = cJSON_GetObjectItem(main_obj, "temp_min");
    cJSON *hum_item  = cJSON_GetObjectItem(main_obj, "humidity");

    out->temp_current = temp_item  ? (float)temp_item->valuedouble  : 0.0f;
    out->temp_high    = temp_max   ? (float)temp_max->valuedouble   : out->temp_current;
    out->temp_low     = temp_min   ? (float)temp_min->valuedouble   : out->temp_current;
    out->humidity     = hum_item   ? (float)hum_item->valuedouble   : 0.0f;

    /* Dew point approximation */
    out->dew_point = out->temp_current - ((100.0f - out->humidity) / 5.0f);

    if (wind_obj) {
        cJSON *ws = cJSON_GetObjectItem(wind_obj, "speed");
        cJSON *wd = cJSON_GetObjectItem(wind_obj, "deg");
        out->wind_speed = ws ? (float)ws->valuedouble : 0.0f;
        if (wd) {
            deg_to_compass((float)wd->valuedouble, out->wind_dir, sizeof(out->wind_dir));
        } else {
            snprintf(out->wind_dir, sizeof(out->wind_dir), "--");
        }
    }

    /* Condition from weather[0].description */
    if (weather_arr && cJSON_IsArray(weather_arr) && cJSON_GetArraySize(weather_arr) > 0) {
        cJSON *w0 = cJSON_GetArrayItem(weather_arr, 0);
        cJSON *desc = w0 ? cJSON_GetObjectItem(w0, "description") : NULL;
        if (desc && cJSON_IsString(desc)) {
            snprintf(out->condition, sizeof(out->condition), "%s", desc->valuestring);
            str_to_upper(out->condition);
        }
    }

    /* UV not available in 2.5 free tier */
    out->uv_index = -1.0f;

    cJSON_Delete(json);

    /* ── Forecast (3-hour intervals) ── */
    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/forecast?"
             "lat=%.4f&lon=%.4f&appid=%s&units=%s&cnt=16",
             cfg->weather_lat, cfg->weather_lon, cfg->weather_api_key, units);

    body = http_get_body(url);
    if (!body) {
        /* Current succeeded, forecast optional */
        return true;
    }

    json = cJSON_Parse(body);
    free(body);
    if (!json) return true;

    cJSON *list = cJSON_GetObjectItem(json, "list");
    if (list && cJSON_IsArray(list)) {
        int count = cJSON_GetArraySize(list);
        if (count > 9) count = 9;  /* Leave slot 0 for current */

        /* Slot 0: current conditions (so the bar chart starts at "now") */
        time_t now;
        time(&now);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        out->hourly_temps[0] = out->temp_current;
        out->hourly_hours[0] = (uint8_t)tm_now.tm_hour;

        /* Slots 1..9: 3-hour forecast entries */
        for (int i = 0; i < count; i++) {
            cJSON *entry = cJSON_GetArrayItem(list, i);
            cJSON *dt    = cJSON_GetObjectItem(entry, "dt");
            cJSON *m     = cJSON_GetObjectItem(entry, "main");
            cJSON *t     = m ? cJSON_GetObjectItem(m, "temp") : NULL;
            if (t) {
                out->hourly_temps[i + 1] = (float)t->valuedouble;
            }
            if (dt) {
                time_t ts = (time_t)dt->valuedouble;
                struct tm tm_info;
                localtime_r(&ts, &tm_info);
                out->hourly_hours[i + 1] = (uint8_t)tm_info.tm_hour;
            }
        }
    }

    cJSON_Delete(json);
    return true;
}

// =============================================================================
// Open-Meteo
// =============================================================================

/** Map WMO weather code to condition string. */
static const char *wmo_code_to_condition(int code) {
    switch (code) {
        case 0:            return "CLEAR SKY";
        case 1:            return "MAINLY CLEAR";
        case 2:            return "PARTLY CLOUDY";
        case 3:            return "OVERCAST";
        case 45: case 48:  return "FOGGY";
        case 51: case 53: case 55: return "DRIZZLE";
        case 61: case 63: case 65: return "RAINY";
        case 71: case 73: case 75: return "SNOWY";
        case 77:           return "SNOW GRAINS";
        case 80: case 81: case 82: return "SHOWERS";
        case 85: case 86:  return "SNOW SHOWERS";
        case 95:           return "THUNDERSTORM";
        case 96: case 99:  return "THUNDERSTORM";
        default:           return "UNKNOWN";
    }
}

static bool fetch_open_meteo(const app_config_t *cfg, weather_data_t *out) {
    const char *temp_unit = (cfg->weather_units == 0) ? "fahrenheit" : "celsius";
    const char *wind_unit = (cfg->weather_units == 0) ? "mph" : "kmh";

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?"
             "latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,"
             "wind_speed_10m,wind_direction_10m,dew_point_2m"
             "&hourly=temperature_2m,uv_index"
             "&temperature_unit=%s&wind_speed_unit=%s&forecast_days=2",
             cfg->weather_lat, cfg->weather_lon, temp_unit, wind_unit);

    char *body = http_get_body(url);
    if (!body) return false;

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        ESP_LOGW(TAG, "Open-Meteo: JSON parse failed");
        return false;
    }

    /* ── Current conditions ── */
    cJSON *current = cJSON_GetObjectItem(json, "current");
    if (!current) {
        ESP_LOGW(TAG, "Open-Meteo: missing 'current'");
        cJSON_Delete(json);
        return false;
    }

    cJSON *ct  = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *ch  = cJSON_GetObjectItem(current, "relative_humidity_2m");
    cJSON *cwc = cJSON_GetObjectItem(current, "weather_code");
    cJSON *cws = cJSON_GetObjectItem(current, "wind_speed_10m");
    cJSON *cwd = cJSON_GetObjectItem(current, "wind_direction_10m");
    cJSON *cdp = cJSON_GetObjectItem(current, "dew_point_2m");

    out->temp_current = ct  ? (float)ct->valuedouble  : 0.0f;
    out->humidity     = ch  ? (float)ch->valuedouble  : 0.0f;
    out->wind_speed   = cws ? (float)cws->valuedouble : 0.0f;
    out->dew_point    = cdp ? (float)cdp->valuedouble : 0.0f;

    if (cwd) {
        deg_to_compass((float)cwd->valuedouble, out->wind_dir, sizeof(out->wind_dir));
    }

    if (cwc) {
        snprintf(out->condition, sizeof(out->condition), "%s",
                 wmo_code_to_condition((int)cwc->valuedouble));
    }

    /* ── Hourly data (temps, UV, high/low) ── */
    cJSON *hourly = cJSON_GetObjectItem(json, "hourly");
    if (hourly) {
        cJSON *h_time = cJSON_GetObjectItem(hourly, "time");
        cJSON *h_temp = cJSON_GetObjectItem(hourly, "temperature_2m");
        cJSON *h_uv   = cJSON_GetObjectItem(hourly, "uv_index");

        int h_count = h_time ? cJSON_GetArraySize(h_time) : 0;

        /* Determine current hour index by matching current time */
        time_t now;
        time(&now);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        int cur_hour = tm_now.tm_hour;

        /* Find the index of the current hour in the hourly array.
         * Open-Meteo returns ISO-8601 strings like "2026-03-22T14:00" */
        int start_idx = -1;
        for (int i = 0; i < h_count; i++) {
            cJSON *t_item = cJSON_GetArrayItem(h_time, i);
            if (!t_item || !cJSON_IsString(t_item)) continue;
            /* Parse hour from "YYYY-MM-DDTHH:MM" */
            const char *ts = t_item->valuestring;
            int len = strlen(ts);
            if (len < 13) continue;
            /* Extract hour: position 11-12 */
            int h = (ts[11] - '0') * 10 + (ts[12] - '0');
            /* Extract day to match: position 8-9 */
            int d = (ts[8] - '0') * 10 + (ts[9] - '0');
            if (d == tm_now.tm_mday && h == cur_hour) {
                start_idx = i;
                break;
            }
        }

        /* Fill next 10 hourly slots */
        if (start_idx >= 0 && h_temp) {
            for (int i = 0; i < 10 && (start_idx + i) < h_count; i++) {
                cJSON *tv = cJSON_GetArrayItem(h_temp, start_idx + i);
                if (tv) out->hourly_temps[i] = (float)tv->valuedouble;

                cJSON *ti = cJSON_GetArrayItem(h_time, start_idx + i);
                if (ti && cJSON_IsString(ti) && strlen(ti->valuestring) >= 13) {
                    out->hourly_hours[i] = (uint8_t)((ti->valuestring[11] - '0') * 10
                                                      + (ti->valuestring[12] - '0'));
                }
            }
        }

        /* UV index: take max from first UV entry at current hour */
        if (h_uv && start_idx >= 0) {
            cJSON *uv_item = cJSON_GetArrayItem(h_uv, start_idx);
            out->uv_index = uv_item ? (float)uv_item->valuedouble : -1.0f;
        }

        /* High/Low: scan today's 24 hourly temps (indices 0..23 for day 1) */
        if (h_temp) {
            float hi = -1000.0f, lo = 1000.0f;
            /* Find first index for today */
            int day_start = -1;
            for (int i = 0; i < h_count; i++) {
                cJSON *t_item = cJSON_GetArrayItem(h_time, i);
                if (!t_item || !cJSON_IsString(t_item) || strlen(t_item->valuestring) < 10) continue;
                int d = (t_item->valuestring[8] - '0') * 10 + (t_item->valuestring[9] - '0');
                if (d == tm_now.tm_mday) {
                    day_start = i;
                    break;
                }
            }
            if (day_start >= 0) {
                for (int i = day_start; i < h_count && i < day_start + 24; i++) {
                    cJSON *t_item = cJSON_GetArrayItem(h_time, i);
                    if (t_item && cJSON_IsString(t_item) && strlen(t_item->valuestring) >= 10) {
                        int d = (t_item->valuestring[8] - '0') * 10 + (t_item->valuestring[9] - '0');
                        if (d != tm_now.tm_mday) break;
                    }
                    cJSON *tv = cJSON_GetArrayItem(h_temp, i);
                    if (tv) {
                        float v = (float)tv->valuedouble;
                        if (v > hi) hi = v;
                        if (v < lo) lo = v;
                    }
                }
                out->temp_high = (hi > -999.0f) ? hi : out->temp_current;
                out->temp_low  = (lo <  999.0f) ? lo : out->temp_current;
            } else {
                out->temp_high = out->temp_current;
                out->temp_low  = out->temp_current;
            }
        }
    }

    cJSON_Delete(json);
    return true;
}

// =============================================================================
// Weather Underground
// =============================================================================

static bool fetch_wunderground(const app_config_t *cfg, weather_data_t *out) {
    const char *units_code = (cfg->weather_units == 0) ? "e" : "m";

    /* ── Current observations (PWS) ── */
    char url[384];
    snprintf(url, sizeof(url),
             "https://api.weather.com/v2/pws/observations/current?"
             "stationId=%s&apiKey=%s&units=%s&format=json",
             cfg->weather_location_name, cfg->weather_api_key, units_code);

    char *body = http_get_body(url);
    if (!body) return false;

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        ESP_LOGW(TAG, "WU current: JSON parse failed");
        return false;
    }

    cJSON *obs_arr = cJSON_GetObjectItem(json, "observations");
    cJSON *obs0 = (obs_arr && cJSON_IsArray(obs_arr) && cJSON_GetArraySize(obs_arr) > 0)
                      ? cJSON_GetArrayItem(obs_arr, 0) : NULL;
    if (!obs0) {
        ESP_LOGW(TAG, "WU: no observations");
        cJSON_Delete(json);
        return false;
    }

    /* Units object: "imperial" or "metric" */
    const char *units_key = (cfg->weather_units == 0) ? "imperial" : "metric";
    cJSON *u = cJSON_GetObjectItem(obs0, units_key);
    if (u) {
        cJSON *t  = cJSON_GetObjectItem(u, "temp");
        cJSON *dp = cJSON_GetObjectItem(u, "dewpt");
        cJSON *ws = cJSON_GetObjectItem(u, "windSpeed");

        out->temp_current = t  ? (float)t->valuedouble  : 0.0f;
        out->dew_point    = dp ? (float)dp->valuedouble  : 0.0f;
        out->wind_speed   = ws ? (float)ws->valuedouble  : 0.0f;
    }

    cJSON *hum = cJSON_GetObjectItem(obs0, "humidity");
    out->humidity = hum ? (float)hum->valuedouble : 0.0f;

    cJSON *winddir = cJSON_GetObjectItem(obs0, "winddir");
    if (winddir) {
        deg_to_compass((float)winddir->valuedouble, out->wind_dir, sizeof(out->wind_dir));
    }

    /* WU current doesn't include condition text — leave blank */
    out->condition[0] = '\0';

    /* High/low and UV not in PWS current — set defaults */
    out->temp_high = out->temp_current;
    out->temp_low  = out->temp_current;
    out->uv_index  = -1.0f;

    cJSON_Delete(json);

    /* ── Hourly forecast ── */
    snprintf(url, sizeof(url),
             "https://api.weather.com/v3/wx/forecast/hourly/12hour?"
             "geocode=%.4f,%.4f&apiKey=%s&units=%s&format=json",
             cfg->weather_lat, cfg->weather_lon, cfg->weather_api_key, units_code);

    body = http_get_body(url);
    if (!body) return true;  /* current succeeded, forecast optional */

    json = cJSON_Parse(body);
    free(body);
    if (!json) return true;

    cJSON *temps = cJSON_GetObjectItem(json, "temperature");
    if (temps && cJSON_IsArray(temps)) {
        int count = cJSON_GetArraySize(temps);
        if (count > 10) count = 10;

        time_t now;
        time(&now);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        for (int i = 0; i < count; i++) {
            cJSON *tv = cJSON_GetArrayItem(temps, i);
            if (tv) out->hourly_temps[i] = (float)tv->valuedouble;
            out->hourly_hours[i] = (uint8_t)((tm_now.tm_hour + i) % 24);
        }

        /* Update high/low from forecast */
        for (int i = 0; i < count; i++) {
            if (out->hourly_temps[i] > out->temp_high) out->temp_high = out->hourly_temps[i];
            if (out->hourly_temps[i] < out->temp_low)  out->temp_low  = out->hourly_temps[i];
        }
    }

    cJSON_Delete(json);
    return true;
}

// =============================================================================
// Poll task
// =============================================================================

static void weather_poll_task(void *arg) {
    (void)arg;

    /* Wait for WiFi connection */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected, starting weather polling");

    while (1) {
        /* Suspend during OTA updates */
        while (ota_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        app_config_t cfg_snap = app_config_get_snapshot();

        /* Skip if no location configured */
        bool has_location = (cfg_snap.weather_location_name[0] != '\0');
        /* OWM and WU need an API key */
        bool needs_key = (cfg_snap.weather_provider == 0 || cfg_snap.weather_provider == 2);
        bool has_key   = (cfg_snap.weather_api_key[0] != '\0');

        if (!has_location || (needs_key && !has_key)) {
            ESP_LOGD(TAG, "Weather not configured (provider=%d), sleeping 60s",
                     cfg_snap.weather_provider);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WEATHER_RETRY_INTERVAL_S * 1000));
            continue;
        }

        /* Fetch from selected provider */
        weather_data_t local;
        memset(&local, 0, sizeof(local));
        local.uv_index = -1.0f;

        bool ok = false;
        switch (cfg_snap.weather_provider) {
            case 0:  ok = fetch_owm(&cfg_snap, &local);          break;
            case 1:  ok = fetch_open_meteo(&cfg_snap, &local);   break;
            case 2:  ok = fetch_wunderground(&cfg_snap, &local);  break;
            default:
                ESP_LOGW(TAG, "Unknown weather provider: %d", cfg_snap.weather_provider);
                break;
        }

        if (ok) {
            local.valid = true;
            local.last_update_ts = (int64_t)time(NULL);

            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                memcpy(&s_data, &local, sizeof(weather_data_t));
                xSemaphoreGive(s_mutex);
            }
            ESP_LOGI(TAG, "Weather updated: %.1f%s, %s",
                     local.temp_current,
                     (cfg_snap.weather_units == 0) ? "F" : "C",
                     local.condition);

            /* Trigger immediate clock UI refresh */
            clock_page_request_update();

            /* Normal poll interval */
            uint32_t interval_ms = (uint32_t)cfg_snap.weather_poll_interval_s * 1000;
            if (interval_ms < 900000)   interval_ms = 900000;   /* min 15 min */
            if (interval_ms > 3600000)  interval_ms = 3600000;  /* max 1 hour */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(interval_ms));
        } else {
            ESP_LOGW(TAG, "Weather fetch failed, retrying in %ds", WEATHER_RETRY_INTERVAL_S);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WEATHER_RETRY_INTERVAL_S * 1000));
        }
    }
}

void weather_client_start(void) {
    if (s_task_handle) return;  /* Already running */

    void *stack = heap_caps_calloc(1, WEATHER_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (!stack) {
        ESP_LOGE(TAG, "Failed to allocate task stack in PSRAM");
        return;
    }

    StaticTask_t *tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (!tcb) {
        ESP_LOGE(TAG, "Failed to allocate TCB");
        free(stack);
        return;
    }

    s_task_handle = xTaskCreateStaticPinnedToCore(
        weather_poll_task,
        "weather_poll",
        WEATHER_TASK_STACK_SIZE,
        NULL,
        5,
        (StackType_t *)stack,
        tcb,
        0  /* Core 0 — networking core */
    );

    if (s_task_handle) {
        ESP_LOGI(TAG, "Weather poll task started on Core 0");
    } else {
        ESP_LOGE(TAG, "Failed to create weather poll task");
        free(stack);
        free(tcb);
    }
}
