#pragma once

/**
 * @file info_overlay_types.h
 * @brief Data structures for info overlay panels.
 *
 * These types are used to transfer data between the API/WebSocket layer
 * and the UI overlay display.  No LVGL or UI dependencies here.
 */

#include <stdbool.h>
#include <stdint.h>

/** Overlay type selector */
typedef enum {
    INFO_OVERLAY_CAMERA = 0,
    INFO_OVERLAY_MOUNT,
    INFO_OVERLAY_IMAGESTATS,
    INFO_OVERLAY_SEQUENCE,
    INFO_OVERLAY_FILTER,
    INFO_OVERLAY_AUTOFOCUS,
    INFO_OVERLAY_SESSION_STATS,
} info_overlay_type_t;

/** Camera + weather detail data (fetched from /equipment/camera/info + /equipment/weather/info) */
typedef struct {
    // Sensor
    char name[64];
    int x_size, y_size;
    float pixel_size;
    int bit_depth;
    char sensor_type[16];
    // Cooling
    float temperature, target_temp;
    float cooler_power;
    bool cooler_on, at_target, dew_heater_on;
    // Exposure
    char camera_state[16];
    float last_download_time;
    int bin_x, bin_y;
    // Settings
    int gain, gain_min, gain_max;
    int offset, offset_min, offset_max;
    char readout_mode[32];
    int usb_limit;
    int battery;
    // Weather (populated only if weather connected)
    bool weather_connected;
    float weather_temp, dew_point, humidity;
    float pressure, wind_speed;
    int wind_direction, cloud_cover;
    char sky_quality[16];
} camera_detail_data_t;

/** Mount position detail data (fetched from /equipment/mount/info) */
typedef struct {
    // Coordinates
    char ra_string[16], dec_string[16];
    float ra_degrees, dec_degrees;
    // Position
    float altitude, azimuth;
    char pier_side[16];
    char alignment_mode[16];
    // Tracking
    char tracking_mode[16];
    bool tracking_enabled;
    char sidereal_time[16];
    // Meridian
    char flip_time[16];
    // Site
    float latitude, longitude, elevation;
    // Status
    bool at_park, at_home, slewing;
    // Device
    char name[64];
    bool connected;
} mount_detail_data_t;

/** Per-image statistics (captured from IMAGE-SAVE WebSocket events) */
typedef struct {
    // Image quality
    int stars;
    float hfr, hfr_stdev;
    // Pixel stats
    float mean, median, stdev;
    int min_val, max_val;
    // Capture settings
    float exposure_time;
    char filter[32];
    int gain, offset;
    float temperature;
    // Equipment
    char camera_name[64];
    char telescope_name[64];
    int focal_length;
    // Metadata
    char date[32];
    char filename[128];
    bool has_data;  // false if no image captured yet
} imagestats_detail_data_t;

/** Sequence progress with per-filter breakdown (fetched from /sequence/json) */
#define MAX_SEQ_FILTERS 10
typedef struct {
    char target_name[64];
    char time_remaining[16];
    // Current step
    char container_name[64];
    char step_name[64];
    char current_filter[32];
    float current_exposure_time;
    int current_completed, current_total;
    // Per-filter breakdown
    struct {
        char name[32];
        int completed, total;
    } filters[MAX_SEQ_FILTERS];
    int filter_count;
    // Totals
    int total_completed, total_total;
    bool has_data;
} sequence_detail_data_t;

/** Filter wheel detail data — uses existing nina_client_t.filters[] and current_filter */
typedef struct {
    char current_filter[32];
    int current_position;
    struct {
        char name[32];
        int id;
    } filters[10];  // Matches MAX_FILTERS
    int filter_count;
    char device_name[64];
    bool connected;
} filter_detail_data_t;

/** Autofocus V-curve data (collected from WebSocket events) */
#define MAX_AF_POINTS 50
typedef struct {
    struct {
        int position;
        float hfr;
    } points[MAX_AF_POINTS];
    int count;
    int best_position;   // Calculated best focus position
    float best_hfr;      // HFR at best position
    bool af_running;     // Currently running autofocus
    bool has_data;       // Have we collected any AF data
} autofocus_data_t;
