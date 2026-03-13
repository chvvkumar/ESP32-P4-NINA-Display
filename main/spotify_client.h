#pragma once

/**
 * @file spotify_client.h
 * @brief Spotify Web API client — playback state, controls, and album art fetching.
 *
 * Uses esp_http_client with TLS (esp_crt_bundle) for all Spotify API calls.
 * Bearer tokens come from spotify_auth module. Playback state is cached
 * behind a mutex for thread-safe access from the poll task and UI.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPOTIFY_MAX_TITLE_LEN    128
#define SPOTIFY_MAX_ARTIST_LEN   128
#define SPOTIFY_MAX_ALBUM_LEN    128
#define SPOTIFY_MAX_ART_URL_LEN  256
#define SPOTIFY_MAX_TRACK_ID_LEN 64

typedef struct {
    bool is_playing;
    bool is_active;                              // Spotify has an active device
    char track_title[SPOTIFY_MAX_TITLE_LEN];
    char artist_name[SPOTIFY_MAX_ARTIST_LEN];
    char album_name[SPOTIFY_MAX_ALBUM_LEN];
    char album_art_url[SPOTIFY_MAX_ART_URL_LEN]; // 640x640 image URL
    char track_id[SPOTIFY_MAX_TRACK_ID_LEN];     // For change detection
    int progress_ms;                             // Current playback position
    int duration_ms;                             // Track duration
    int64_t fetched_at_ms;                       // esp_timer_get_time()/1000 when fetched
} spotify_playback_t;

typedef enum {
    SPOTIFY_ACTION_PLAY,
    SPOTIFY_ACTION_PAUSE,
    SPOTIFY_ACTION_NEXT,
    SPOTIFY_ACTION_PREV,
} spotify_action_t;

/** Action queue — poll task or web handler can enqueue, poll task dequeues */
extern QueueHandle_t spotify_action_queue;

/**
 * Initialize Spotify client (creates mutex and action queue).
 * Call once at startup before any other spotify_client functions.
 */
void spotify_client_init(void);

/**
 * Fetch currently playing track from Spotify API.
 * @param out  Filled with playback data on success
 * @return ESP_OK if playing, ESP_ERR_NOT_FOUND if nothing playing (204),
 *         ESP_FAIL on API/auth error
 */
esp_err_t spotify_client_get_currently_playing(spotify_playback_t *out);

/**
 * Get a thread-safe copy of the last successfully fetched playback state.
 * @param out  Destination struct (copied under mutex)
 */
void spotify_client_get_cached_playback(spotify_playback_t *out);

/**
 * Playback controls. All return ESP_OK on 2xx response from Spotify.
 */
esp_err_t spotify_client_play(void);
esp_err_t spotify_client_pause(void);
esp_err_t spotify_client_next(void);
esp_err_t spotify_client_previous(void);

/**
 * Fetch album art JPEG from the given URL.
 * @param url       HTTPS URL of album art (from spotify_playback_t.album_art_url)
 * @param out_buf   Receives heap_caps_malloc'd JPEG buffer (caller frees with free())
 * @param out_size  Receives JPEG size in bytes
 * @return ESP_OK on success
 */
esp_err_t spotify_client_fetch_album_art(const char *url, uint8_t **out_buf,
                                          size_t *out_size);

#ifdef __cplusplus
}
#endif
