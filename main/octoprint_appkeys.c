/**
 * @file octoprint_appkeys.c
 * @brief OctoPrint Application Keys authorization flow. See octoprint_appkeys.h.
 */

#include "octoprint_appkeys.h"

#include "app_config.h"
#include "http_fetch.h"
#include "tasks.h"          /* psram_task_spawn */

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "octo_appkeys";

/* How long the user gets to approve before we stop asking. OctoPrint expires a
 * pending request on its own side after a few minutes, at which point the poll
 * starts answering 404 and the flow ends as DENIED before this cap is reached.
 * The cap exists so a server that never expires anything cannot leave the
 * worker polling forever. */
#define APPKEY_TIMEOUT_MS   (5 * 60 * 1000)
#define APPKEY_POLL_MS      1000
#define APPKEY_HTTP_TIMEOUT_MS 8000

/* An app_token is a UUID today; sized with room to spare and truncation-safe. */
#define APPKEY_TOKEN_LEN    96
/* OctoPrint API keys are 32 hex characters; the config field holds 63 + NUL. */
#define APPKEY_KEY_LEN      sizeof(((app_config_t *)0)->octoprint_api_key)
#define APPKEY_BASE_LEN     sizeof(((app_config_t *)0)->octoprint_url)

/* ── Published state ──────────────────────────────────────────────────────────
 * Written by the worker, read by httpd workers on the other core. A spinlock,
 * not a mutex: the guarded region is a bounded copy of a few hundred bytes, and
 * a spinlock needs no init call, so there is no "who calls _init()" question for
 * a module that is only reachable from two web handlers. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static octoprint_appkey_state_t s_state = OCTO_APPKEY_IDLE;
static char s_dialog[OCTO_APPKEY_DIALOG_LEN];
static char s_detail[OCTO_APPKEY_DETAIL_LEN];
static bool s_busy;

/* Written only from octoprint_appkeys_start(), which runs on the single httpd
 * worker task, so it needs no lock of its own. */
static TaskHandle_t s_task;

/* Keep-alive slot for the flow's requests -- above all the 1 Hz decision poll,
 * which would otherwise open a fresh connection every second for up to five
 * minutes. Touched only by appkeys_task (the sole caller of appkey_http),
 * while s_busy is held: created lazily, destroyed when the flow ends. */
static http_fetch_conn_t *s_conn;

static void set_state(octoprint_appkey_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_mux);
    s_state = state;
    if (detail) {
        strlcpy(s_detail, detail, sizeof(s_detail));
    } else {
        s_detail[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
}

static void set_dialog(const char *url)
{
    portENTER_CRITICAL(&s_mux);
    if (url) {
        strlcpy(s_dialog, url, sizeof(s_dialog));
    } else {
        s_dialog[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
}

static void clear_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    s_busy = false;
    portEXIT_CRITICAL(&s_mux);
}

const char *octoprint_appkeys_state_name(octoprint_appkey_state_t state)
{
    switch (state) {
        case OCTO_APPKEY_PROBING:     return "probing";
        case OCTO_APPKEY_WAITING:     return "waiting";
        case OCTO_APPKEY_GRANTED:     return "granted";
        case OCTO_APPKEY_DENIED:      return "denied";
        case OCTO_APPKEY_UNSUPPORTED: return "unsupported";
        case OCTO_APPKEY_TIMEOUT:     return "timeout";
        case OCTO_APPKEY_ERROR:       return "error";
        case OCTO_APPKEY_IDLE:
        default:                      return "idle";
    }
}

void octoprint_appkeys_snapshot(octoprint_appkey_state_t *state,
                                char *auth_dialog, size_t dialog_len,
                                char *detail, size_t detail_len)
{
    portENTER_CRITICAL(&s_mux);
    if (state) *state = s_state;
    if (auth_dialog && dialog_len > 0) strlcpy(auth_dialog, s_dialog, dialog_len);
    if (detail && detail_len > 0) strlcpy(detail, s_detail, detail_len);
    portEXIT_CRITICAL(&s_mux);
}

/* ── HTTP ─────────────────────────────────────────────────────────────────── */

/**
 * One small request through the shared http_fetch spine.
 *
 * Returns the HTTP status code, or 0 when no response arrived at all
 * (DNS/connect/timeout) -- every decision in this flow is a status-code
 * decision, including the non-2xx ones (204, 404), which http_fetch reports
 * through status_out on the failure return as well.
 *
 * @param post_body NULL for a GET; a JSON string switches the request to POST.
 * @param out_body  optional; on a 2xx receives the PSRAM body the caller must
 *                  heap_caps_free(). Set to NULL on every non-2xx.
 */
static int appkey_http(const char *url, bool tls, const char *post_body,
                       char **out_body)
{
    int status = 0;
    char *body = NULL;
    size_t len = 0;
    if (s_conn == NULL) {
        s_conn = http_fetch_conn_create();   /* NULL is fine: one-shot per request */
    }
    http_fetch_opts_t opts = {
        .timeout_ms = APPKEY_HTTP_TIMEOUT_MS,
        .use_tls_bundle = tls,
        /* Same as octoprint_client: an OctoPrint behind a reverse proxy commonly
         * answers a GET with a redirect (http->https, or a path prefix), and a
         * probe that reported one as "unsupported" would kill the flow for a
         * perfectly capable server. http_fetch never follows a redirect for a
         * POST, so this is 0 there in any case. */
        .max_redirects = post_body ? 0 : 3,
        .max_response_bytes = 1024,
        .accept = "application/json",
        .post_body = post_body,
        .conn = s_conn,
        .status_out = &status,
    };
    /* http_fetch_text leaves *out_body untouched on failure, so body must stay
     * NULL unless the call actually succeeded. */
    if (http_fetch_text(url, &opts, &body, &len) != ESP_OK) {
        body = NULL;
    }
    if (out_body) {
        *out_body = body;
    } else if (body) {
        heap_caps_free(body);
    }
    return status;
}

/* ── Flow ─────────────────────────────────────────────────────────────────── */

/** Persist a granted key. Returns false (and reports nothing) on failure. */
static bool save_api_key(const char *key)
{
    if (key == NULL || key[0] == '\0' || strlen(key) >= APPKEY_KEY_LEN) {
        ESP_LOGE(TAG, "Granted key is empty or too long for the config field");
        return false;
    }
    /* Config snapshot pattern: never field-write the live config, and never put
     * an ~8.6 KB app_config_t on a stack. */
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        ESP_LOGE(TAG, "PSRAM alloc failed for config snapshot");
        return false;
    }
    app_config_get_snapshot_into(cfg);
    strlcpy(cfg->octoprint_api_key, key, sizeof(cfg->octoprint_api_key));
    app_config_save(cfg);
    heap_caps_free(cfg);
    return true;
}

/**
 * Place the authorization request and read back the token + approval URL.
 * Returns true on a 2xx carrying an app_token.
 */
static bool place_request(const char *base, bool tls, const char *app_name,
                          char *token, size_t token_len)
{
    /* Every failure return below publishes a state: appkeys_flow() stops without
     * publishing anything of its own when this returns false, so a silent return
     * would leave the flow stuck in PROBING with no worker behind it. */
    static const char *OOM = "The device is out of memory. Try again.";

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        set_state(OCTO_APPKEY_ERROR, OOM);
        return false;
    }
    /* Built with cJSON rather than snprintf so a hostname containing a quote or
     * a backslash cannot break out of the JSON string. */
    if (cJSON_AddStringToObject(root, "app", app_name) == NULL) {
        cJSON_Delete(root);
        set_state(OCTO_APPKEY_ERROR, OOM);
        return false;
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        set_state(OCTO_APPKEY_ERROR, OOM);
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/plugin/appkeys/request", base);

    char *body = NULL;
    int status = appkey_http(url, tls, payload, &body);
    cJSON_free(payload);

    if (status < 200 || status >= 300 || body == NULL) {
        ESP_LOGW(TAG, "Authorization request refused (HTTP %d)", status);
        if (body) heap_caps_free(body);
        set_state(OCTO_APPKEY_ERROR,
                  "OctoPrint refused the access request. Enter a key manually.");
        return false;
    }

    cJSON *resp = cJSON_Parse(body);
    heap_caps_free(body);
    if (resp == NULL) {
        set_state(OCTO_APPKEY_ERROR, "OctoPrint sent a reply we could not read.");
        return false;
    }

    bool ok = false;
    cJSON *tok = cJSON_GetObjectItem(resp, "app_token");
    if (cJSON_IsString(tok) && tok->valuestring[0] != '\0') {
        strlcpy(token, tok->valuestring, token_len);
        ok = true;

        /* auth_dialog is 1.8.0+ only; absent on older servers, where the user
         * approves from OctoPrint's own interface instead. A server that sends
         * a site-relative path gets the base prefixed back on. */
        cJSON *dlg = cJSON_GetObjectItem(resp, "auth_dialog");
        if (cJSON_IsString(dlg) && dlg->valuestring[0] != '\0') {
            if (dlg->valuestring[0] == '/') {
                char full[OCTO_APPKEY_DIALOG_LEN];
                int n = snprintf(full, sizeof(full), "%s%s", base, dlg->valuestring);
                /* Truncated means a wrong address, and the UI turns whatever is
                 * here into a link the user clicks. Offer nothing instead; the
                 * user can still approve from OctoPrint's own interface. */
                set_dialog((n > 0 && n < (int)sizeof(full)) ? full : NULL);
            } else {
                set_dialog(dlg->valuestring);
            }
        }
    } else {
        set_state(OCTO_APPKEY_ERROR, "OctoPrint did not return a request token.");
    }

    cJSON_Delete(resp);
    return ok;
}

/**
 * Poll for the decision until one arrives or the cap expires. Publishes the
 * terminal state itself.
 */
static void await_decision(const char *base, bool tls, const char *token)
{
    char url[256];
    int n = snprintf(url, sizeof(url), "%s/plugin/appkeys/request/%s", base, token);
    if (n < 0 || n >= (int)sizeof(url)) {
        set_state(OCTO_APPKEY_ERROR, "The OctoPrint address is too long.");
        return;
    }

    const int64_t deadline_us = esp_timer_get_time() + (int64_t)APPKEY_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(APPKEY_POLL_MS));

        char *body = NULL;
        int status = appkey_http(url, tls, NULL, &body);

        if (status == 200 && body != NULL) {
            cJSON *resp = cJSON_Parse(body);
            cJSON *key = resp ? cJSON_GetObjectItem(resp, "api_key") : NULL;
            bool saved = cJSON_IsString(key) && save_api_key(key->valuestring);
            if (resp) cJSON_Delete(resp);
            heap_caps_free(body);

            if (saved) {
                ESP_LOGI(TAG, "Access granted; API key saved");
                set_state(OCTO_APPKEY_GRANTED, "Access granted. The key is saved.");
            } else {
                set_state(OCTO_APPKEY_ERROR,
                          "Access was granted but the key could not be saved.");
            }
            return;
        }

        if (body) heap_caps_free(body);

        if (status == 404) {
            /* OctoPrint answers 404 both for a declined request and for one it
             * has already expired; the two are not distinguishable here. */
            ESP_LOGI(TAG, "Access request declined or expired");
            set_state(OCTO_APPKEY_DENIED,
                      "The request was declined or expired in OctoPrint.");
            return;
        }
        /* 202 is "still pending". Anything else (a transport hiccup, a restart
         * of OctoPrint) is treated the same way: keep waiting until the cap. */
    }

    ESP_LOGW(TAG, "No decision within %d s", APPKEY_TIMEOUT_MS / 1000);
    set_state(OCTO_APPKEY_TIMEOUT, "Nobody approved the request in time.");
}

/** Probe, request, then wait for the decision. Always leaves a terminal state. */
static void appkeys_flow(void)
{
    char base[APPKEY_BASE_LEN];
    char app_name[64];
    const app_config_t *cfg = app_config_get();
    strlcpy(base, cfg->octoprint_url, sizeof(base));
    /* The app name is what OctoPrint shows the user in the approval prompt, so
     * it names this device, not the firmware. */
    snprintf(app_name, sizeof(app_name), "NINA Display (%s)", cfg->hostname);

    /* A trailing slash would double up in every URL below. */
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') {
        base[--len] = '\0';
    }
    if (len == 0) {
        set_state(OCTO_APPKEY_ERROR, "Enter the OctoPrint address first.");
        return;
    }
    const bool tls = (strncmp(base, "https://", 8) == 0);

    char url[256];
    snprintf(url, sizeof(url), "%s/plugin/appkeys/probe", base);
    int status = appkey_http(url, tls, NULL, NULL);
    if (status != 204) {
        ESP_LOGW(TAG, "Probe returned %d; workflow unavailable", status);
        set_state(OCTO_APPKEY_UNSUPPORTED,
                  status == 0 ? "Could not reach OctoPrint at that address."
                              : "This OctoPrint cannot hand out keys this way.");
        return;
    }

    char token[APPKEY_TOKEN_LEN];
    if (!place_request(base, tls, app_name, token, sizeof(token))) {
        return;   /* place_request published the reason */
    }

    ESP_LOGI(TAG, "Access request placed; waiting for approval");
    set_state(OCTO_APPKEY_WAITING, "Waiting for you to approve it in OctoPrint.");
    await_decision(base, tls, token);
}

/**
 * Worker. Parks on a notification between flows instead of deleting itself: its
 * stack comes from psram_task_spawn(), which never frees, and a task cannot
 * free its own stack on the way out. Reusing one parked task also removes the
 * window where a self-deleting task is still running after clearing s_busy.
 */
static void appkeys_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        appkeys_flow();
        /* Flow over (whatever the terminal state): release the keep-alive slot
         * before clearing s_busy, so the next flow cannot race it. */
        if (s_conn) {
            http_fetch_conn_destroy(s_conn);
            s_conn = NULL;
        }
        clear_busy();
    }
}

/**
 * Persist @p url as octoprint_url when it differs from the saved value. Same
 * snapshot pattern as save_api_key(). Skips the ~350 ms NVS write when equal.
 * Returns false only on allocation failure.
 */
static bool save_url_if_changed(const char *url)
{
    if (strcmp(app_config_get()->octoprint_url, url) == 0) {
        return true;
    }
    app_config_t *cfg = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) {
        ESP_LOGE(TAG, "PSRAM alloc failed for config snapshot");
        return false;
    }
    app_config_get_snapshot_into(cfg);
    strlcpy(cfg->octoprint_url, url, sizeof(cfg->octoprint_url));
    app_config_save(cfg);
    heap_caps_free(cfg);
    ESP_LOGI(TAG, "OctoPrint address updated from the access request");
    return true;
}

esp_err_t octoprint_appkeys_start(const char *url)
{
    /* One flow at a time: a second request while one is live would leave two
     * pollers racing to write the same config field. */
    portENTER_CRITICAL(&s_mux);
    bool busy = s_busy;
    if (!busy) s_busy = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    set_dialog(NULL);

    /* The worker reads the address from config, so a caller-supplied one must
     * be on disk before the worker is woken. Done under s_busy so a concurrent
     * start cannot interleave its own save with this one. */
    if (url && url[0] != '\0' && !save_url_if_changed(url)) {
        set_state(OCTO_APPKEY_ERROR, "The device is out of memory. Try again.");
        clear_busy();
        return ESP_ERR_NO_MEM;
    }

    if (app_config_get()->octoprint_url[0] == '\0') {
        set_state(OCTO_APPKEY_ERROR, "Enter the OctoPrint address first.");
        clear_busy();
        return ESP_OK;
    }

    set_state(OCTO_APPKEY_PROBING, "Contacting OctoPrint.");

    /* Created on first use, then reused: the button may never be pressed, and a
     * 10 KB stack should not be standing by for that. */
    if (s_task == NULL) {
        s_task = psram_task_spawn(appkeys_task, "octo_akey", 10240, NULL, 3, 0);
        if (s_task == NULL) {
            set_state(OCTO_APPKEY_ERROR, "The device is out of memory. Try again.");
            clear_busy();
            return ESP_ERR_NO_MEM;
        }
    }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}
