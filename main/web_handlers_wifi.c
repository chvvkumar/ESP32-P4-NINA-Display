#include "web_server_internal.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include <string.h>
#include "esp_netif.h"
#include "ui/nina_setup_screen.h"

#define MAX_SCAN_AP_RECORDS  30
#define MAX_SCAN_RESULTS     20

static const char *auth_mode_to_string(wifi_auth_mode_t auth)
{
    switch (auth) {
        case WIFI_AUTH_OPEN:            return "Open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_ENT_192:    return "WPA3-Enterprise";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        default:                        return "Unknown";
    }
}

static bool is_auth_compatible(wifi_auth_mode_t auth)
{
    switch (auth) {
        case WIFI_AUTH_OPEN:
        case WIFI_AUTH_WPA_PSK:
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return true;
        default:
            return false;
    }
}

static int compare_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;
    return ap_b->rssi - ap_a->rssi;
}

esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    if (!is_setup_mode()) {
        REQUIRE_AUTH(req);
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", "WiFi scan failed");
        cJSON_AddNumberToObject(root, "code", err);
        char *json = cJSON_PrintUnformatted(root);
        if (!json) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(root);
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MAX_SCAN_AP_RECORDS) {
        ap_count = MAX_SCAN_AP_RECORDS;
    }

    wifi_ap_record_t *ap_records = heap_caps_malloc(ap_count * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    if (!ap_records) {
        esp_wifi_clear_ap_list();
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", "Out of memory for scan results");
        cJSON_AddNumberToObject(root, "code", ESP_ERR_NO_MEM);
        char *json = cJSON_PrintUnformatted(root);
        if (!json) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(root);
        return ESP_OK;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    qsort(ap_records, ap_count, sizeof(wifi_ap_record_t), compare_rssi_desc);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    int result_count = 0;
    int incompatible_count = 0;
    char seen_ssids[MAX_SCAN_RESULTS][33];
    int seen_count = 0;

    for (int i = 0; i < ap_count && result_count < MAX_SCAN_RESULTS; i++) {
        const char *ssid = (const char *)ap_records[i].ssid;

        if (ssid[0] == '\0') continue;

        bool duplicate = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_ssids[j], ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        strncpy(seen_ssids[seen_count], ssid, 32);
        seen_ssids[seen_count][32] = '\0';
        seen_count++;

        bool compatible = is_auth_compatible(ap_records[i].authmode);
        if (!compatible) incompatible_count++;

        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", ssid);
        cJSON_AddNumberToObject(net, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(net, "channel", ap_records[i].primary);
        cJSON_AddStringToObject(net, "auth", auth_mode_to_string(ap_records[i].authmode));
        cJSON_AddBoolToObject(net, "compatible", compatible);
        cJSON_AddItemToArray(networks, net);
        result_count++;
    }

    cJSON_AddNumberToObject(root, "count", result_count);
    cJSON_AddNumberToObject(root, "incompatible_count", incompatible_count);

    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        cJSON_Delete(root);
        heap_caps_free(ap_records);
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    heap_caps_free(ap_records);
    return ESP_OK;
}

esp_err_t wifi_setup_post_handler(httpd_req_t *req)
{
    if (!is_setup_mode()) {
        REQUIRE_AUTH(req);
    }

    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return send_400(req, "Empty request body");

    cJSON *root = cJSON_Parse(buf);
    if (!root) return send_400(req, "Invalid JSON");

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_400(req, "SSID is required");
    }

    /* Work on a mutex-protected snapshot copy; never field-write the live config.
     * Heap-allocated in PSRAM (app_config_t is ~7.6 KB — too large for the httpd stack). */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    app_config_get_snapshot_into(cfg);
    strlcpy(cfg->wifi_networks[0].ssid, ssid_item->valuestring,
            sizeof(cfg->wifi_networks[0].ssid));
    if (cJSON_IsString(pass_item)) {
        strlcpy(cfg->wifi_networks[0].password, pass_item->valuestring,
                sizeof(cfg->wifi_networks[0].password));
    } else {
        cfg->wifi_networks[0].password[0] = '\0';
    }
    /* Single atomic memcpy under mutex + NVS persist. */
    app_config_save(cfg);
    heap_caps_free(cfg);

    cJSON_Delete(root);

    extern void wifi_connect_to_slot(int index);
    esp_wifi_disconnect();
    wifi_connect_to_slot(0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    if (!is_setup_mode()) {
        REQUIRE_AUTH(req);
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    bool connected = false;
    char ip_str[16] = "";
    char ssid_str[33] = "";

    if (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        connected = true;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

        wifi_config_t wcfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &wcfg) == ESP_OK) {
            strlcpy(ssid_str, (const char *)wcfg.sta.ssid, sizeof(ssid_str));
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddStringToObject(root, "ssid", ssid_str);
    cJSON_AddStringToObject(root, "ip", ip_str);

    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}
