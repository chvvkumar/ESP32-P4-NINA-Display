/**
 * @file spotify_client.c
 * @brief Spotify Web API client — playback state, controls, and album art fetching.
 *
 * All HTTP calls use esp_http_client with esp_crt_bundle for TLS verification.
 * Response buffers are allocated from SPIRAM. Cached playback state is
 * mutex-protected for safe access from multiple FreeRTOS tasks.
 */

#include "spotify_client.h"
#include "spotify_auth.h"
#include "http_fetch.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "spotify_client";

/* Response buffer for currently-playing endpoint (JSON can be large) */
#define SPOTIFY_RESPONSE_BUF_SIZE   16384

/* Response buffer cap for album art JPEG downloads */
#define SPOTIFY_ART_MAX_SIZE        (512 * 1024)   /* 512 KB max */

/* HTTP timeout for Spotify API requests */
#define SPOTIFY_HTTP_TIMEOUT_MS     10000

/* Spotify API base URL */
#define SPOTIFY_API_BASE            "https://api.spotify.com/v1/me/player"

// =============================================================================
// Internal State
// =============================================================================

static SemaphoreHandle_t s_mutex;
static spotify_playback_t s_cached_playback;

QueueHandle_t spotify_action_queue;

/* Auth header buffer — allocated once from PSRAM to avoid 2.1KB stack usage
 * per API call. Safe: all Spotify API calls run on the single poll task. */
static char *s_auth_header_buf = NULL;
#define AUTH_HEADER_BUF_SIZE 1100

// =============================================================================
// Helpers
// =============================================================================

/**
 * Set the Authorization header on an existing HTTP client handle.
 * Returns ESP_OK if a valid token was obtained, ESP_FAIL otherwise.
 */
static esp_err_t set_auth_header(esp_http_client_handle_t client)
{
    /* Lazy-allocate PSRAM buffer on first use */
    if (!s_auth_header_buf) {
        s_auth_header_buf = heap_caps_malloc(AUTH_HEADER_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_auth_header_buf) {
            ESP_LOGE(TAG, "Failed to allocate auth header buffer from PSRAM");
            return ESP_FAIL;
        }
    }

    /* Fetch token into the tail of the buffer, then prepend "Bearer " */
    char *token_start = s_auth_header_buf + 7;  /* room for "Bearer " */
    size_t token_max = AUTH_HEADER_BUF_SIZE - 7;
    if (spotify_auth_get_access_token(token_start, token_max) != ESP_OK) {
        ESP_LOGW(TAG, "No valid access token available");
        return ESP_FAIL;
    }

    /* Prepend "Bearer " in-place */
    memcpy(s_auth_header_buf, "Bearer ", 7);
    esp_http_client_set_header(client, "Authorization", s_auth_header_buf);
    return ESP_OK;
}

/**
 * Send an HTTP request with an empty body and Bearer auth.
 * Used for playback control endpoints (play, pause, next, previous).
 *
 * @param url     Full API URL
 * @param method  HTTP_METHOD_PUT or HTTP_METHOD_POST
 * @return ESP_OK on 2xx response, ESP_FAIL otherwise
 */
static esp_err_t send_control_request(const char *url, esp_http_client_method_t method)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = method,
        .timeout_ms = SPOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for %s", url);
        return ESP_FAIL;
    }

    if (set_auth_header(client) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Set content length to 0 — empty body */
    esp_http_client_set_header(client, "Content-Length", "0");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed for %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        ESP_LOGD(TAG, "Control request OK: %s → %d", url, status);
        return ESP_OK;
    }

    if (status == 401) {
        ESP_LOGW(TAG, "HTTP 401 for %s — invalidating token", url);
        spotify_auth_invalidate_token();
    } else {
        ESP_LOGW(TAG, "Control request failed: %s → HTTP %d", url, status);
    }
    return ESP_FAIL;
}

// =============================================================================
// Public API — Init
// =============================================================================

/* Guards s_player_conn and s_player_shutdown. Held for the full lifetime of
 * any player/album-art HTTP request so spotify_client_prepare_shutdown can
 * guarantee no request is in flight before it destroys the handle. */
static SemaphoreHandle_t s_player_mutex;
static bool s_player_shutdown = false;

void spotify_client_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_player_mutex = xSemaphoreCreateMutex();
    memset(&s_cached_playback, 0, sizeof(s_cached_playback));
    spotify_action_queue = xQueueCreate(4, sizeof(spotify_action_t));
    ESP_LOGI(TAG, "Spotify client initialized");
}

// =============================================================================
// Public API — Get Currently Playing (persistent keep-alive connection)
// =============================================================================

/* Persistent keep-alive slot for the currently-playing endpoint. http_fetch's
 * conn owns the underlying esp_http_client handle and its own stale/dead
 * keep-alive reconnect-once logic (mirrors the hand-rolled retry this used
 * to do here directly). */
static http_fetch_conn_t *s_player_conn = NULL;

/* PSRAM buffer for the raw access token passed to http_fetch's bearer_token
 * option (http_fetch prepends "Bearer " itself, so no prefix room needed
 * here, unlike s_auth_header_buf above). Lazy-allocated on first use. */
static char *s_player_token_buf = NULL;
#define SPOTIFY_TOKEN_BUF_SIZE AUTH_HEADER_BUF_SIZE

/* Caller must hold s_player_mutex. */
static void player_conn_destroy(void)
{
    if (s_player_conn) {
        http_fetch_conn_destroy(s_player_conn);
        s_player_conn = NULL;
    }
}

esp_err_t spotify_client_get_currently_playing(spotify_playback_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_player_mutex) return ESP_FAIL;

    /* Hold the player mutex for the whole request so prepare_shutdown cannot
     * destroy the handle mid-flight. This poll is the resumption point after a
     * shutdown, so clear the flag before (re)creating the handle. */
    if (xSemaphoreTake(s_player_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    s_player_shutdown = false;

    esp_err_t ret = ESP_FAIL;

    if (!s_player_conn) {
        s_player_conn = http_fetch_conn_create();
        if (!s_player_conn) {
            ESP_LOGE(TAG, "Failed to create http_fetch conn for currently-playing");
            ret = ESP_FAIL;
            goto unlock;
        }
    }

    if (!s_player_token_buf) {
        s_player_token_buf = heap_caps_malloc(SPOTIFY_TOKEN_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_player_token_buf) {
            ESP_LOGE(TAG, "Failed to allocate token buffer from PSRAM");
            ret = ESP_FAIL;
            goto unlock;
        }
    }
    if (spotify_auth_get_access_token(s_player_token_buf, SPOTIFY_TOKEN_BUF_SIZE) != ESP_OK) {
        ESP_LOGW(TAG, "No valid access token available");
        ret = ESP_FAIL;
        goto unlock;
    }

    int status = 0;
    char *body = NULL;
    size_t body_len = 0;
    http_fetch_opts_t opts = {
        .use_tls_bundle = true,
        .timeout_ms = SPOTIFY_HTTP_TIMEOUT_MS,
        .max_attempts = 1,        /* conn's internal stale-reconnect covers the
                                    * hand-rolled retry this call used to do */
        .max_response_bytes = SPOTIFY_RESPONSE_BUF_SIZE,
        .bearer_token = s_player_token_buf,
        .conn = s_player_conn,
        .status_out = &status,
    };

    esp_err_t err = http_fetch_text(SPOTIFY_API_BASE "/currently-playing", &opts,
                                     &body, &body_len);

    /* 204 = nothing currently playing. Any 2xx (including 204) is success to
     * http_fetch, so this arrives as err == ESP_OK with an empty body; keep
     * the connection alive, same as before. */
    if (status == 204) {
        ESP_LOGD(TAG, "No active playback (204)");
        if (body) heap_caps_free(body);

        /* Update cached state to reflect nothing playing */
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_cached_playback.is_playing = false;
            s_cached_playback.is_active = false;
            xSemaphoreGive(s_mutex);
        }

        memset(out, 0, sizeof(*out));
        ret = ESP_ERR_NOT_FOUND;
        goto unlock;
    }

    if (status == 401) {
        ESP_LOGW(TAG, "HTTP 401 from currently-playing — invalidating token");
        spotify_auth_invalidate_token();
        if (body) heap_caps_free(body);
        player_conn_destroy();
        ret = ESP_FAIL;
        goto unlock;
    }

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "currently-playing failed: %s (HTTP %d)",
                 esp_err_to_name(err), status);
        if (body) heap_caps_free(body);
        player_conn_destroy();
        ret = ESP_FAIL;
        goto unlock;
    }

    if (body_len == 0) {
        ESP_LOGW(TAG, "Empty response body from currently-playing");
        if (body) heap_caps_free(body);
        ret = ESP_FAIL;
        goto unlock;
    }

    /* Parse JSON */
    cJSON *json = cJSON_Parse(body);
    heap_caps_free(body);

    if (!json) {
        /* Likely a truncated/partial body on a reused connection — destroy so
         * the next poll starts on a clean connection. */
        ESP_LOGW(TAG, "Failed to parse currently-playing JSON");
        player_conn_destroy();
        ret = ESP_FAIL;
        goto unlock;
    }

    /* Extract fields */
    spotify_playback_t pb;
    memset(&pb, 0, sizeof(pb));

    cJSON *is_playing = cJSON_GetObjectItem(json, "is_playing");
    pb.is_playing = cJSON_IsTrue(is_playing);
    pb.is_active = true;

    cJSON *progress = cJSON_GetObjectItem(json, "progress_ms");
    if (cJSON_IsNumber(progress)) {
        pb.progress_ms = (int)progress->valuedouble;
    }

    cJSON *item = cJSON_GetObjectItem(json, "item");
    if (item && cJSON_IsObject(item)) {
        /* Track title */
        cJSON *name = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            snprintf(pb.track_title, sizeof(pb.track_title), "%s", name->valuestring);
        }

        /* Track ID */
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(id) && id->valuestring) {
            snprintf(pb.track_id, sizeof(pb.track_id), "%s", id->valuestring);
        }

        /* Duration */
        cJSON *duration = cJSON_GetObjectItem(item, "duration_ms");
        if (cJSON_IsNumber(duration)) {
            pb.duration_ms = (int)duration->valuedouble;
        }

        /* First artist name */
        cJSON *artists = cJSON_GetObjectItem(item, "artists");
        if (cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
            cJSON *first_artist = cJSON_GetArrayItem(artists, 0);
            cJSON *artist_name = cJSON_GetObjectItem(first_artist, "name");
            if (cJSON_IsString(artist_name) && artist_name->valuestring) {
                snprintf(pb.artist_name, sizeof(pb.artist_name), "%s", artist_name->valuestring);
            }
        }

        /* Album name and art URL */
        cJSON *album = cJSON_GetObjectItem(item, "album");
        if (album && cJSON_IsObject(album)) {
            cJSON *album_name = cJSON_GetObjectItem(album, "name");
            if (cJSON_IsString(album_name) && album_name->valuestring) {
                snprintf(pb.album_name, sizeof(pb.album_name), "%s", album_name->valuestring);
            }

            /* First image (largest, typically 640x640) */
            cJSON *images = cJSON_GetObjectItem(album, "images");
            if (cJSON_IsArray(images) && cJSON_GetArraySize(images) > 0) {
                cJSON *first_image = cJSON_GetArrayItem(images, 0);
                cJSON *img_url = cJSON_GetObjectItem(first_image, "url");
                if (cJSON_IsString(img_url) && img_url->valuestring) {
                    snprintf(pb.album_art_url, sizeof(pb.album_art_url), "%s", img_url->valuestring);
                }
            }
        }
    }

    cJSON_Delete(json);

    pb.fetched_at_ms = esp_timer_get_time() / 1000;

    /* Update cached state under mutex */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(&s_cached_playback, &pb, sizeof(pb));
        xSemaphoreGive(s_mutex);
    }

    memcpy(out, &pb, sizeof(pb));

    ESP_LOGD(TAG, "Now playing: \"%s\" by %s (%s)", pb.track_title, pb.artist_name,
             pb.is_playing ? "playing" : "paused");

    ret = ESP_OK;

unlock:
    xSemaphoreGive(s_player_mutex);
    return ret;
}

// =============================================================================
// Public API — Cached Playback State
// =============================================================================

void spotify_client_get_cached_playback(spotify_playback_t *out)
{
    if (!out) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(out, &s_cached_playback, sizeof(s_cached_playback));
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

// =============================================================================
// Public API — Playback Controls
// =============================================================================

esp_err_t spotify_client_play(void)
{
    return send_control_request(SPOTIFY_API_BASE "/play", HTTP_METHOD_PUT);
}

esp_err_t spotify_client_pause(void)
{
    return send_control_request(SPOTIFY_API_BASE "/pause", HTTP_METHOD_PUT);
}

esp_err_t spotify_client_next(void)
{
    return send_control_request(SPOTIFY_API_BASE "/next", HTTP_METHOD_POST);
}

esp_err_t spotify_client_previous(void)
{
    return send_control_request(SPOTIFY_API_BASE "/previous", HTTP_METHOD_POST);
}

// =============================================================================
// Public API — Album Art Fetch
// =============================================================================

esp_err_t spotify_client_fetch_album_art(const char *url, uint8_t **out_buf,
                                          size_t *out_size)
{
    if (!url || !out_buf || !out_size) return ESP_ERR_INVALID_ARG;

    *out_buf = NULL;
    *out_size = 0;

    if (!s_player_mutex) return ESP_FAIL;

    /* Hold the player mutex so prepare_shutdown waits for this request too. */
    if (xSemaphoreTake(s_player_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (s_player_shutdown) {
        xSemaphoreGive(s_player_mutex);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = SPOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for album art");
        xSemaphoreGive(s_player_mutex);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed for album art: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_player_mutex);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGW(TAG, "Album art HTTP %d for %s", status, url);
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_player_mutex);
        return ESP_FAIL;
    }

    /* Determine buffer size */
    size_t buf_size;
    if (content_length > 0) {
        if ((size_t)content_length > SPOTIFY_ART_MAX_SIZE) {
            ESP_LOGW(TAG, "Album art too large: %d bytes", content_length);
            esp_http_client_cleanup(client);
            xSemaphoreGive(s_player_mutex);
            return ESP_FAIL;
        }
        buf_size = (size_t)content_length;
    } else {
        /* No content-length header — use max and reallocate later */
        buf_size = SPOTIFY_ART_MAX_SIZE;
    }

    uint8_t *buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for album art", buf_size);
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_player_mutex);
        return ESP_FAIL;
    }

    /* Stream response into buffer */
    bool had_content_length = (content_length > 0);
    size_t total_read = 0;
    int read_len;
    while (total_read < buf_size) {
        read_len = esp_http_client_read(client, (char *)buffer + total_read,
                                         buf_size - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }

    /* INTEG-7: With no Content-Length the buffer is sized to the cap. If it
     * filled exactly, the image may be larger than the cap and silently
     * truncated. Probe one more byte: if more data is available the source
     * exceeded SPOTIFY_ART_MAX_SIZE — reject rather than decode a partial JPEG. */
    if (!had_content_length && total_read == buf_size) {
        char probe;
        int extra = esp_http_client_read(client, &probe, 1);
        if (extra > 0) {
            ESP_LOGW(TAG, "Album art exceeds %d byte cap (no content-length) — rejecting",
                     (int)SPOTIFY_ART_MAX_SIZE);
            esp_http_client_cleanup(client);
            free(buffer);
            xSemaphoreGive(s_player_mutex);
            return ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGW(TAG, "Empty album art response");
        free(buffer);
        xSemaphoreGive(s_player_mutex);
        return ESP_FAIL;
    }

    /* If we over-allocated (no content-length), shrink to actual size */
    if (total_read < buf_size) {
        uint8_t *shrunk = heap_caps_realloc(buffer, total_read, MALLOC_CAP_SPIRAM);
        if (shrunk) {
            buffer = shrunk;
        }
        /* If realloc fails, keep the original oversized buffer — still valid */
    }

    *out_buf = buffer;
    *out_size = total_read;
    ret = ESP_OK;

    ESP_LOGD(TAG, "Album art fetched: %zu bytes from %s", total_read, url);
    xSemaphoreGive(s_player_mutex);
    return ret;
}

// =============================================================================
// Public API — Connection Cleanup
// =============================================================================

void spotify_client_destroy_connection(void)
{
    if (s_player_mutex && xSemaphoreTake(s_player_mutex, portMAX_DELAY) == pdTRUE) {
        player_conn_destroy();
        xSemaphoreGive(s_player_mutex);
    } else {
        player_conn_destroy();
    }
}

void spotify_client_prepare_shutdown(void)
{
    if (!s_player_mutex) {
        return;
    }

    /* Bounded wait: the in-flight request is capped by SPOTIFY_HTTP_TIMEOUT_MS,
     * so 15s is long enough for it to finish. */
    if (xSemaphoreTake(s_player_mutex, pdMS_TO_TICKS(15000)) == pdTRUE) {
        s_player_shutdown = true;
        player_conn_destroy();
        xSemaphoreGive(s_player_mutex);
    } else {
        /* Contract requires the handle destroyed on return; force it. */
        ESP_LOGW(TAG, "prepare_shutdown: mutex wait timed out — forcing teardown");
        s_player_shutdown = true;
        player_conn_destroy();
    }
}
