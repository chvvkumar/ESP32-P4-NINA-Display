#pragma once

/**
 * @file weather_client.h
 * @brief Provider-abstracted HTTP weather client with mutex-protected data.
 *
 * Supports OpenWeatherMap 2.5, Open-Meteo, and Weather Underground.
 * Data is polled on a dedicated FreeRTOS task and made available
 * via weather_client_get_data() under mutex protection.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float   temp_current;
    float   temp_high;
    float   temp_low;
    float   humidity;
    float   dew_point;
    float   wind_speed;
    char    wind_dir[4];
    float   uv_index;
    char    condition[32];
    float   hourly_temps[10];
    uint8_t hourly_hours[10];
    bool    valid;
    int64_t last_update_ts;
} weather_data_t;

/** Initialise internal state (creates mutex). Call once before start. */
void weather_client_init(void);

/** Spawn the weather poll task. Call after WiFi init. */
void weather_client_start(void);

/** Copy current weather data into *out under mutex. */
void weather_client_get_data(weather_data_t *out);

/** Returns true if cached data is valid (successful fetch completed). */
bool weather_client_has_valid_data(void);

/** Wake the poll task immediately for an out-of-cycle refresh. */
void weather_client_force_refresh(void);

/** Clear cached weather data (marks invalid). Call when provider changes. */
void weather_client_invalidate(void);
