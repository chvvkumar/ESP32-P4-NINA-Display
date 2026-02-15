#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_websocket_client.h"

#define MAX_FILTERS 10

// Filter information
typedef struct {
    char name[32];      // Filter name (e.g., "Ha", "L", "R", "G", "B")
    int id;             // Filter position/ID
} nina_filter_t;

// NINA client data structure
typedef struct {
    bool connected;
    char status[32];
    char target_name[64];
    char profile_name[64];
    char telescope_name[64];
    
    struct {
        float temp;
        float cooler_power;
    } camera;
    
    struct {
        float rms_total;
        float rms_ra;
        float rms_dec;
    } guider;
    
    struct {
        int position;
    } focuser;
    
    struct {
        float illumination;
    } moon;
    
    // Sequence info
    int exposure_count;         // Number of completed exposures for current filter (CompletedIterations)
    int exposure_iterations;    // Total number of exposures planned for current filter (ExposureCount)
    float exposure_current;     // Elapsed time in current exposure (seconds)
    float exposure_total;       // Total duration of current exposure (seconds, from ExposureTime)
    char current_filter[32];    // Current filter name (e.g., "Ha", "Sii", "L")
    char container_name[64];    // Running container name (e.g., "LRGBSHO") - stripped of "_Container"
    char container_step[64];    // Currently running step/instruction name (e.g., "Smart Exposure", "Auto Focus")
    char time_remaining[32];    // Time remaining for entire sequence (HH:MM:SS format)
    bool is_dithering;
    
    // Image stats
    float hfr;
    int stars;
    int saturated_pixels;
    
    // Mount
    char meridian_flip[16];
    float rotator_angle;

    // Available filters from filter wheel
    nina_filter_t filters[MAX_FILTERS];
    int filter_count;

    // WebSocket state
    bool websocket_connected;
} nina_client_t;

// Polling intervals (ms)
#define NINA_POLL_SLOW_MS     30000   // Focuser, mount
#define NINA_POLL_SEQUENCE_MS 10000   // Sequence counts (expensive JSON)

// Polling state - tracks timers and cached static data between polls
typedef struct {
    // Timestamps (ms from esp_timer_get_time)
    int64_t last_slow_poll_ms;
    int64_t last_sequence_poll_ms;

    // Static data fetched once
    bool static_fetched;

    // Cached static data (survives across polls)
    char cached_profile[64];
    char cached_telescope[64];
    nina_filter_t cached_filters[MAX_FILTERS];
    int cached_filter_count;
} nina_poll_state_t;

// Initialize polling state (call once before polling loop)
void nina_poll_state_init(nina_poll_state_t *state);

// Tiered polling - fetches data at different rates based on change frequency
void nina_client_poll(const char *base_url, nina_client_t *data, nina_poll_state_t *state);

// Legacy API - fetches all data every call (kept for compatibility)
void nina_client_get_data(const char *base_url, nina_client_t *data);

// WebSocket API - connect to NINA WebSocket for event-driven updates
// Derives ws:// URL from the HTTP API base_url (e.g., http://host:1888/v2/api/ -> ws://host:1888/v2/socket)
void nina_websocket_start(const char *base_url, nina_client_t *data);
void nina_websocket_stop(void);
