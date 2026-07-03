#include "mqtt_ha.h"
#include "build_version.h"
#include "app_config.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "tasks.h"
#include "ui/nina_dashboard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

static const char *TAG = "mqtt_ha";

/* ── Deferred command handoff (INTEG-2/3/4) ─────────────────────────────────
 * The MQTT event callback runs in the esp-mqtt event task and must stay fast
 * and non-blocking — it must not take the display lock, drive the backlight,
 * mutate live config, or write NVS (all of which can block and stall the MQTT
 * keepalive). The callback only PARSES a command and enqueues the requested
 * change here. mqtt_ha_process_pending(), called from the UI-context
 * data_update_task each cycle, applies the change under the display lock and
 * persists config via the snapshot+save pattern. */
typedef enum {
    MQTT_CMD_SCREEN_BRIGHTNESS,  /* value = 0..100 backlight brightness */
    MQTT_CMD_TEXT_BRIGHTNESS,    /* value = 0..100 color (text) brightness */
    MQTT_CMD_REBOOT,             /* deferred restart */
} mqtt_cmd_type_t;

typedef struct {
    mqtt_cmd_type_t type;
    int value;  /* clamped 0..100 for brightness commands; unused for reboot */
} mqtt_cmd_t;

#define MQTT_CMD_QUEUE_LEN 8
static QueueHandle_t s_cmd_queue = NULL;

// Exponential backoff for MQTT reconnection
#define MQTT_BACKOFF_INITIAL_MS  5000   // 5 seconds
#define MQTT_BACKOFF_MAX_MS      60000  // 60 seconds
#define MQTT_BACKOFF_MULTIPLIER  2

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;

/* Boot-time brightness restore (one-shot). MQTT brightness commands are
 * live-only (never saved to NVS), so a reboot reverts to the NVS value until
 * HA publishes again. The device publishes its screen/text state topics
 * retained, so the broker already holds the last live brightness. On the
 * first connect after boot we subscribe to our own state topics, apply the
 * retained payload through the normal command queue, then unsubscribe. A
 * non-retained message on a state topic is the echo of our own connect-time
 * state publish and means the broker had no retained value — stop waiting. */
static bool s_restore_screen_pending = true;
static bool s_restore_text_pending = true;
static char s_device_id[16];  // Last 6 hex chars of MAC
static uint32_t s_backoff_ms = MQTT_BACKOFF_INITIAL_MS;
static esp_timer_handle_t s_reconnect_timer = NULL;

// Topic buffers
static char s_topic_screen_cmd[128];
static char s_topic_screen_state[128];
static char s_topic_text_cmd[128];
static char s_topic_text_state[128];
static char s_topic_reboot_cmd[128];
static char s_topic_uptime_state[128];
static char s_topic_avail[128];

static void build_topics(const char *prefix)
{
    snprintf(s_topic_screen_cmd, sizeof(s_topic_screen_cmd), "%s/screen/set", prefix);
    snprintf(s_topic_screen_state, sizeof(s_topic_screen_state), "%s/screen/state", prefix);
    snprintf(s_topic_text_cmd, sizeof(s_topic_text_cmd), "%s/text/set", prefix);
    snprintf(s_topic_text_state, sizeof(s_topic_text_state), "%s/text/state", prefix);
    snprintf(s_topic_reboot_cmd, sizeof(s_topic_reboot_cmd), "%s/reboot/set", prefix);
    snprintf(s_topic_uptime_state, sizeof(s_topic_uptime_state), "%s/uptime/state", prefix);
    snprintf(s_topic_avail, sizeof(s_topic_avail), "%s/status", prefix);
}

static void init_device_id(void)
{
    uint8_t mac[6];
    /* ESP_MAC_WIFI_STA fails on ESP32-P4 (remote coprocessor), use base MAC from eFuse */
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        /* Fallback: derive from hostname so each device is still unique */
        const char *host = app_config_get()->hostname;
        snprintf(s_device_id, sizeof(s_device_id), "%.15s", host[0] ? host : "unknown");
        return;
    }
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static char *get_device_ip(void)
{
    static char ip_str[16];
    ip_str[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }
    return ip_str;
}

/**
 * @brief Build the shared HA device JSON object for discovery payloads
 */
static cJSON *create_device_object(void)
{
    cJSON *dev = cJSON_CreateObject();
    char uid[32];
    snprintf(uid, sizeof(uid), "ninadisplay_%s", s_device_id);

    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(uid));
    cJSON_AddItemToObject(dev, "identifiers", ids);
    const char *hostname = app_config_get()->hostname;
    cJSON_AddStringToObject(dev, "name", hostname[0] ? hostname : "NINA Display");
    cJSON_AddStringToObject(dev, "model", "ESP32-P4-WIFI6-Touch-LCD-4B");
    cJSON_AddStringToObject(dev, "manufacturer", "Waveshare");
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char sw_ver[96];
    snprintf(sw_ver, sizeof(sw_ver), "%s (%s)", app_desc->version, BUILD_GIT_TAG);
    cJSON_AddStringToObject(dev, "sw_version", sw_ver);

    char config_url[48];
    snprintf(config_url, sizeof(config_url), "http://%s", get_device_ip());
    cJSON_AddStringToObject(dev, "configuration_url", config_url);

    return dev;
}

static void publish_discovery(void)
{
    if (!s_mqtt_client || !s_connected) return;

    const char *prefix = app_config_get()->mqtt_topic_prefix;
    char disc_topic[192];
    char uid_buf[64];

    // --- Screen Brightness Light ---
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/light/%s_screen/config", prefix);
    snprintf(uid_buf, sizeof(uid_buf), "ninadisplay_%s_screen", s_device_id);

    cJSON *screen = cJSON_CreateObject();
    cJSON_AddStringToObject(screen, "name", "Screen Brightness");
    cJSON_AddStringToObject(screen, "unique_id", uid_buf);
    cJSON_AddStringToObject(screen, "object_id", uid_buf);
    cJSON_AddStringToObject(screen, "schema", "json");
    cJSON_AddStringToObject(screen, "command_topic", s_topic_screen_cmd);
    cJSON_AddStringToObject(screen, "state_topic", s_topic_screen_state);
    cJSON_AddStringToObject(screen, "availability_topic", s_topic_avail);
    cJSON_AddBoolToObject(screen, "brightness", true);
    cJSON_AddNumberToObject(screen, "brightness_scale", 100);
    cJSON_AddStringToObject(screen, "icon", "mdi:brightness-6");
    cJSON_AddItemToObject(screen, "device", create_device_object());

    char *payload = cJSON_PrintUnformatted(screen);
    if (payload) {
        esp_mqtt_client_publish(s_mqtt_client, disc_topic, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(screen);

    // --- Text Brightness Light ---
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/light/%s_text/config", prefix);
    snprintf(uid_buf, sizeof(uid_buf), "ninadisplay_%s_text", s_device_id);

    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "name", "Text Brightness");
    cJSON_AddStringToObject(text, "unique_id", uid_buf);
    cJSON_AddStringToObject(text, "object_id", uid_buf);
    cJSON_AddStringToObject(text, "schema", "json");
    cJSON_AddStringToObject(text, "command_topic", s_topic_text_cmd);
    cJSON_AddStringToObject(text, "state_topic", s_topic_text_state);
    cJSON_AddStringToObject(text, "availability_topic", s_topic_avail);
    cJSON_AddBoolToObject(text, "brightness", true);
    cJSON_AddNumberToObject(text, "brightness_scale", 100);
    cJSON_AddStringToObject(text, "icon", "mdi:format-color-text");
    cJSON_AddItemToObject(text, "device", create_device_object());

    payload = cJSON_PrintUnformatted(text);
    if (payload) {
        esp_mqtt_client_publish(s_mqtt_client, disc_topic, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(text);

    // --- Reboot Button ---
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/button/%s_reboot/config", prefix);
    snprintf(uid_buf, sizeof(uid_buf), "ninadisplay_%s_reboot", s_device_id);

    cJSON *reboot = cJSON_CreateObject();
    cJSON_AddStringToObject(reboot, "name", "Reboot");
    cJSON_AddStringToObject(reboot, "unique_id", uid_buf);
    cJSON_AddStringToObject(reboot, "object_id", uid_buf);
    cJSON_AddStringToObject(reboot, "command_topic", s_topic_reboot_cmd);
    cJSON_AddStringToObject(reboot, "payload_press", "PRESS");
    cJSON_AddStringToObject(reboot, "availability_topic", s_topic_avail);
    cJSON_AddStringToObject(reboot, "icon", "mdi:restart");
    cJSON_AddStringToObject(reboot, "device_class", "restart");
    cJSON_AddItemToObject(reboot, "device", create_device_object());

    payload = cJSON_PrintUnformatted(reboot);
    if (payload) {
        esp_mqtt_client_publish(s_mqtt_client, disc_topic, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(reboot);

    // --- Uptime Diagnostic Sensor ---
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/sensor/%s_uptime/config", prefix);
    snprintf(uid_buf, sizeof(uid_buf), "ninadisplay_%s_uptime", s_device_id);

    cJSON *uptime = cJSON_CreateObject();
    cJSON_AddStringToObject(uptime, "name", "Uptime");
    cJSON_AddStringToObject(uptime, "unique_id", uid_buf);
    cJSON_AddStringToObject(uptime, "object_id", uid_buf);
    cJSON_AddStringToObject(uptime, "state_topic", s_topic_uptime_state);
    cJSON_AddStringToObject(uptime, "availability_topic", s_topic_avail);
    cJSON_AddStringToObject(uptime, "device_class", "duration");
    cJSON_AddStringToObject(uptime, "unit_of_measurement", "s");
    cJSON_AddStringToObject(uptime, "icon", "mdi:timer-outline");
    cJSON_AddStringToObject(uptime, "entity_category", "diagnostic");
    cJSON_AddItemToObject(uptime, "device", create_device_object());

    payload = cJSON_PrintUnformatted(uptime);
    if (payload) {
        esp_mqtt_client_publish(s_mqtt_client, disc_topic, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(uptime);

    ESP_LOGI(TAG, "HA discovery configs published");
}

void mqtt_ha_publish_state(void)
{
    if (!s_mqtt_client || !s_connected) return;

    app_config_t *cfg = app_config_get();

    // Screen brightness state
    cJSON *screen_state = cJSON_CreateObject();
    cJSON_AddStringToObject(screen_state, "state", cfg->brightness > 0 ? "ON" : "OFF");
    cJSON_AddNumberToObject(screen_state, "brightness", cfg->brightness);
    char *payload = cJSON_PrintUnformatted(screen_state);
    if (payload) {
        esp_mqtt_client_publish(s_mqtt_client, s_topic_screen_state, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(screen_state);

    // Uptime (seconds since boot)
    int64_t uptime_us = esp_timer_get_time();
    char uptime_str[24];
    snprintf(uptime_str, sizeof(uptime_str), "%lld", (long long)(uptime_us / 1000000));
    esp_mqtt_client_publish(s_mqtt_client, s_topic_uptime_state, uptime_str, 0, 0, 0);

    // Text brightness state
    cJSON *text_state = cJSON_CreateObject();
    cJSON_AddStringToObject(text_state, "state", cfg->color_brightness > 0 ? "ON" : "OFF");
    cJSON_AddNumberToObject(text_state, "brightness", cfg->color_brightness);
    payload = cJSON_PrintUnformatted(text_state);
    if (payload) {
        esp_mqtt_client_publish(s_mqtt_client, s_topic_text_state, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(text_state);
}

/* Non-blocking enqueue from the MQTT event task. Drops the command if the
 * queue is full rather than blocking the event loop. */
static void enqueue_cmd(mqtt_cmd_type_t type, int value)
{
    if (!s_cmd_queue) return;
    mqtt_cmd_t cmd = { .type = type, .value = value };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MQTT command queue full — dropping command %d", (int)type);
    }
}

/* Parse-only: resolve the requested brightness from the JSON payload and hand
 * it off. Runs in the MQTT event task — no config writes, no display lock. */
static void handle_screen_command(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    cJSON *state = cJSON_GetObjectItem(root, "state");
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");

    int val = -1;  /* -1 = "ON with no explicit value" sentinel */
    if (cJSON_IsString(state) && strcmp(state->valuestring, "OFF") == 0) {
        val = 0;
    } else if (cJSON_IsNumber(brightness)) {
        val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
    } else if (cJSON_IsString(state) && strcmp(state->valuestring, "ON") == 0) {
        val = -1;  /* consumer turns a 0 brightness back on at a default */
    } else {
        cJSON_Delete(root);
        return;  /* nothing actionable */
    }

    enqueue_cmd(MQTT_CMD_SCREEN_BRIGHTNESS, val);
    cJSON_Delete(root);
}

static void handle_text_command(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    cJSON *state = cJSON_GetObjectItem(root, "state");
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");

    int val = -1;  /* -1 = "ON with no explicit value" sentinel */
    if (cJSON_IsString(state) && strcmp(state->valuestring, "OFF") == 0) {
        val = 0;
    } else if (cJSON_IsNumber(brightness)) {
        val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
    } else if (cJSON_IsString(state) && strcmp(state->valuestring, "ON") == 0) {
        val = -1;
    } else {
        cJSON_Delete(root);
        return;
    }

    enqueue_cmd(MQTT_CMD_TEXT_BRIGHTNESS, val);
    cJSON_Delete(root);
}

static void handle_reboot_command(const char *data, int len)
{
    if (len >= 5 && strncmp(data, "PRESS", 5) == 0) {
        ESP_LOGW(TAG, "Reboot requested via MQTT (deferred)");
        enqueue_cmd(MQTT_CMD_REBOOT, 0);
    }
}

void mqtt_ha_process_pending(void)
{
    if (!s_cmd_queue) return;

    bool brightness_applied = false;
    bool reboot_requested = false;

    mqtt_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
        case MQTT_CMD_SCREEN_BRIGHTNESS: {
            /* LIVE-ONLY: never persist MQTT brightness. NVS writes run with the
             * CPU cache disabled, which can starve the esp-hosted SDIO transport
             * and trigger a host restart. An HA lux automation publishes these
             * commands very frequently, so we apply live and never save. Manual
             * UI / web changes still persist via their own handlers. */
            int newv = cmd.value;
            if (newv < 0) {  /* "ON" with no value: restore a sane default if off */
                int cur = app_config_get()->brightness;  /* scalar read, no save */
                newv = (cur == 0) ? 50 : cur;
            }
            /* Apply backlight live unless screen-sleep owns it. The backlight set
             * is config-independent and works without any config write. */
            if (!screen_asleep) {
                bsp_display_brightness_set(newv);
                ESP_LOGI(TAG, "Screen brightness set to %d%% via MQTT (live, not saved)", newv);
            } else {
                ESP_LOGI(TAG, "Screen brightness %d%% via MQTT ignored (display sleeping, live-only)", newv);
            }
            brightness_applied = true;
            break;
        }
        case MQTT_CMD_TEXT_BRIGHTNESS: {
            /* LIVE-ONLY: never persist. The rendered theme reads color_brightness
             * from the live app_config (apply_theme_to_page() in nina_dashboard.c),
             * so the live effect requires the in-memory value to be visible to the
             * theme code. Replicate the web UI live-preview (color_brightness_post_handler
             * in web_handlers_display.c): write the single scalar field on the live
             * config, then re-apply the theme — but skip app_config_save(). A single
             * aligned int store is the same mechanism the UI uses; no torn-write race. */
            app_config_t *cfg = app_config_get();
            int newv = cmd.value;
            if (newv < 0) {  /* "ON" with no value */
                newv = (cfg->color_brightness == 0) ? 100 : cfg->color_brightness;
            }
            cfg->color_brightness = newv;  /* live scalar write, NOT saved */
            /* Re-apply theme so the new color brightness takes effect live. */
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_apply_theme(cfg->theme_index);
                bsp_display_unlock();
            } else {
                ESP_LOGW(TAG, "Display lock timeout (MQTT theme apply)");
            }
            ESP_LOGI(TAG, "Text brightness set to %d%% via MQTT (live, not saved)", newv);
            brightness_applied = true;
            break;
        }
        case MQTT_CMD_REBOOT:
            reboot_requested = true;
            break;
        }
    }

    /* Publish current (live) state so HA optimistic state stays in sync. Nothing
     * is persisted; this only reflects the value we just applied in memory. */
    if (brightness_applied) {
        mqtt_ha_publish_state();
    }

    if (reboot_requested) {
        ESP_LOGW(TAG, "Rebooting now (MQTT request)");
        esp_restart();
    }
}

static void reconnect_timer_cb(void *arg)
{
    if (s_mqtt_client && !s_connected) {
        ESP_LOGI(TAG, "Attempting MQTT reconnect (backoff: %"PRIu32" ms)", s_backoff_ms);
        esp_mqtt_client_reconnect(s_mqtt_client);
    }
}

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer || !s_mqtt_client) return;

    // Stop any pending reconnect before scheduling a new one
    esp_timer_stop(s_reconnect_timer);

    ESP_LOGD(TAG, "Scheduling MQTT reconnect in %"PRIu32" ms", s_backoff_ms);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);

    // Increase backoff for next attempt, capped at max
    s_backoff_ms = s_backoff_ms * MQTT_BACKOFF_MULTIPLIER;
    if (s_backoff_ms > MQTT_BACKOFF_MAX_MS) {
        s_backoff_ms = MQTT_BACKOFF_MAX_MS;
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        s_connected = true;
        s_backoff_ms = MQTT_BACKOFF_INITIAL_MS;  // Reset backoff on successful connect

        // Publish availability
        esp_mqtt_client_publish(s_mqtt_client, s_topic_avail, "online", 0, 1, 1);

        // Publish HA discovery configs
        publish_discovery();

        // Subscribe to command topics
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_screen_cmd, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_text_cmd, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_reboot_cmd, 1);

        /* Boot-time restore: subscribe to our own retained state topics BEFORE
         * the state publish below. TCP ordering guarantees the broker processes
         * the SUBSCRIBE first, so the retained delivery snapshots the pre-reboot
         * value even though we immediately publish the NVS state afterwards. */
        if (s_restore_screen_pending) {
            esp_mqtt_client_subscribe(s_mqtt_client, s_topic_screen_state, 1);
        }
        if (s_restore_text_pending) {
            esp_mqtt_client_subscribe(s_mqtt_client, s_topic_text_state, 1);
        }

        // Publish current state
        mqtt_ha_publish_state();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected from broker");
        s_connected = false;
        schedule_reconnect();
        break;

    case MQTT_EVENT_DATA:
        /* Boot-time restore: our own state topics, subscribed one-shot above.
         * The state payload {"state":...,"brightness":N} has the same shape as
         * a command payload, so the command parsers apply directly. Retained
         * flag REQUIRED here (opposite of the command-topic rule below): the
         * retained message is the pre-reboot value; a non-retained message is
         * the echo of our own connect-time publish (no retained value existed).
         * Either way this topic's restore is finished — unsubscribe. */
        if (event->topic && event->topic_len > 0) {
            if (s_restore_screen_pending &&
                event->topic_len == (int)strlen(s_topic_screen_state) &&
                strncmp(event->topic, s_topic_screen_state, event->topic_len) == 0) {
                if (event->retain) {
                    handle_screen_command(event->data, event->data_len);
                    ESP_LOGI(TAG, "Screen brightness restored from retained MQTT state");
                }
                s_restore_screen_pending = false;
                esp_mqtt_client_unsubscribe(s_mqtt_client, s_topic_screen_state);
                break;
            }
            if (s_restore_text_pending &&
                event->topic_len == (int)strlen(s_topic_text_state) &&
                strncmp(event->topic, s_topic_text_state, event->topic_len) == 0) {
                if (event->retain) {
                    handle_text_command(event->data, event->data_len);
                    ESP_LOGI(TAG, "Text brightness restored from retained MQTT state");
                }
                s_restore_text_pending = false;
                esp_mqtt_client_unsubscribe(s_mqtt_client, s_topic_text_state);
                break;
            }
        }
        /* Ignore retained command messages. The screen/text/reboot topics are
         * Home Assistant command topics (momentary light-set / button-press); a
         * retained payload is a stale command the broker redelivers on every
         * (re)connect. Acting on a retained reboot boot-loops the device (one
         * reboot per MQTT connect). Only live commands represent real intent. */
        if (event->retain) {
            ESP_LOGW(TAG, "Ignoring retained MQTT command on subscribed topic");
            break;
        }
        if (event->topic && event->topic_len > 0) {
            if (event->topic_len == (int)strlen(s_topic_screen_cmd) &&
                strncmp(event->topic, s_topic_screen_cmd, event->topic_len) == 0) {
                handle_screen_command(event->data, event->data_len);
            } else if (event->topic_len == (int)strlen(s_topic_text_cmd) &&
                       strncmp(event->topic, s_topic_text_cmd, event->topic_len) == 0) {
                handle_text_command(event->data, event->data_len);
            } else if (event->topic_len == (int)strlen(s_topic_reboot_cmd) &&
                       strncmp(event->topic, s_topic_reboot_cmd, event->topic_len) == 0) {
                handle_reboot_command(event->data, event->data_len);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error event");
        break;

    default:
        break;
    }
}

void mqtt_ha_start(void)
{
    app_config_t *cfg = app_config_get();

    if (!cfg->mqtt_enabled || cfg->mqtt_broker_url[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled or no broker URL configured");
        return;
    }

    if (s_mqtt_client) {
        ESP_LOGW(TAG, "MQTT client already running");
        return;
    }

    init_device_id();
    build_topics(cfg->mqtt_topic_prefix);

    /* Command handoff queue: written by the MQTT event task, drained by the
     * UI-context data_update_task via mqtt_ha_process_pending(). */
    if (!s_cmd_queue) {
        s_cmd_queue = xQueueCreate(MQTT_CMD_QUEUE_LEN, sizeof(mqtt_cmd_t));
        if (!s_cmd_queue) {
            ESP_LOGE(TAG, "Failed to create MQTT command queue");
            return;
        }
    }

    // Create reconnect timer (one-shot, managed by backoff logic)
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = reconnect_timer_cb,
            .name = "mqtt_reconnect",
        };
        esp_err_t terr = esp_timer_create(&timer_args, &s_reconnect_timer);
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create reconnect timer: %s", esp_err_to_name(terr));
            return;
        }
    }
    s_backoff_ms = MQTT_BACKOFF_INITIAL_MS;

    // Build broker URI with port
    char uri[192];
    // Check if URL already has a scheme
    if (strncmp(cfg->mqtt_broker_url, "mqtt://", 7) == 0 ||
        strncmp(cfg->mqtt_broker_url, "mqtts://", 8) == 0) {
        snprintf(uri, sizeof(uri), "%s", cfg->mqtt_broker_url);
    } else {
        snprintf(uri, sizeof(uri), "mqtt://%s", cfg->mqtt_broker_url);
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .broker.address.port = cfg->mqtt_port,
        .credentials.username = cfg->mqtt_username[0] ? cfg->mqtt_username : NULL,
        .credentials.authentication.password = cfg->mqtt_password[0] ? cfg->mqtt_password : NULL,
        .session.last_will.topic = s_topic_avail,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.disable_auto_reconnect = true,  // We handle reconnect with exponential backoff
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return;
    }

    ESP_LOGI(TAG, "MQTT client started, connecting to %s:%d", uri, cfg->mqtt_port);
}

void mqtt_ha_stop(void)
{
    if (!s_mqtt_client) return;

    // Stop reconnect timer first to prevent it firing during shutdown
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }

    if (s_connected) {
        esp_mqtt_client_publish(s_mqtt_client, s_topic_avail, "offline", 0, 1, 1);
    }

    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_connected = false;
    s_backoff_ms = MQTT_BACKOFF_INITIAL_MS;
    ESP_LOGI(TAG, "MQTT client stopped");
}

bool mqtt_ha_is_connected(void)
{
    return s_connected;
}
