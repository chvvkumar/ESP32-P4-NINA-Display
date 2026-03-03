#pragma once

/**
 * @file allsky_client.h
 * @brief AllSky API HTTP client — polls /all endpoint and extracts configured field values.
 *
 * The field_config_json maps quadrant names (thermal, sqm, ambient, power)
 * to JSON key paths using dot-notation (e.g. "pistatus.AS_CPUTEMP").
 * Each quadrant has main/sub1/sub2 slots (ambient also has dot1/dot2).
 */

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ALLSKY_MAX_FIELDS 16

/* Field index mapping — matches quadrant layout in the AllSky UI page */
#define ALLSKY_F_THERMAL_MAIN   0
#define ALLSKY_F_THERMAL_SUB1   1
#define ALLSKY_F_THERMAL_SUB2   2
#define ALLSKY_F_SQM_MAIN       3
#define ALLSKY_F_SQM_SUB1       4
#define ALLSKY_F_SQM_SUB2       5
#define ALLSKY_F_AMBIENT_MAIN   6
#define ALLSKY_F_AMBIENT_SUB1   7
#define ALLSKY_F_AMBIENT_SUB2   8
#define ALLSKY_F_AMBIENT_DOT1   9
#define ALLSKY_F_AMBIENT_DOT2  10
#define ALLSKY_F_POWER_MAIN    11
#define ALLSKY_F_POWER_SUB1    12
#define ALLSKY_F_POWER_SUB2    13

typedef struct {
    bool connected;
    char field_values[ALLSKY_MAX_FIELDS][32];
    int64_t last_poll_ms;
    SemaphoreHandle_t mutex;
} allsky_data_t;

/** Initialise allsky_data_t (creates mutex). Call once before any polling. */
void allsky_data_init(allsky_data_t *data);

/** Lock the data struct. Returns true if acquired within timeout_ms. */
bool allsky_data_lock(allsky_data_t *data, int timeout_ms);

/** Unlock the data struct. */
void allsky_data_unlock(allsky_data_t *data);

/**
 * Poll the AllSky API at http://{hostname}/all, extract fields according
 * to field_config_json, and update *data under the mutex.
 *
 * @param hostname         AllSky host:port (e.g. "allskypi5.lan:8080")
 * @param field_config_json  JSON mapping quadrants to dot-notation key paths
 * @param data             Output data struct (mutex-protected)
 */
void allsky_client_poll(const char *hostname, const char *field_config_json, allsky_data_t *data);
