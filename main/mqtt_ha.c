#include "mqtt_ha.h"
#include "app_config.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "ui/nina_dashboard.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "mqtt_ha";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;
static char s_device_id[16];  // Last 6 hex chars of MAC

// Topic buffers
static char s_topic_screen_cmd[128];
static char s_topic_screen_state[128];
static char s_topic_text_cmd[128];
static char s_topic_text_state[128];
static char s_topic_reboot_cmd[128];
static char s_topic_avail[128];

static void build_topics(const char *prefix)
{
    snprintf(s_topic_screen_cmd, sizeof(s_topic_screen_cmd), "%s/screen/set", prefix);
    snprintf(s_topic_screen_state, sizeof(s_topic_screen_state), "%s/screen/state", prefix);
    snprintf(s_topic_text_cmd, sizeof(s_topic_text_cmd), "%s/text/set", prefix);
    snprintf(s_topic_text_state, sizeof(s_topic_text_state), "%s/text/state", prefix);
    snprintf(s_topic_reboot_cmd, sizeof(s_topic_reboot_cmd), "%s/reboot/set", prefix);
    snprintf(s_topic_avail, sizeof(s_topic_avail), "%s/status", prefix);
}

static void init_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
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
    cJSON_AddStringToObject(dev, "name", "NINA Display");
    cJSON_AddStringToObject(dev, "model", "ESP32-P4-WIFI6-Touch-LCD-4B");
    cJSON_AddStringToObject(dev, "manufacturer", "Waveshare");
    cJSON_AddStringToObject(dev, "sw_version", "1.0.0");

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

static void handle_screen_command(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    app_config_t *cfg = app_config_get();

    cJSON *state = cJSON_GetObjectItem(root, "state");
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");

    if (cJSON_IsString(state) && strcmp(state->valuestring, "OFF") == 0) {
        cfg->brightness = 0;
    } else if (cJSON_IsNumber(brightness)) {
        int val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->brightness = val;
    } else if (cJSON_IsString(state) && strcmp(state->valuestring, "ON") == 0) {
        if (cfg->brightness == 0) cfg->brightness = 50;
    }

    bsp_display_brightness_set(cfg->brightness);
    ESP_LOGI(TAG, "Screen brightness set to %d%% via MQTT", cfg->brightness);

    cJSON_Delete(root);
    mqtt_ha_publish_state();
}

static void handle_text_command(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    app_config_t *cfg = app_config_get();

    cJSON *state = cJSON_GetObjectItem(root, "state");
    cJSON *brightness = cJSON_GetObjectItem(root, "brightness");

    if (cJSON_IsString(state) && strcmp(state->valuestring, "OFF") == 0) {
        cfg->color_brightness = 0;
    } else if (cJSON_IsNumber(brightness)) {
        int val = brightness->valueint;
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        cfg->color_brightness = val;
    } else if (cJSON_IsString(state) && strcmp(state->valuestring, "ON") == 0) {
        if (cfg->color_brightness == 0) cfg->color_brightness = 100;
    }

    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        nina_dashboard_apply_theme(cfg->theme_index);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Display lock timeout (MQTT theme apply)");
    }

    ESP_LOGI(TAG, "Text brightness set to %d%% via MQTT", cfg->color_brightness);

    cJSON_Delete(root);
    mqtt_ha_publish_state();
}

static void handle_reboot_command(const char *data, int len)
{
    if (len >= 5 && strncmp(data, "PRESS", 5) == 0) {
        ESP_LOGW(TAG, "Reboot requested via MQTT");
        esp_restart();
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

        // Publish availability
        esp_mqtt_client_publish(s_mqtt_client, s_topic_avail, "online", 0, 1, 1);

        // Publish HA discovery configs
        publish_discovery();

        // Subscribe to command topics
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_screen_cmd, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_text_cmd, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_reboot_cmd, 1);

        // Publish current state
        mqtt_ha_publish_state();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_DATA:
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

    if (s_connected) {
        esp_mqtt_client_publish(s_mqtt_client, s_topic_avail, "offline", 0, 1, 1);
    }

    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_connected = false;
    ESP_LOGI(TAG, "MQTT client stopped");
}

bool mqtt_ha_is_connected(void)
{
    return s_connected;
}
