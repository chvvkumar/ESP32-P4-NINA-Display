#pragma once

#include <stdbool.h>

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
} nina_client_t;

// Fetch data from a NINA instance
// base_url: e.g., "http://astromele2.lan:1888/v2/api/"
// data: pointer to nina_client_t struct to populate
void nina_client_get_data(const char *base_url, nina_client_t *data);
