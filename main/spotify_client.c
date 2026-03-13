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

// =============================================================================
// Helpers
// =============================================================================

/**
 * Set the Authorization header on an existing HTTP client handle.
 * Returns ESP_OK if a valid token was obtained, ESP_FAIL otherwise.
 */
static esp_err_t set_auth_header(esp_http_client_handle_t client)
{
    char token[1024];
    if (spotify_auth_get_access_token(token, sizeof(token)) != ESP_OK) {
        ESP_LOGW(TAG, "No valid access token available");
        return ESP_FAIL;
    }

    /* Build "Bearer <token>" header value — stack-local for thread safety.
     * esp_http_client_set_header duplicates the string internally. */
    char auth_header[1100];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth_header);
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

void spotify_client_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_cached_playback, 0, sizeof(s_cached_playback));
    spotify_action_queue = xQueueCreate(4, sizeof(spotify_action_t));
    ESP_LOGI(TAG, "Spotify client initialized");
}

// =============================================================================
// Public API — Get Currently Playing
// =============================================================================

esp_err_t spotify_client_get_currently_playing(spotify_playback_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    const char *url = SPOTIFY_API_BASE "/currently-playing";

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = SPOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for currently-playing");
        return ESP_FAIL;
    }

    if (set_auth_header(client) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* 204 = nothing currently playing */
    if (status == 204) {
        ESP_LOGD(TAG, "No active playback (204)");
        esp_http_client_cleanup(client);

        /* Update cached state to reflect nothing playing */
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_cached_playback.is_playing = false;
            s_cached_playback.is_active = false;
            xSemaphoreGive(s_mutex);
        }

        memset(out, 0, sizeof(*out));
        return ESP_ERR_NOT_FOUND;
    }

    if (status == 401) {
        ESP_LOGW(TAG, "HTTP 401 from currently-playing — invalidating token");
        spotify_auth_invalidate_token();
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (status != 200) {
        ESP_LOGW(TAG, "Unexpected HTTP %d from currently-playing", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Allocate response buffer from SPIRAM */
    int buf_size = (content_length > 0) ? (content_length + 1) : SPOTIFY_RESPONSE_BUF_SIZE;
    if (buf_size > SPOTIFY_RESPONSE_BUF_SIZE) {
        buf_size = SPOTIFY_RESPONSE_BUF_SIZE;
    }

    char *buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for response", buf_size);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Read response body */
    int total_read = 0, read_len;
    int max_read = buf_size - 1;
    while (total_read < max_read) {
        read_len = esp_http_client_read(client, buffer + total_read, max_read - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }
    buffer[total_read] = '\0';

    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGW(TAG, "Empty response body from currently-playing");
        free(buffer);
        return ESP_FAIL;
    }

    /* Parse JSON */
    cJSON *json = cJSON_Parse(buffer);
    free(buffer);

    if (!json) {
        ESP_LOGW(TAG, "Failed to parse currently-playing JSON");
        return ESP_FAIL;
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

    return ESP_OK;
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

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = SPOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for album art");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed for album art: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGW(TAG, "Album art HTTP %d for %s", status, url);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Determine buffer size */
    size_t buf_size;
    if (content_length > 0) {
        if ((size_t)content_length > SPOTIFY_ART_MAX_SIZE) {
            ESP_LOGW(TAG, "Album art too large: %d bytes", content_length);
            esp_http_client_cleanup(client);
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
        return ESP_FAIL;
    }

    /* Stream response into buffer */
    size_t total_read = 0;
    int read_len;
    while (total_read < buf_size) {
        read_len = esp_http_client_read(client, (char *)buffer + total_read,
                                         buf_size - total_read);
        if (read_len <= 0) break;
        total_read += read_len;
    }

    esp_http_client_cleanup(client);

    if (total_read == 0) {
        ESP_LOGW(TAG, "Empty album art response");
        free(buffer);
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

    ESP_LOGD(TAG, "Album art fetched: %zu bytes from %s", total_read, url);
    return ESP_OK;
}
