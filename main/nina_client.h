#pragma once

#include <stdbool.h>

// NINA client data structure
typedef struct {
    bool connected;
    char status[32];
    char target_name[64];
    char profile_name[64];
    
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
    int exposure_count;
    int exposure_iterations;
    float exposure_current;
    float exposure_total;
    char current_filter[32];
    char time_remaining[32];
    bool is_dithering;
    
    // Image stats
    float hfr;
    int stars;
    int saturated_pixels;
    
    // Mount
    char meridian_flip[16];
    float rotator_angle;
} nina_client_t;

// Fetch data from a NINA instance
// base_url: e.g., "http://astromele2.lan:1888/v2/api/"
// data: pointer to nina_client_t struct to populate
void nina_client_get_data(const char *base_url, nina_client_t *data);
