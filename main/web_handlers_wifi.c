#include "web_server_internal.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include <string.h>

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
    REQUIRE_AUTH(req);

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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);

    free(json);
    cJSON_Delete(root);
    heap_caps_free(ap_records);
    return ESP_OK;
}
