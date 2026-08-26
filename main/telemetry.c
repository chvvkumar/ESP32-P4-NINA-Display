/* Anonymous telemetry client -- see telemetry.h for the payload contract.
 * The daily POST clones the spotify_auth do_token_request() client shape
 * (crt_bundle, open/write/fetch_headers, discard body, cleanup, no retry). */

#include "telemetry.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "driver/temperature_sensor.h"

#include "app_config.h"
#include "build_version.h"
#include "perf_monitor.h"
#include "power_mgmt.h"
#include "net_trace.h"
#include "tasks.h"   /* s_wifi_event_group + WIFI_CONNECTED_BIT */

static const char *TAG = "telemetry";

#define TELEMETRY_NVS_NAMESPACE   "app_conf"   /* same namespace as the config blob */
#define TELEMETRY_NVS_KEY_UUID    "dev_uuid"
#define TELEMETRY_URL             "https://ninadash.challa.co/v1/report"
#define TELEMETRY_START_DELAY_MS  (120 * 1000)      /* never contend with boot */
#define TELEMETRY_PERIOD_S        (24 * 60 * 60)
#define TELEMETRY_JITTER_MAX_S    3600              /* de-synchronize the fleet */
#define TELEMETRY_HTTP_TIMEOUT_MS 5000
#define TELEMETRY_PAYLOAD_CAP     1024

static char s_uuid[37];   /* 8-4-4-4-12 lowercase hex + NUL */

void telemetry_init(void)
{
    if (s_uuid[0] != '\0') {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;   /* no NVS, no id; the payload carries "" and the task still runs */
    }
    size_t len = sizeof(s_uuid);
    if (nvs_get_str(h, TELEMETRY_NVS_KEY_UUID, s_uuid, &len) != ESP_OK ||
        s_uuid[0] == '\0') {
        uint8_t raw[16];
        esp_fill_random(raw, sizeof(raw));
        snprintf(s_uuid, sizeof(s_uuid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                 raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
        nvs_set_str(h, TELEMETRY_NVS_KEY_UUID, s_uuid);
        nvs_commit(h);
    }
    nvs_close(h);
}

float telemetry_read_temp_c(void)
{
    /* Sole installer of the SoC temperature sensor: a second install fails,
     * so /api/status reads through here too (was a function-local static in
     * status_get_handler). A racing first call only fails its own install
     * and leaves the statics untouched; the winner's handle survives. */
    static temperature_sensor_handle_t s_tsens = NULL;
    static bool s_tsens_ready = false;
    static float s_last_c = 0.0f;

    if (!s_tsens_ready) {
        temperature_sensor_handle_t t = NULL;
        temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&tsens_cfg, &t) == ESP_OK &&
            temperature_sensor_enable(t) == ESP_OK) {
            s_tsens = t;
            s_tsens_ready = true;
        }
    }
    float c = 0.0f;
    if (s_tsens_ready && temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
        s_last_c = c;
    }
    return s_last_c;
}

int telemetry_build_payload(char *buf, size_t cap, bool include_crash)
{
    if (!buf || cap == 0) {
        return -1;
    }
    const app_config_t *cfg = app_config_get();

    const esp_partition_t *part = esp_ota_get_running_partition();
    const char *part_label = part ? part->label : "";

    power_mgmt_crash_info_t ci = power_mgmt_get_crash_info();
    uint32_t reset_reason = power_mgmt_get_last_reset_reason();
    const char *reason_str = power_mgmt_reset_reason_str(reset_reason);

    float cpu0 = 0.0f, cpu1 = 0.0f;
    perf_monitor_get_core_loads(&cpu0, &cpu1, NULL);

    wifi_ap_record_t ap;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    uint32_t pages = 0;
    if (cfg->allsky_enabled)      pages |= 1u << 0;
    if (cfg->spotify_enabled)     pages |= 1u << 1;
    if (cfg->goes_enabled)        pages |= 1u << 2;
    if (cfg->moon_enabled)        pages |= 1u << 3;
    if (cfg->solar_enabled)       pages |= 1u << 4;
    if (cfg->custom_enabled)      pages |= 1u << 5;
    if (cfg->radar_enabled)       pages |= 1u << 6;
    if (cfg->clouds_enabled)      pages |= 1u << 7;
    if (cfg->json_enabled)        pages |= 1u << 8;
    if (cfg->ha_enabled)          pages |= 1u << 9;
    if (cfg->octoprint_enabled)   pages |= 1u << 10;
    if (cfg->flights_enabled)     pages |= 1u << 11;
    if (cfg->demo_mode)           pages |= 1u << 12;
    if (cfg->auto_rotate_enabled) pages |= 1u << 13;

    /* "weather configured" mirrors the /api/status derivation. */
    bool weather_needs_key = (cfg->weather_provider == 0 || cfg->weather_provider == 2);
    bool weather_configured = (cfg->weather_location_name[0] != '\0') &&
                              (!weather_needs_key || cfg->weather_api_key[0] != '\0');
    uint32_t integ = 0;
    if (cfg->mqtt_enabled)       integ |= 1u << 0;
    if (weather_configured)      integ |= 1u << 1;
    if (cfg->auth_enabled)       integ |= 1u << 2;
    if (cfg->debug_mode)         integ |= 1u << 3;
    if (cfg->deep_sleep_enabled) integ |= 1u << 4;
    if (cfg->audio_muted)        integ |= 1u << 5;

    int n = snprintf(buf, cap,
        "{\"id\":\"%s\",\"schema\":1,"
        "\"fw\":{\"tag\":\"%s\",\"sha\":\"%s\",\"branch\":\"%s\","
        "\"channel\":%u,\"partition\":\"%s\"},"
        "\"boot\":{\"count\":%lu,\"uptime_s\":%lld,\"reset_reason\":\"%s\"},",
        s_uuid, BUILD_GIT_TAG, BUILD_GIT_SHA, BUILD_GIT_BRANCH,
        (unsigned)cfg->update_channel, part_label,
        (unsigned long)ci.boot_count,
        (long long)(esp_timer_get_time() / 1000000),
        reason_str);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    size_t pos = (size_t)n;

    /* Crash block only after an abnormal reset (panic, WDTs, brownout,
     * CPU lockup: power_mgmt_reset_is_abnormal, the existing classifier),
     * and only when the caller wants it (first report of the boot). */
    if (include_crash && power_mgmt_reset_is_abnormal(reset_reason)) {
        n = snprintf(buf + pos, cap - pos,
            "\"crash\":{\"reason\":\"%s\",\"count\":%lu},",
            reason_str, (unsigned long)ci.crash_count);
        if (n < 0 || pos + (size_t)n >= cap) {
            return -1;
        }
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, cap - pos,
        "\"sys\":{\"heap_free\":%u,\"heap_min\":%u,"
        "\"psram_free\":%u,\"psram_largest\":%u,"
        "\"cpu0\":%d,\"cpu1\":%d,\"temp_c\":%d,\"rssi\":%d},"
        "\"features\":{\"pages\":%lu,\"integrations\":%lu}}",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (int)cpu0, (int)cpu1, (int)telemetry_read_temp_c(), rssi,
        (unsigned long)pages, (unsigned long)integ);
    if (n < 0 || pos + (size_t)n >= cap) {
        return -1;
    }
    return (int)(pos + (size_t)n);
}

/* One POST, no retry; success and failure alike are a single debug line. */
static void telemetry_send_report(bool include_crash)
{
    char *payload = heap_caps_malloc(TELEMETRY_PAYLOAD_CAP, MALLOC_CAP_SPIRAM);
    if (!payload) {
        return;
    }
    int len = telemetry_build_payload(payload, TELEMETRY_PAYLOAD_CAP, include_crash);
    if (len <= 0) {
        heap_caps_free(payload);
        return;
    }

    esp_http_client_config_t http_cfg = {
        .url = TELEMETRY_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TELEMETRY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        heap_caps_free(payload);
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    net_ev_note(pcTaskGetName(NULL));
    int status = -1;
    esp_err_t err = esp_http_client_open(client, len);
    if (err == ESP_OK && esp_http_client_write(client, payload, len) == len) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        /* Read and discard the response so the connection closes cleanly. */
        char sink[64];
        while (esp_http_client_read(client, sink, sizeof(sink)) > 0) {
        }
    }
    esp_http_client_cleanup(client);
    heap_caps_free(payload);
    ESP_LOGD(TAG, "Report POST: err=%s http=%d len=%d", esp_err_to_name(err), status, len);
}

void telemetry_task(void *arg)
{
    (void)arg;

    /* Wait for WiFi, then stay clear of the boot rush. */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(TELEMETRY_START_DELAY_MS));

    /* One panic must reach the server once, not once per daily report: the
     * crash block rides only the first report of this boot. */
    bool first_report = true;
    while (1) {
        if (app_config_get()->telemetry_enabled) {
            telemetry_send_report(first_report);
            first_report = false;
        }
        uint32_t sleep_s = TELEMETRY_PERIOD_S +
                           (esp_random() % (TELEMETRY_JITTER_MAX_S + 1u));
        /* Chunked delay: pdMS_TO_TICKS overflows 32-bit tick math for any
         * span past about 71 minutes at the 1000 Hz tick (ms * tick rate
         * wraps uint32), which turned the daily period into 8-68 minutes.
         * One-minute chunks keep every conversion far below the wrap. */
        for (uint32_t slept = 0; slept < sleep_s; slept += 60u) {
            vTaskDelay(pdMS_TO_TICKS(60u * 1000u));
        }
    }
}
