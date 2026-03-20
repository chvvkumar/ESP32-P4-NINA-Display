/**
 * @file spotify_auth.c
 * @brief Spotify OAuth PKCE token lifecycle — NVS storage of refresh/access
 *        tokens, expiry tracking, HTTPS token exchange with accounts.spotify.com.
 *
 * Thread-safe: a mutex guards all token read/write operations since the poll
 * task (Core 0) and web handlers (any core) both access token state.
 */

#include "spotify_auth.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "spotify_auth";

#define NVS_NAMESPACE           "spotify"
#define NVS_KEY_REFRESH         "refresh_tok"
#define NVS_KEY_ACCESS          "access_tok"
#define NVS_KEY_EXPIRY          "tok_expiry"
#define TOKEN_ENDPOINT          "https://accounts.spotify.com/api/token"
#define TOKEN_REFRESH_MARGIN_MS 60000   /* Refresh 60s before expiry */
#define TOKEN_RESPONSE_BUF_SIZE 4096    /* 4KB SPIRAM for token responses */
#define HTTP_TIMEOUT_MS         10000
#define POST_BODY_BUF_SIZE      2048    /* URL-encoded POST body */

static SemaphoreHandle_t s_mutex;
static char *s_access_token = NULL;
static char *s_refresh_token = NULL;
#define ACCESS_TOKEN_SIZE  1024
#define REFRESH_TOKEN_SIZE 512
static int64_t s_token_expiry_ms;       /* Monotonic ms when access token expires */
static spotify_auth_state_t s_state = SPOTIFY_AUTH_NONE;

/* Forward declarations */
static esp_err_t do_token_request(const char *body, size_t body_len);
static void save_tokens_to_nvs(void);
static void load_tokens_from_nvs(void);
static void clear_tokens_from_nvs(void);
static int64_t now_ms(void);

// =============================================================================
// Time helper
// =============================================================================

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// =============================================================================
// NVS helpers
// =============================================================================

static void load_tokens_from_nvs(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS namespace '%s' not found — no stored tokens", NVS_NAMESPACE);
        return;
    }

    size_t len;

    /* Refresh token */
    len = REFRESH_TOKEN_SIZE;
    err = nvs_get_str(nvs, NVS_KEY_REFRESH, s_refresh_token, &len);
    if (err != ESP_OK) {
        s_refresh_token[0] = '\0';
    }

    /* Access token */
    len = ACCESS_TOKEN_SIZE;
    err = nvs_get_str(nvs, NVS_KEY_ACCESS, s_access_token, &len);
    if (err != ESP_OK) {
        s_access_token[0] = '\0';
    }

    /* Expiry — stored as int64 but NVS only has i32/u64. Use blob. */
    len = sizeof(s_token_expiry_ms);
    err = nvs_get_blob(nvs, NVS_KEY_EXPIRY, &s_token_expiry_ms, &len);
    if (err != ESP_OK) {
        s_token_expiry_ms = 0;
    }

    nvs_close(nvs);

    if (s_refresh_token[0] != '\0') {
        s_state = SPOTIFY_AUTH_AUTHORIZED;
        ESP_LOGI(TAG, "Loaded Spotify tokens from NVS (refresh token present)");
    }
}

static void save_tokens_to_nvs(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(nvs, NVS_KEY_REFRESH, s_refresh_token);
    nvs_set_str(nvs, NVS_KEY_ACCESS, s_access_token);
    nvs_set_blob(nvs, NVS_KEY_EXPIRY, &s_token_expiry_ms, sizeof(s_token_expiry_ms));
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGD(TAG, "Tokens saved to NVS");
}

static void clear_tokens_from_nvs(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for erase: %s", esp_err_to_name(err));
        return;
    }

    nvs_erase_key(nvs, NVS_KEY_REFRESH);
    nvs_erase_key(nvs, NVS_KEY_ACCESS);
    nvs_erase_key(nvs, NVS_KEY_EXPIRY);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Tokens cleared from NVS");
}

// =============================================================================
// URL encoding helper
// =============================================================================

/**
 * Append a URL-encoded key=value pair to a buffer.
 * If the buffer already has content (pos > 0 and buffer[pos-1] != '\0' effectively),
 * prepend '&'.
 * Returns the new write position, or -1 on overflow.
 */
static int url_encode_param(char *buf, int pos, int buf_size,
                            const char *key, const char *value) {
    /* Simple append — Spotify token endpoint values are safe ASCII
     * (base64url codes, client IDs, redirect URIs with limited charset).
     * No full percent-encoding needed for these specific parameters. */
    int written;
    if (pos > 0) {
        written = snprintf(buf + pos, buf_size - pos, "&%s=%s", key, value);
    } else {
        written = snprintf(buf + pos, buf_size - pos, "%s=%s", key, value);
    }
    if (written < 0 || pos + written >= buf_size) {
        return -1;
    }
    return pos + written;
}

// =============================================================================
// Token request — core HTTPS POST to Spotify token endpoint
// =============================================================================

static esp_err_t do_token_request(const char *body, size_t body_len) {
    esp_http_client_config_t http_cfg = {
        .url = TOKEN_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int write_len = esp_http_client_write(client, body, (int)body_len);
    if (write_len < 0) {
        ESP_LOGE(TAG, "HTTP write failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* Determine buffer size */
    int buf_size = (content_length > 0 && content_length < TOKEN_RESPONSE_BUF_SIZE)
                       ? (content_length + 1)
                       : TOKEN_RESPONSE_BUF_SIZE;

    char *buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for token response", buf_size);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* Read response body */
    int total_read = 0;
    int max_read = buf_size - 1;
    while (total_read < max_read) {
        int read_len = esp_http_client_read(client, buffer + total_read, max_read - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    buffer[total_read] = '\0';

    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Token request failed HTTP %d: %.256s", status, buffer);
        free(buffer);
        return ESP_FAIL;
    }

    if (total_read == 0) {
        ESP_LOGE(TAG, "Empty token response body");
        free(buffer);
        return ESP_FAIL;
    }

    /* Parse JSON response */
    cJSON *json = cJSON_Parse(buffer);
    free(buffer);

    if (!json) {
        ESP_LOGE(TAG, "Failed to parse token response JSON");
        return ESP_FAIL;
    }

    esp_err_t result = ESP_FAIL;

    cJSON *access = cJSON_GetObjectItem(json, "access_token");
    cJSON *expires_in = cJSON_GetObjectItem(json, "expires_in");

    if (cJSON_IsString(access) && cJSON_IsNumber(expires_in)) {
        /* Update access token */
        snprintf(s_access_token, ACCESS_TOKEN_SIZE, "%s", access->valuestring);

        /* Compute expiry in monotonic ms */
        int expires_s = expires_in->valueint;
        s_token_expiry_ms = now_ms() + ((int64_t)expires_s * 1000);

        /* Refresh token — Spotify may or may not return a new one.
         * If present, update it; otherwise keep the existing one. */
        cJSON *refresh = cJSON_GetObjectItem(json, "refresh_token");
        if (cJSON_IsString(refresh) && refresh->valuestring[0] != '\0') {
            snprintf(s_refresh_token, REFRESH_TOKEN_SIZE, "%s", refresh->valuestring);
        }

        s_state = SPOTIFY_AUTH_AUTHORIZED;
        save_tokens_to_nvs();

        ESP_LOGI(TAG, "Token exchange OK — expires in %d s", expires_s);
        result = ESP_OK;
    } else {
        cJSON *error = cJSON_GetObjectItem(json, "error");
        const char *err_str = cJSON_IsString(error) ? error->valuestring : "unknown";
        ESP_LOGE(TAG, "Token response missing fields (error: %s)", err_str);
    }

    cJSON_Delete(json);
    return result;
}

// =============================================================================
// Public API
// =============================================================================

void spotify_auth_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    if (!s_access_token) {
        s_access_token = heap_caps_calloc(1, ACCESS_TOKEN_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (!s_refresh_token) {
        s_refresh_token = heap_caps_calloc(1, REFRESH_TOKEN_SIZE, MALLOC_CAP_SPIRAM);
    }
    configASSERT(s_access_token && s_refresh_token);

    s_access_token[0] = '\0';
    s_refresh_token[0] = '\0';
    s_token_expiry_ms = 0;
    s_state = SPOTIFY_AUTH_NONE;

    load_tokens_from_nvs();
}

spotify_auth_state_t spotify_auth_get_state(void) {
    spotify_auth_state_t state;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    state = s_state;
    xSemaphoreGive(s_mutex);
    return state;
}

esp_err_t spotify_auth_exchange_code(const char *code, const char *code_verifier,
                                      const char *redirect_uri) {
    if (!code || !code_verifier || !redirect_uri) {
        ESP_LOGE(TAG, "exchange_code: NULL parameter");
        return ESP_ERR_INVALID_ARG;
    }

    const app_config_t *cfg = app_config_get();
    if (cfg->spotify_client_id[0] == '\0') {
        ESP_LOGE(TAG, "exchange_code: no client_id configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build URL-encoded POST body */
    char *body = heap_caps_malloc(POST_BODY_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "Failed to allocate POST body buffer");
        return ESP_ERR_NO_MEM;
    }

    int pos = 0;
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "grant_type", "authorization_code");
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "code", code);
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "redirect_uri", redirect_uri);
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "client_id", cfg->spotify_client_id);
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "code_verifier", code_verifier);

    if (pos < 0) {
        ESP_LOGE(TAG, "POST body overflow");
        free(body);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = do_token_request(body, (size_t)pos);
    xSemaphoreGive(s_mutex);

    free(body);

    if (err != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = SPOTIFY_AUTH_ERROR;
        xSemaphoreGive(s_mutex);
    }

    return err;
}

esp_err_t spotify_auth_get_access_token(char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return ESP_ERR_INVALID_ARG;

    buf[0] = '\0';
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* No refresh token means not authorized */
    if (s_refresh_token[0] == '\0') {
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    /* Check if token is still valid (with margin) */
    if (s_access_token[0] != '\0' &&
        now_ms() < (s_token_expiry_ms - TOKEN_REFRESH_MARGIN_MS)) {
        snprintf(buf, buf_size, "%s", s_access_token);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    /* Need to refresh */
    ESP_LOGI(TAG, "Access token expired or missing — refreshing");

    const app_config_t *cfg = app_config_get();
    if (cfg->spotify_client_id[0] == '\0') {
        ESP_LOGE(TAG, "Cannot refresh: no client_id configured");
        s_state = SPOTIFY_AUTH_ERROR;
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    /* Build refresh POST body */
    char *body = heap_caps_malloc(POST_BODY_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "Failed to allocate refresh body buffer");
        s_state = SPOTIFY_AUTH_ERROR;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    int pos = 0;
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "grant_type", "refresh_token");
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "refresh_token", s_refresh_token);
    pos = url_encode_param(body, pos, POST_BODY_BUF_SIZE,
                           "client_id", cfg->spotify_client_id);

    if (pos < 0) {
        ESP_LOGE(TAG, "Refresh body overflow");
        free(body);
        s_state = SPOTIFY_AUTH_ERROR;
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    esp_err_t err = do_token_request(body, (size_t)pos);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Token refresh failed");
        s_state = SPOTIFY_AUTH_ERROR;
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    snprintf(buf, buf_size, "%s", s_access_token);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void spotify_auth_invalidate_token(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_access_token[0] = '\0';
    s_token_expiry_ms = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Access token invalidated — will refresh on next use");
}

void spotify_auth_logout(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_access_token[0] = '\0';
    s_refresh_token[0] = '\0';
    s_token_expiry_ms = 0;
    s_state = SPOTIFY_AUTH_NONE;

    clear_tokens_from_nvs();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Logged out — all tokens cleared");
}

bool spotify_auth_has_tokens(void) {
    bool has;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    has = (s_refresh_token[0] != '\0');
    xSemaphoreGive(s_mutex);
    return has;
}
