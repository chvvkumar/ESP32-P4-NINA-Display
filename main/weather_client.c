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
#include "poll_task.h"

#include "ui/nina_clock.h"
#include "http_fetch.h"
#include "json_get.h"
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

/* A cJSON array is a linked list, so cJSON_GetArrayItem() re-walks from the
 * head on every call — an indexed loop over one is O(n^2). Seek once to the
 * start index, then advance with ->next inside the loop. Returns NULL when the
 * array is shorter than n, matching cJSON_GetArrayItem()'s out-of-range
 * behaviour. */
static cJSON *array_seek(const cJSON *arr, int n) {
    cJSON *it = arr ? arr->child : NULL;
    while (it && n-- > 0) it = it->next;
    return it;
}

// =============================================================================
// HTTP fetch helper — returns PSRAM-allocated buffer, caller must
// heap_caps_free()
// =============================================================================

/**
 * Perform an HTTP GET and return the response body as a null-terminated
 * PSRAM-allocated string.  Returns NULL on any error.
 *
 * Thin wrapper over the shared http_fetch_text() (main/http_fetch.h/.c):
 * TLS cert-bundle validation on, redirects followed up to 3 hops, single
 * attempt (this poller's own retry interval is the retry mechanism), same
 * response-size cap as before. All transport/status/size error logging
 * happens inside http_fetch_text() itself.
 */
static char *http_get_body(const char *url) {
    http_fetch_opts_t opts = {
        .timeout_ms         = WEATHER_HTTP_TIMEOUT_MS,
        .use_tls_bundle     = true,
        .max_redirects      = 3,
        .max_attempts       = 1,
        .max_response_bytes = WEATHER_RESPONSE_BUF_SIZE,
    };

    char *body = NULL;
    size_t len = 0;
    if (http_fetch_text(url, &opts, &body, &len) != ESP_OK) {
        return NULL;
    }

    if (len == 0) {
        ESP_LOGW(TAG, "Empty response body");
        heap_caps_free(body);
        return NULL;
    }

    return body;
}

// =============================================================================
// OpenWeatherMap 2.5
// =============================================================================

static bool fetch_owm(const app_config_t *cfg, weather_data_t *out) {
    const char *units = (cfg->weather_units == 0) ? "imperial" : "metric";

    /* ── Current weather ── */
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/weather?"
             "lat=%.4f&lon=%.4f&appid=%s&units=%s",
             cfg->weather_lat, cfg->weather_lon, cfg->weather_api_key, units);

    char *body = http_get_body(url);
    if (!body) return false;

    cJSON *json = cJSON_Parse(body);
    heap_caps_free(body);
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

    /* temp_current first: it is the fallback for the high/low pair. */
    out->temp_current = json_num_or(main_obj, "temp", 0.0f);
    out->temp_high    = json_num_or(main_obj, "temp_max", out->temp_current);
    out->temp_low     = json_num_or(main_obj, "temp_min", out->temp_current);
    out->humidity     = json_num_or(main_obj, "humidity", 0.0f);

    /* Dew point approximation */
    out->dew_point = out->temp_current - ((100.0f - out->humidity) / 5.0f);

    if (wind_obj) {
        cJSON *wd = cJSON_GetObjectItem(wind_obj, "deg");
        out->wind_speed = json_num_or(wind_obj, "speed", 0.0f);
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
             "https://api.openweathermap.org/data/2.5/forecast?"
             "lat=%.4f&lon=%.4f&appid=%s&units=%s&cnt=16",
             cfg->weather_lat, cfg->weather_lon, cfg->weather_api_key, units);

    body = http_get_body(url);
    if (!body) {
        /* Current succeeded, forecast optional */
        return true;
    }

    json = cJSON_Parse(body);
    heap_caps_free(body);
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
        int i = -1;
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, list) {
            if (++i >= count) break;
            cJSON *dt = cJSON_GetObjectItem(entry, "dt");
            JSON_GET_FLOAT(cJSON_GetObjectItem(entry, "main"), "temp",
                           out->hourly_temps[i + 1]);
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
    heap_caps_free(body);
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

    cJSON *cwc = cJSON_GetObjectItem(current, "weather_code");
    cJSON *cwd = cJSON_GetObjectItem(current, "wind_direction_10m");

    out->temp_current = json_num_or(current, "temperature_2m", 0.0f);
    out->humidity     = json_num_or(current, "relative_humidity_2m", 0.0f);
    out->wind_speed   = json_num_or(current, "wind_speed_10m", 0.0f);
    out->dew_point    = json_num_or(current, "dew_point_2m", 0.0f);

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
        int idx = 0;
        cJSON *t_item = NULL;
        cJSON_ArrayForEach(t_item, h_time) {
            /* Parse hour from "YYYY-MM-DDTHH:MM" */
            if (cJSON_IsString(t_item) && strlen(t_item->valuestring) >= 13) {
                const char *ts = t_item->valuestring;
                /* Extract hour: position 11-12 */
                int h = (ts[11] - '0') * 10 + (ts[12] - '0');
                /* Extract day to match: position 8-9 */
                int d = (ts[8] - '0') * 10 + (ts[9] - '0');
                if (d == tm_now.tm_mday && h == cur_hour) {
                    start_idx = idx;
                    break;
                }
            }
            idx++;
        }

        /* Fill next 10 hourly slots */
        if (start_idx >= 0 && h_temp) {
            cJSON *tv = array_seek(h_temp, start_idx);
            cJSON *ti = array_seek(h_time, start_idx);
            for (int i = 0; i < 10 && (start_idx + i) < h_count; i++) {
                if (tv) {
                    out->hourly_temps[i] = (float)tv->valuedouble;
                    tv = tv->next;
                }
                if (ti) {
                    if (cJSON_IsString(ti) && strlen(ti->valuestring) >= 13) {
                        out->hourly_hours[i] = (uint8_t)((ti->valuestring[11] - '0') * 10
                                                          + (ti->valuestring[12] - '0'));
                    }
                    ti = ti->next;
                }
            }
        }

        /* UV index: take max from first UV entry at current hour */
        if (h_uv && start_idx >= 0) {
            cJSON *uv_item = array_seek(h_uv, start_idx);
            out->uv_index = uv_item ? (float)uv_item->valuedouble : -1.0f;
        }

        /* High/Low: scan today's 24 hourly temps (indices 0..23 for day 1) */
        if (h_temp) {
            float hi = -1000.0f, lo = 1000.0f;
            /* Find first index for today */
            int day_start = -1;
            int d_idx = 0;
            cJSON *d_item = NULL;
            cJSON_ArrayForEach(d_item, h_time) {
                if (cJSON_IsString(d_item) && strlen(d_item->valuestring) >= 10) {
                    int d = (d_item->valuestring[8] - '0') * 10 + (d_item->valuestring[9] - '0');
                    if (d == tm_now.tm_mday) {
                        day_start = d_idx;
                        break;
                    }
                }
                d_idx++;
            }
            if (day_start >= 0) {
                cJSON *ht = array_seek(h_time, day_start);
                cJSON *tv = array_seek(h_temp, day_start);
                for (int i = day_start; i < h_count && i < day_start + 24; i++) {
                    if (ht) {
                        if (cJSON_IsString(ht) && strlen(ht->valuestring) >= 10) {
                            int d = (ht->valuestring[8] - '0') * 10 + (ht->valuestring[9] - '0');
                            if (d != tm_now.tm_mday) break;
                        }
                        ht = ht->next;
                    }
                    if (tv) {
                        float v = (float)tv->valuedouble;
                        if (v > hi) hi = v;
                        if (v < lo) lo = v;
                        tv = tv->next;
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
    heap_caps_free(body);
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
        out->temp_current = json_num_or(u, "temp", 0.0f);
        out->dew_point    = json_num_or(u, "dewpt", 0.0f);
        out->wind_speed   = json_num_or(u, "windSpeed", 0.0f);
    }

    out->humidity = json_num_or(obs0, "humidity", 0.0f);

    cJSON *winddir = cJSON_GetObjectItem(obs0, "winddir");
    if (winddir) {
        deg_to_compass((float)winddir->valuedouble, out->wind_dir, sizeof(out->wind_dir));
    }

    /* WU current doesn't include condition text — leave blank */
    out->condition[0] = '\0';

    /* UV is at the top level of the PWS observation */
    out->uv_index = json_num_or(obs0, "uv", -1.0f);

    /* High/low not in PWS current — set defaults, updated by forecast below */
    out->temp_high = out->temp_current;
    out->temp_low  = out->temp_current;

    cJSON_Delete(json);

    /* ── Hourly forecast ──
     * Try the Weather.com v3 forecast endpoint first (works with paid WU keys).
     * If that fails (free WU keys only cover v2/pws endpoints), fall back to
     * Open-Meteo which is free and always available.
     */
    bool got_forecast = false;

    /* Attempt 1: Weather.com v3 hourly forecast */
    snprintf(url, sizeof(url),
             "https://api.weather.com/v3/wx/forecast/hourly/12hour?"
             "geocode=%.4f,%.4f&apiKey=%s&units=%s&language=en-US&format=json",
             cfg->weather_lat, cfg->weather_lon, cfg->weather_api_key, units_code);

    body = http_get_body(url);
    if (body) {
        json = cJSON_Parse(body);
        heap_caps_free(body);
        if (json) {
            cJSON *temps = cJSON_GetObjectItem(json, "temperature");
            if (temps && cJSON_IsArray(temps)) {
                int count = cJSON_GetArraySize(temps);
                if (count > 10) count = 10;

                time_t now;
                time(&now);
                struct tm tm_now;
                localtime_r(&now, &tm_now);

                int slot = -1;
                cJSON *tv = NULL;
                cJSON_ArrayForEach(tv, temps) {
                    if (++slot >= count) break;
                    out->hourly_temps[slot] = (float)tv->valuedouble;
                    out->hourly_hours[slot] = (uint8_t)((tm_now.tm_hour + slot) % 24);
                }

                /* Update high/low from forecast */
                for (int i = 0; i < count; i++) {
                    if (out->hourly_temps[i] > out->temp_high) out->temp_high = out->hourly_temps[i];
                    if (out->hourly_temps[i] < out->temp_low)  out->temp_low  = out->hourly_temps[i];
                }
                got_forecast = true;
            }
            cJSON_Delete(json);
        }
    }

    /* Attempt 2: Open-Meteo fallback (free, no API key) */
    if (!got_forecast && cfg->weather_lat != 0.0f && cfg->weather_lon != 0.0f) {
        ESP_LOGI(TAG, "WU v3 forecast unavailable, falling back to Open-Meteo");

        const char *temp_unit = (cfg->weather_units == 0) ? "fahrenheit" : "celsius";
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?"
                 "latitude=%.4f&longitude=%.4f"
                 "&hourly=temperature_2m,uv_index"
                 "&daily=temperature_2m_max,temperature_2m_min"
                 "&temperature_unit=%s&forecast_days=2&timezone=auto",
                 cfg->weather_lat, cfg->weather_lon, temp_unit);

        body = http_get_body(url);
        if (body) {
            json = cJSON_Parse(body);
            heap_caps_free(body);
            if (json) {
                cJSON *hourly = cJSON_GetObjectItem(json, "hourly");
                if (hourly) {
                    cJSON *h_time = cJSON_GetObjectItem(hourly, "time");
                    cJSON *h_temp = cJSON_GetObjectItem(hourly, "temperature_2m");
                    cJSON *h_uv   = cJSON_GetObjectItem(hourly, "uv_index");
                    int h_count = h_time ? cJSON_GetArraySize(h_time) : 0;

                    /* Find current hour in the hourly array */
                    time_t now;
                    time(&now);
                    struct tm tm_now;
                    localtime_r(&now, &tm_now);

                    int start_idx = -1;
                    int idx = 0;
                    cJSON *t_item = NULL;
                    cJSON_ArrayForEach(t_item, h_time) {
                        if (cJSON_IsString(t_item) && strlen(t_item->valuestring) >= 13) {
                            const char *ts = t_item->valuestring;
                            int h = (ts[11] - '0') * 10 + (ts[12] - '0');
                            int d = (ts[8] - '0') * 10 + (ts[9] - '0');
                            if (d == tm_now.tm_mday && h == tm_now.tm_hour) {
                                start_idx = idx;
                                break;
                            }
                        }
                        idx++;
                    }

                    if (start_idx >= 0 && h_temp) {
                        cJSON *tv = array_seek(h_temp, start_idx);
                        cJSON *ti = array_seek(h_time, start_idx);
                        for (int i = 0; i < 10 && (start_idx + i) < h_count; i++) {
                            if (tv) {
                                out->hourly_temps[i] = (float)tv->valuedouble;
                                tv = tv->next;
                            }
                            if (ti) {
                                if (cJSON_IsString(ti) && strlen(ti->valuestring) >= 13) {
                                    out->hourly_hours[i] = (uint8_t)(
                                        (ti->valuestring[11] - '0') * 10 +
                                        (ti->valuestring[12] - '0'));
                                }
                                ti = ti->next;
                            }
                        }
                        got_forecast = true;
                    }

                    /* UV index from Open-Meteo */
                    if (h_uv && start_idx >= 0) {
                        cJSON *uv_item = array_seek(h_uv, start_idx);
                        if (uv_item) out->uv_index = (float)uv_item->valuedouble;
                    }
                }

                /* Daily high/low from Open-Meteo */
                cJSON *daily = cJSON_GetObjectItem(json, "daily");
                if (daily) {
                    cJSON *d_max = cJSON_GetObjectItem(daily, "temperature_2m_max");
                    cJSON *d_min = cJSON_GetObjectItem(daily, "temperature_2m_min");
                    if (d_max && cJSON_IsArray(d_max) && cJSON_GetArraySize(d_max) > 0) {
                        cJSON *v = cJSON_GetArrayItem(d_max, 0);
                        if (v) out->temp_high = (float)v->valuedouble;
                    }
                    if (d_min && cJSON_IsArray(d_min) && cJSON_GetArraySize(d_min) > 0) {
                        cJSON *v = cJSON_GetArrayItem(d_min, 0);
                        if (v) out->temp_low = (float)v->valuedouble;
                    }
                }

                cJSON_Delete(json);
            }
        }
    }

    return true;
}

// =============================================================================
// Poll task
// =============================================================================

/* Poll interval (seconds) from the cfg snapshot taken during the most recent
 * successful poll_once() -- stashed here so weather_interval_ms() (called by
 * poll_loop_run right after a successful poll_once()) can clamp it without
 * re-snapshotting the config. Only read/written from the weather poll task. */
static uint32_t s_last_poll_interval_s;

static bool weather_poll_once(void *arg) {
    (void)arg;

    /* app_config_t is ~20 KB; never place it on this task's stack. Snapshot
     * into a PSRAM heap buffer and free on every return path. */
    app_config_t *cfg_snap = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (cfg_snap == NULL) {
        ESP_LOGE(TAG, "Weather poll: config snapshot alloc failed");
        return false;
    }
    app_config_get_snapshot_into(cfg_snap);

    /* Skip if no location configured */
    bool has_location = (cfg_snap->weather_location_name[0] != '\0');
    /* OWM and WU need an API key */
    bool needs_key = (cfg_snap->weather_provider == 0 || cfg_snap->weather_provider == 2);
    bool has_key   = (cfg_snap->weather_api_key[0] != '\0');

    if (!has_location || (needs_key && !has_key)) {
        ESP_LOGD(TAG, "Weather not configured (provider=%d), sleeping 60s",
                 cfg_snap->weather_provider);
        heap_caps_free(cfg_snap);
        return false; /* backoff is fixed at WEATHER_RETRY_INTERVAL_S -- same as fetch failure */
    }

    /* Fetch from selected provider */
    weather_data_t local;
    memset(&local, 0, sizeof(local));
    local.uv_index = -1.0f;

    bool ok = false;
    switch (cfg_snap->weather_provider) {
        case 0:  ok = fetch_owm(cfg_snap, &local);          break;
        case 1:  ok = fetch_open_meteo(cfg_snap, &local);   break;
        case 2:  ok = fetch_wunderground(cfg_snap, &local);  break;
        default:
            ESP_LOGW(TAG, "Unknown weather provider: %d", cfg_snap->weather_provider);
            break;
    }

    if (!ok) {
        ESP_LOGW(TAG, "Weather fetch failed, retrying in %ds", WEATHER_RETRY_INTERVAL_S);
        heap_caps_free(cfg_snap);
        return false;
    }

    local.valid = true;
    local.last_update_ts = (int64_t)time(NULL);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(&s_data, &local, sizeof(weather_data_t));
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Weather updated: %.1f%s, %s",
             local.temp_current,
             (cfg_snap->weather_units == 0) ? "F" : "C",
             local.condition);

    /* Trigger immediate clock UI refresh */
    clock_page_request_update();

    s_last_poll_interval_s = cfg_snap->weather_poll_interval_s;
    heap_caps_free(cfg_snap);
    return true;
}

static uint32_t weather_interval_ms(void *arg) {
    (void)arg;

    /* Normal poll interval, clamped 15 min - 1 hour */
    uint32_t interval_ms = s_last_poll_interval_s * 1000;
    if (interval_ms < 900000)   interval_ms = 900000;   /* min 15 min */
    if (interval_ms > 3600000)  interval_ms = 3600000;  /* max 1 hour */
    return interval_ms;
}

static void weather_poll_task(void *arg) {
    (void)arg;

    poll_loop_spec_t spec = {
        .name = "weather",
        .wifi_group = s_wifi_event_group,
        .wifi_bits = WIFI_CONNECTED_BIT,
        .page_active = &clock_page_active,
        .poll_once = weather_poll_once,
        .interval_ms = weather_interval_ms,
        /* Both the "not configured" and "fetch failed" paths retry at a flat
         * WEATHER_RETRY_INTERVAL_S: initial == max means poll_backoff_next()
         * always returns WEATHER_RETRY_INTERVAL_S * 1000, never doubling. */
        .backoff_initial_ms = WEATHER_RETRY_INTERVAL_S * 1000,
        .backoff_max_ms = WEATHER_RETRY_INTERVAL_S * 1000,
    };

    poll_loop_run(&spec, NULL);
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
