#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui/info_overlay_types.h"

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
    int64_t exposure_end_epoch; // Absolute end time (Unix epoch seconds) for client-side interpolation
    char current_filter[32];    // Current filter name (e.g., "Ha", "Sii", "L")
    char container_name[64];    // Running container name (e.g., "LRGBSHO") - stripped of "_Container"
    char container_step[64];    // Currently running step/instruction name (e.g., "Smart Exposure", "Auto Focus")
    char time_remaining[32];    // Time remaining for entire sequence (HH:MM:SS format)
    bool is_dithering;
    
    // Image stats
    float hfr;
    int stars;
    char target_time_remaining[16]; // Target remaining time "HH:MM" from Loop Until Time_Condition
    
    // Mount
    char meridian_flip[16];
    float rotator_angle;

    // Power/Switch info (from /equipment/switch/info)
    struct {
        float input_voltage;
        float total_amps;
        float total_watts;
        char  amps_name[32];       // API name for current (e.g., "Total Current", "Amp")
        char  watts_name[32];      // API name for power (e.g., "Total Power", "Watt")
        float pwm[4];              // PWM/dew heater outputs (%)
        char  pwm_names[4][32];    // Names for each PWM output
        int   pwm_count;
        bool  switch_connected;
    } power;

    // Available filters from filter wheel
    nina_filter_t filters[MAX_FILTERS];
    int filter_count;

    // WebSocket state
    bool websocket_connected;

    // Set true when a new image is saved (IMAGE-SAVE event or image-history change)
    // Consumer should clear after handling
    volatile bool new_image_available;

    // Set true by WebSocket event handlers to request immediate UI refresh.
    // Data task checks this and refreshes UI without waiting for next poll cycle.
    volatile bool ui_refresh_needed;

    // Set by WebSocket event handlers when a sequence-relevant event occurs
    // (IMAGE-SAVE, TS-NEWTARGETSTART, SEQUENCE-STARTING). Causes immediate
    // sequence poll on next cycle.
    volatile bool sequence_poll_needed;

    // Timestamp (ms from esp_timer_get_time/1000) of last successful poll.
    // Used by the UI to display a stale-data indicator.  0 = never polled.
    int64_t last_successful_poll_ms;

    // IMAGE-SAVE detailed stats (captured from WebSocket events)
    imagestats_detail_data_t last_image_stats;

    // Autofocus V-curve data (captured from WebSocket events)
    autofocus_data_t autofocus;

    // Mutex for synchronizing access between WebSocket event handler and data task.
    // Must be created with nina_client_init_mutex() before use.
    SemaphoreHandle_t mutex;
} nina_client_t;

// Initialize the mutex for a nina_client_t instance. Call once after struct init.
void nina_client_init_mutex(nina_client_t *client);

// Lock/unlock helpers with short timeouts suitable for real-time use.
// nina_client_lock() returns true if the lock was acquired.
bool nina_client_lock(nina_client_t *client, uint32_t timeout_ms);
void nina_client_unlock(nina_client_t *client);

// Polling intervals (ms)
#define NINA_POLL_SLOW_MS     30000   // Focuser, mount, switch
#define NINA_POLL_SEQUENCE_MS 15000   // Sequence counts (supplemented by event-driven sequence_poll_needed)

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

    // Persistent HTTP client handle for keep-alive reuse (esp_http_client_handle_t)
    void *http_client;
} nina_poll_state_t;

// Initialize polling state (call once before polling loop)
void nina_poll_state_init(nina_poll_state_t *state);

// Tiered polling - fetches data at different rates based on change frequency
void nina_client_poll(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance);

// Heartbeat-only polling for background (inactive) instances
// Only fetches camera info to maintain connection status
void nina_client_poll_heartbeat(const char *base_url, nina_client_t *data, int instance);

// Background polling for inactive instances — pre-fetches slow-changing data
// (profile, filters, focuser, mount, switch, sequence) so it's ready on page switch.
// Skips fast-changing data: guider RMS, HFR/stars, current filter position.
void nina_client_poll_background(const char *base_url, nina_client_t *data, nina_poll_state_t *state, int instance);

// Legacy API - fetches all data every call (kept for compatibility)
void nina_client_get_data(const char *base_url, nina_client_t *data);

// Fetch prepared image as JPEG from NINA API
// Returns heap-allocated JPEG bytes (caller must free), or NULL on error
// Uses: GET /prepared-image?resize=true&size=WxH&quality=Q&autoPrepare=true
uint8_t *nina_client_fetch_prepared_image(const char *base_url, int width, int height, int quality, size_t *out_size);
