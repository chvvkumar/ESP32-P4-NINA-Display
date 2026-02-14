#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Data structure holding status for one NINA instance (Orbital Theme)
typedef struct {
    bool connected;
    char profile_name[64];     // NINA Profile Name (e.g. "ASI2600mm_SQA55_AM5")
    char target_name[64];
    char status[32];           // Camera state: "EXPOSING", "IDLE", "DITHERING", etc.
    float exposure_current;    // Current exposure time in seconds
    float exposure_total;      // Total exposure time in seconds
    bool is_dithering;         // True if status == "Dithering"
    
    // Stats for the 4-card grid
    float guide_rms;           // Total Guiding RMS in arcseconds
    float guide_ra_rms;        // RA Guiding RMS
    float guide_dec_rms;       // Dec Guiding RMS
    float cam_temp;            // Camera temperature in Celsius
    int cooler_power;          // Cooler power percentage (0-100)
    char meridian_flip[16];    // Time to meridian flip (e.g., "01:40h")
    float hfr;                 // Half Flux Radius
    int stars;                 // Star count
    int focuser_pos;           // Focuser position
    float rotator_angle;       // Rotator angle
    char current_filter[32];   // Current Filter Name
    int saturated_pixels;      // Number of saturated pixels
    char time_remaining[32];   // Time remaining for target (e.g. "02:15:00")

    // Sequence info
    int exposure_count;        // Current exposure number
    int exposure_iterations;   // Total exposures in sequence
} NinaData;

// Initialize the dashboard UI (Orbital Theme)
void nina_dashboard_init(void);

// Update the data for a specific instance (0 or 1)
void nina_dashboard_update(int instance_index, const NinaData *data);

// Cycle between the two rig views (0 -> 1 -> 0)
void nina_dashboard_cycle_view(void);

#ifdef __cplusplus
}
#endif
