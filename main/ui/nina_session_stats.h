#pragma once

/**
 * @file nina_session_stats.h
 * @brief Session statistics collector -- PSRAM ring buffers for analytics.
 *
 * All public functions are thread-safe (spinlock-protected).
 * No LVGL calls -- purely a data store.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define SESSION_MAX_POINTS 500

typedef struct {
    float   rms_total;
    float   hfr;
    float   temperature;
    int     stars;
    float   cooler_power;
    int64_t timestamp_ms;
} session_data_point_t;

typedef struct {
    session_data_point_t *points;   /* PSRAM-allocated ring buffer */
    int     count;                  /* Total points recorded */
    int     write_index;            /* Next write position (wraps) */
    int     capacity;               /* SESSION_MAX_POINTS */

    /* Running statistics */
    float   rms_min, rms_max, rms_sum;
    int     rms_count;
    float   hfr_min, hfr_max, hfr_sum;
    int     hfr_count;

    /* Exposure tracking */
    int     total_exposures;
    float   total_exposure_time_s;
    int64_t session_start_ms;       /* Timestamp of first record (0 = none) */
} session_stats_t;

/** Allocate PSRAM buffers.  Call once at startup. */
void nina_session_stats_init(void);

/** Record a data point.  Thread-safe. */
void nina_session_stats_record(int instance, float rms_total, float hfr,
                               float temperature, int stars, float cooler_power);

/** Reset all stats for all instances.  Thread-safe. */
void nina_session_stats_reset(void);

/** Get read-only pointer to instance stats.  Caller should copy quickly. */
const session_stats_t *nina_session_stats_get(int instance);

/** Add exposure time.  Thread-safe. */
void nina_session_stats_add_exposure(int instance, float exposure_time_s);

#ifdef __cplusplus
}
#endif
