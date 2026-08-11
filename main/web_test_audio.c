/**
 * @file web_test_audio.c
 * @brief Standalone speaker/voice test server on port 8080.
 *
 * Second esp_http_server instance, separate from start_web_server() so the
 * test surface shares none of the main server's route table, session store or
 * auth wrapper.  ctrl_port is moved off the default 32768 (claimed by the main
 * server's HTTPD_DEFAULT_CONFIG) to 32769; two instances on the same ctrl_port
 * fail to start.
 *
 * No auth by design: the instance exposes three routes, all of which do
 * nothing but queue a sound.  Nothing else is reachable on this port.
 */

#include "web_test_audio.h"
#include "audio_alert.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "test_audio";

#define TEST_AUDIO_PORT       8080
#define TEST_AUDIO_CTRL_PORT  32769   /* main server keeps the default 32768 */

extern const uint8_t test_audio_html_start[] asm("_binary_test_audio_html_start");
extern const uint8_t test_audio_html_end[]   asm("_binary_test_audio_html_end");

/* ── Helpers ────────────────────────────────────────────────────────────── */
static esp_err_t send_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t send_err(httpd_req_t *req, const char *message) {
    ESP_LOGW(TAG, "Rejected: %s", message);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", message);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── Handlers ───────────────────────────────────────────────────────────── */
static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)test_audio_html_start,
                    test_audio_html_end - test_audio_html_start);
    return ESP_OK;
}

static esp_err_t clip_post(httpd_req_t *req) {
    char q[96] = {0}, name[32] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK ||
        name[0] == '\0') {
        return send_err(req, "missing name");
    }
    if (!audio_alert_test_clip(name)) {
        return send_err(req, "unknown clip");
    }
    return send_ok(req);
}

static esp_err_t alert_post(httpd_req_t *req) {
    char q[128] = {0}, tbuf[16] = {0}, ibuf[8] = {0}, vbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return send_err(req, "missing query");
    }

    if (httpd_query_key_value(q, "type", tbuf, sizeof(tbuf)) != ESP_OK) {
        return send_err(req, "missing type");
    }
    alert_type_t type;
    if      (strcmp(tbuf, "rms") == 0)    type = ALERT_RMS;
    else if (strcmp(tbuf, "hfr") == 0)    type = ALERT_HFR;
    else if (strcmp(tbuf, "safety") == 0) type = ALERT_SAFETY;
    else return send_err(req, "bad type");

    if (httpd_query_key_value(q, "instance", ibuf, sizeof(ibuf)) != ESP_OK) {
        return send_err(req, "missing instance");
    }
    int instance = atoi(ibuf);
    if (instance < 1 || instance > 3) {
        return send_err(req, "instance must be 1-3");
    }

    /* value is optional and ignored for safety. */
    float value = 0.0f;
    if (type != ALERT_SAFETY) {
        if (httpd_query_key_value(q, "value", vbuf, sizeof(vbuf)) != ESP_OK) {
            return send_err(req, "missing value");
        }
        char *end = NULL;
        value = strtof(vbuf, &end);
        if (end == vbuf) return send_err(req, "bad value");
    }

    audio_alert_test_speak(type, instance - 1, value);
    return send_ok(req);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void web_test_audio_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = TEST_AUDIO_PORT;
    config.ctrl_port        = TEST_AUDIO_CTRL_PORT;
    config.max_uri_handlers = 3;
    config.stack_size       = 6144;   /* default 4096 + margin for strtof/snprintf */
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start test server on port %d", TEST_AUDIO_PORT);
        return;
    }

    static const httpd_uri_t routes[] = {
        { "/",                 HTTP_GET,  page_get,   NULL },
        { "/api/test/clip",    HTTP_POST, clip_post,  NULL },
        { "/api/test/alert",   HTTP_POST, alert_post, NULL },
    };
    for (int i = 0; i < (int)(sizeof(routes) / sizeof(routes[0])); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "Audio test server on http://<device>:%d/", TEST_AUDIO_PORT);
}
