/**
 * @file nina_session_stats.c
 * @brief Session statistics collector -- PSRAM ring buffers for analytics.
 *
 * All public functions are thread-safe via spinlock.
 * No LVGL calls -- purely a data store.
 */

#include "nina_session_stats.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>
#include <float.h>

static const char *TAG = "stats";

/* ── Module state ───────────────────────────────────────────────────── */
static session_stats_t s_stats[MAX_NINA_INSTANCES];
static portMUX_TYPE    s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Public API ─────────────────────────────────────────────────────── */

void nina_session_stats_init(void) {
    memset(s_stats, 0, sizeof(s_stats));

    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        s_stats[i].points = (session_data_point_t *)heap_caps_calloc(
            SESSION_MAX_POINTS, sizeof(session_data_point_t), MALLOC_CAP_SPIRAM);
        if (!s_stats[i].points) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM for instance %d stats", i);
            continue;
        }
        s_stats[i].capacity = SESSION_MAX_POINTS;
        s_stats[i].rms_min = FLT_MAX;
        s_stats[i].hfr_min = FLT_MAX;
        ESP_LOGI(TAG, "Instance %d: allocated %d points in PSRAM", i, SESSION_MAX_POINTS);
    }

    ESP_LOGI(TAG, "Session stats initialized");
}

void nina_session_stats_record(int instance, float rms_total, float hfr,
                               float temperature, int stars, float cooler_power) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;

    session_stats_t *st = &s_stats[instance];
    if (!st->points) return;

    int64_t t = esp_timer_get_time() / 1000;

    portENTER_CRITICAL(&s_lock);

    /* Set session start on first record */
    if (st->session_start_ms == 0) {
        st->session_start_ms = t;
    }

    /* Write data point to ring buffer */
    session_data_point_t *pt = &st->points[st->write_index];
    pt->rms_total = rms_total;
    pt->hfr = hfr;
    pt->temperature = temperature;
    pt->stars = stars;
    pt->cooler_power = cooler_power;
    pt->timestamp_ms = t;

    st->write_index = (st->write_index + 1) % st->capacity;
    if (st->count < st->capacity) st->count++;

    /* Update running RMS statistics (skip zero/invalid) */
    if (rms_total > 0.0f) {
        if (rms_total < st->rms_min) st->rms_min = rms_total;
        if (rms_total > st->rms_max) st->rms_max = rms_total;
        st->rms_sum += rms_total;
        st->rms_count++;
    }

    /* Update running HFR statistics (skip zero/invalid) */
    if (hfr > 0.0f) {
        if (hfr < st->hfr_min) st->hfr_min = hfr;
        if (hfr > st->hfr_max) st->hfr_max = hfr;
        st->hfr_sum += hfr;
        st->hfr_count++;
    }

    portEXIT_CRITICAL(&s_lock);
}

void nina_session_stats_reset(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;

    portENTER_CRITICAL(&s_lock);
    session_stats_t *st = &s_stats[instance];
    session_data_point_t *pts = st->points; /* Preserve allocation */
    int cap = st->capacity;

    if (pts) {
        memset(pts, 0, cap * sizeof(session_data_point_t));
    }

    memset(st, 0, sizeof(session_stats_t));
    st->points = pts;
    st->capacity = cap;
    st->rms_min = FLT_MAX;
    st->hfr_min = FLT_MAX;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "Session stats reset for instance %d", instance);
}

const session_stats_t *nina_session_stats_get(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return NULL;
    return &s_stats[instance];
}

void nina_session_stats_add_exposure(int instance, float exposure_time_s) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;

    portENTER_CRITICAL(&s_lock);
    s_stats[instance].total_exposures++;
    s_stats[instance].total_exposure_time_s += exposure_time_s;
    portEXIT_CRITICAL(&s_lock);
}
