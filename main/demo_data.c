#include "demo_data.h"
#include "app_config.h"
#include "nina_connection.h"
#include "ui/nina_session_stats.h"
#include "ui/nina_safety.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "demo_data";

/* ── Random helpers ────────────────────────────────────────────────── */

static float rand_float(float min, float max)
{
    return min + (float)(esp_random() % 10000) / 10000.0f * (max - min);
}

static float random_walk(float current, float step, float lo, float hi)
{
    float delta = rand_float(-step, step);
    float v = current + delta;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

/* ── Demo profile (static per-instance configuration) ─────────────── */

typedef struct {
    const char *profile_name;
    const char *telescope_name;
    const char *camera_name;
    const char *target_name;
    const char *container_name;
    const char *container_step;

    const char *filter_names[6];
    float       filter_exposure_s[6];
    int         filter_iterations[6];
    int         filter_count;

    float camera_temp;
    float cooler_power_base;
    int   focuser_base;
    float rotator_angle;
    bool  rotator_connected;
    float moon_illumination;

    int   flip_start_s;   /* initial countdown seconds, 0 = no flip */
    int   flip_reset_s;   /* reset value after countdown expires    */

    float power_voltage;
    float power_amps;
    float dew_heater_pwm;

    float rms_ra_base;
    float rms_dec_base;
    float hfr_base;
    int   star_base;
} demo_profile_t;

static const demo_profile_t profiles[3] = {
    /* Instance 0 — Narrowband SHO */
    {
        .profile_name   = "Esprit 100 + ASI2600MM",
        .telescope_name = "Sky-Watcher Esprit 100ED",
        .camera_name    = "ZWO ASI2600MM Pro",
        .target_name    = "NGC 6992 - Eastern Veil Nebula",
        .container_name = "SHO",
        .container_step = "Smart Exposure",
        .filter_names      = { "SII", "Ha", "OIII" },
        .filter_exposure_s = { 600, 300, 300 },
        .filter_iterations = { 20, 30, 30 },
        .filter_count      = 3,
        .camera_temp       = -20.0f,
        .cooler_power_base = 72.0f,
        .focuser_base      = 22500,
        .rotator_angle     = 127.5f,
        .rotator_connected = true,
        .moon_illumination = 23.0f,
        .flip_start_s      = 8100,
        .flip_reset_s      = 16200,
        .power_voltage     = 12.2f,
        .power_amps        = 4.2f,
        .dew_heater_pwm    = 42.0f,
        .rms_ra_base       = 0.52f,
        .rms_dec_base      = 0.38f,
        .hfr_base          = 2.1f,
        .star_base         = 95,
    },
    /* Instance 1 — Broadband LRGB */
    {
        .profile_name   = "RASA 8 Imaging Rig",
        .telescope_name = "Celestron RASA 8",
        .camera_name    = "ZWO ASI6200MM Pro",
        .target_name    = "M31 - Andromeda Galaxy",
        .container_name = "LRGB",
        .container_step = "Smart Exposure",
        .filter_names      = { "L", "R", "G", "B" },
        .filter_exposure_s = { 180, 120, 120, 120 },
        .filter_iterations = { 40, 20, 20, 20 },
        .filter_count      = 4,
        .camera_temp       = -10.0f,
        .cooler_power_base = 55.0f,
        .focuser_base      = 15800,
        .rotator_angle     = 0.0f,
        .rotator_connected = false,
        .moon_illumination = 23.0f,
        .flip_start_s      = 0,
        .flip_reset_s      = 0,
        .power_voltage     = 13.2f,
        .power_amps        = 3.8f,
        .dew_heater_pwm    = 35.0f,
        .rms_ra_base       = 0.45f,
        .rms_dec_base      = 0.32f,
        .hfr_base          = 1.8f,
        .star_base         = 280,
    },
    /* Instance 2 — Bicolor HOO */
    {
        .profile_name   = "FSQ-106 + QHY268M",
        .telescope_name = "Takahashi FSQ-106EDX4",
        .camera_name    = "QHY268M",
        .target_name    = "IC 1396 - Elephant's Trunk Nebula",
        .container_name = "HOO",
        .container_step = "Smart Exposure",
        .filter_names      = { "Ha", "OIII" },
        .filter_exposure_s = { 300, 300 },
        .filter_iterations = { 40, 40 },
        .filter_count      = 2,
        .camera_temp       = -15.0f,
        .cooler_power_base = 63.0f,
        .focuser_base      = 31200,
        .rotator_angle     = 45.0f,
        .rotator_connected = true,
        .moon_illumination = 23.0f,
        .flip_start_s      = 2700,
        .flip_reset_s      = 10800,
        .power_voltage     = 12.0f,
        .power_amps        = 5.1f,
        .dew_heater_pwm    = 50.0f,
        .rms_ra_base       = 0.58f,
        .rms_dec_base      = 0.42f,
        .hfr_base          = 2.3f,
        .star_base         = 110,
    },
};

/* ── Per-instance mutable runtime state ───────────────────────────── */

typedef struct {
    int   current_filter_idx;
    float exposure_current;
    int   exposure_count;
    int   exposure_total_count;

    float rms_ra;
    float rms_dec;
    float hfr;
    int   focuser_pos;
    int   flip_countdown_s;

    float voltage;
    float amps;
    float dew_pwm;
    float cooler_power;

    bool  dithering;
} demo_state_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

static float star_multiplier(const char *filter)
{
    if (!filter) return 0.5f;
    if (strcmp(filter, "L") == 0)    return 1.0f;
    if (strcmp(filter, "R") == 0)    return 0.75f;
    if (strcmp(filter, "G") == 0)    return 0.75f;
    if (strcmp(filter, "B") == 0)    return 0.75f;
    if (strcmp(filter, "Ha") == 0)   return 0.45f;
    if (strcmp(filter, "OIII") == 0) return 0.30f;
    if (strcmp(filter, "SII") == 0)  return 0.20f;
    return 0.5f;
}

/** Compute total remaining seconds for the sequence. */
static int compute_remaining_s(const demo_profile_t *prof, const demo_state_t *st)
{
    int idx = st->current_filter_idx;
    /* Remaining subs in current filter × exposure time */
    int remaining_current = (prof->filter_iterations[idx] - st->exposure_count);
    if (remaining_current < 0) remaining_current = 0;
    int total_s = (int)(remaining_current * prof->filter_exposure_s[idx]);

    /* All subsequent filters */
    for (int f = idx + 1; f < prof->filter_count; f++) {
        total_s += (int)(prof->filter_iterations[f] * prof->filter_exposure_s[f]);
    }
    return total_s;
}

/* ── Main task ────────────────────────────────────────────────────── */

void demo_data_task(void *param)
{
    demo_task_params_t *p = (demo_task_params_t *)param;
    nina_client_t *instances = p->instances;
    allsky_data_t *allsky    = p->allsky;
    int count = p->instance_count;
    if (count > 3) count = 3;

    ESP_LOGI(TAG, "Starting demo data generator for %d instances", count);

    /* ── Initialise per-instance state ────────────────────────────── */
    demo_state_t state[3];
    memset(state, 0, sizeof(state));

    for (int i = 0; i < count; i++) {
        const demo_profile_t *prof = &profiles[i];
        demo_state_t *st = &state[i];

        st->current_filter_idx = 0;
        st->exposure_current   = 0.0f;
        st->exposure_count     = 0;
        st->exposure_total_count = 0;
        st->rms_ra       = prof->rms_ra_base;
        st->rms_dec      = prof->rms_dec_base;
        st->hfr           = prof->hfr_base;
        st->focuser_pos   = prof->focuser_base;
        st->flip_countdown_s = prof->flip_start_s;
        st->voltage       = prof->power_voltage;
        st->amps          = prof->power_amps;
        st->dew_pwm       = prof->dew_heater_pwm;
        st->cooler_power  = prof->cooler_power_base;
        st->dithering     = false;

        /* Report connected state to the connection manager */
        nina_connection_report_poll(i, true);
        nina_connection_report_ws(i, true);
        nina_connection_set_static_data_ready(i, true);
    }

    /* Report safety monitor as connected and safe */
    nina_safety_update(true, true);

    /* AllSky walk state */
    float as_thermal = 12.5f;
    float as_sqm     = 20.8f;
    float as_ambient = 14.0f;
    float as_humidity = 58.0f;
    float as_dewpoint = 6.0f;
    float as_power_a  = 5.1f;
    float as_power_v  = 12.0f;

    /* Target time remaining cycles 0 → 3:45:00 in seconds */
    int target_time_s = 13500; /* start at 3:45:00 */

    int cycle = 0;

    /* ── Main loop — 2 second period ─────────────────────────────── */
    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        time_t now_epoch;
        time(&now_epoch);

        for (int i = 0; i < count; i++) {
            nina_client_t *d = &instances[i];
            const demo_profile_t *prof = &profiles[i];
            demo_state_t *st = &state[i];

            if (!nina_client_lock(d, 1000)) {
                ESP_LOGW(TAG, "Failed to lock instance %d", i);
                continue;
            }

            /* ── Static / status fields ──────────────────────────── */
            d->connected = true;
            d->websocket_connected = true;
            d->last_successful_poll_ms = now_ms;
            strncpy(d->status, "EXPOSING", sizeof(d->status) - 1);
            strncpy(d->target_name, prof->target_name, sizeof(d->target_name) - 1);
            strncpy(d->profile_name, prof->profile_name, sizeof(d->profile_name) - 1);
            strncpy(d->telescope_name, prof->telescope_name, sizeof(d->telescope_name) - 1);
            strncpy(d->camera_name, prof->camera_name, sizeof(d->camera_name) - 1);
            strncpy(d->container_name, prof->container_name, sizeof(d->container_name) - 1);
            strncpy(d->container_step, prof->container_step, sizeof(d->container_step) - 1);

            /* ── Camera ──────────────────────────────────────────── */
            d->camera.temp = prof->camera_temp + rand_float(-0.1f, 0.1f);
            st->cooler_power = random_walk(st->cooler_power, 2.0f,
                                           prof->cooler_power_base - 8.0f,
                                           prof->cooler_power_base + 5.0f);
            d->camera.cooler_power = st->cooler_power;

            /* ── Filters ─────────────────────────────────────────── */
            d->filter_count = prof->filter_count;
            for (int f = 0; f < prof->filter_count; f++) {
                strncpy(d->filters[f].name, prof->filter_names[f],
                        sizeof(d->filters[f].name) - 1);
                d->filters[f].id = f;
            }

            /* ── Current filter ──────────────────────────────────── */
            int fidx = st->current_filter_idx;
            strncpy(d->current_filter, prof->filter_names[fidx],
                    sizeof(d->current_filter) - 1);

            /* ── Exposure progress ───────────────────────────────── */
            st->exposure_current += 2.0f; /* 2 second tick */
            d->exposure_current    = st->exposure_current;
            d->exposure_total      = prof->filter_exposure_s[fidx];
            d->exposure_count      = st->exposure_count;
            d->exposure_iterations = prof->filter_iterations[fidx];
            d->exposure_total_count = st->exposure_total_count;

            float remaining_in_exposure = d->exposure_total - d->exposure_current;
            if (remaining_in_exposure < 0) remaining_in_exposure = 0;
            d->exposure_end_epoch = (int64_t)now_epoch + (int64_t)remaining_in_exposure;

            /* ── Guider RMS ──────────────────────────────────────── */
            bool spike = (esp_random() % 100) < 5;
            if (spike) {
                st->rms_ra  = rand_float(0.9f, 1.3f);
                st->rms_dec = rand_float(0.7f, 1.0f);
            } else {
                st->rms_ra  = random_walk(st->rms_ra,  0.02f, 0.25f, 0.85f);
                st->rms_dec = random_walk(st->rms_dec, 0.015f, 0.18f, 0.65f);
            }
            d->guider.rms_ra  = st->rms_ra;
            d->guider.rms_dec = st->rms_dec;
            d->guider.rms_total = sqrtf(st->rms_ra * st->rms_ra +
                                        st->rms_dec * st->rms_dec);

            /* ── Focuser ─────────────────────────────────────────── */
            st->focuser_pos = (int)random_walk((float)st->focuser_pos, 1.0f,
                                               (float)(prof->focuser_base - 300),
                                               (float)(prof->focuser_base + 300));
            d->focuser.position = st->focuser_pos;

            /* ── Moon ────────────────────────────────────────────── */
            d->moon.illumination = prof->moon_illumination;

            /* ── Rotator ─────────────────────────────────────────── */
            d->rotator_angle     = prof->rotator_angle;
            d->rotator_connected = prof->rotator_connected;

            /* ── Safety ──────────────────────────────────────────── */
            d->safety_connected = true;
            d->safety_is_safe   = true;

            /* ── Meridian flip ───────────────────────────────────── */
            if (prof->flip_start_s > 0) {
                st->flip_countdown_s -= 2;
                if (st->flip_countdown_s <= 0) {
                    st->flip_countdown_s = prof->flip_reset_s;
                }
                int h = st->flip_countdown_s / 3600;
                int m = (st->flip_countdown_s % 3600) / 60;
                snprintf(d->meridian_flip, sizeof(d->meridian_flip),
                         "%d:%02d", h, m);
            } else {
                strncpy(d->meridian_flip, "--:--", sizeof(d->meridian_flip) - 1);
            }

            /* ── Target conditions ───────────────────────────────── */
            {
                int tt = target_time_s;
                int th = tt / 3600;
                int tm = (tt % 3600) / 60;
                snprintf(d->target_time_remaining, sizeof(d->target_time_remaining),
                         "%d:%02d", th, tm);
                strncpy(d->target_time_reason, "SETS IN", sizeof(d->target_time_reason) - 1);
                d->target_condition_count = 2;
            }

            /* ── Sequence time remaining ─────────────────────────── */
            {
                int rem = compute_remaining_s(prof, st);
                int rh = rem / 3600;
                int rm = (rem % 3600) / 60;
                int rs = rem % 60;
                snprintf(d->time_remaining, sizeof(d->time_remaining),
                         "%d:%02d:%02d", rh, rm, rs);
            }

            /* ── Power box ───────────────────────────────────────── */
            st->voltage = random_walk(st->voltage, 0.05f, 11.8f, 13.5f);
            st->amps    = random_walk(st->amps, 0.1f,
                                      prof->power_amps - 1.0f,
                                      prof->power_amps + 1.0f);
            st->dew_pwm = random_walk(st->dew_pwm, 1.5f, 20.0f, 60.0f);

            d->power.input_voltage = st->voltage;
            d->power.total_amps    = st->amps;
            d->power.total_watts   = st->voltage * st->amps;
            strncpy(d->power.amps_name, "Total Current", sizeof(d->power.amps_name) - 1);
            strncpy(d->power.watts_name, "Total Power", sizeof(d->power.watts_name) - 1);
            d->power.pwm[0] = st->dew_pwm;
            d->power.pwm[1] = st->dew_pwm * 0.8f;
            d->power.pwm[2] = 0.0f;
            d->power.pwm[3] = 0.0f;
            strncpy(d->power.pwm_names[0], "Dew Heater 1", sizeof(d->power.pwm_names[0]) - 1);
            strncpy(d->power.pwm_names[1], "Dew Heater 2", sizeof(d->power.pwm_names[1]) - 1);
            strncpy(d->power.pwm_names[2], "Fan",          sizeof(d->power.pwm_names[2]) - 1);
            strncpy(d->power.pwm_names[3], "Aux",          sizeof(d->power.pwm_names[3]) - 1);
            d->power.pwm_count        = 4;
            d->power.switch_connected = true;

            /* ── Dithering ───────────────────────────────────────── */
            d->is_dithering = st->dithering;
            st->dithering = false;

            /* ── Wait state ──────────────────────────────────────── */
            d->is_waiting = false;
            d->wait_start_epoch = 0;

            /* ── Exposure completion ─────────────────────────────── */
            if (st->exposure_current >= prof->filter_exposure_s[fidx]) {
                st->exposure_count++;
                st->exposure_total_count++;
                st->exposure_current = 0.0f;
                st->dithering = true;
                d->new_image_available = true;
                d->ui_refresh_needed   = true;

                /* HFR + stars */
                st->hfr = random_walk(st->hfr, 0.05f, 1.4f, 3.2f);
                float mult = star_multiplier(prof->filter_names[fidx]);
                int stars = (int)(prof->star_base * mult * rand_float(0.85f, 1.15f));
                d->hfr   = st->hfr;
                d->stars  = stars;

                /* HFR ring buffer */
                if (d->hfr_ring.hfr && d->hfr_ring.stars) {
                    int wi = d->hfr_ring.write_idx;
                    d->hfr_ring.hfr[wi]   = st->hfr;
                    d->hfr_ring.stars[wi]  = stars;
                    d->hfr_ring.write_idx  = (wi + 1) % HFR_RING_SIZE;
                    d->hfr_ring.count++;
                }

                /* Last image stats */
                d->last_image_stats.has_data       = true;
                d->last_image_stats.stars           = stars;
                d->last_image_stats.hfr             = st->hfr;
                d->last_image_stats.hfr_stdev       = rand_float(0.1f, 0.4f);
                d->last_image_stats.mean            = rand_float(800.0f, 1500.0f);
                d->last_image_stats.median          = d->last_image_stats.mean - rand_float(20.0f, 80.0f);
                d->last_image_stats.stdev           = rand_float(150.0f, 400.0f);
                d->last_image_stats.min_val         = (int)rand_float(0.0f, 50.0f);
                d->last_image_stats.max_val         = (int)rand_float(55000.0f, 65535.0f);
                d->last_image_stats.exposure_time   = prof->filter_exposure_s[fidx];
                strncpy(d->last_image_stats.filter, prof->filter_names[fidx],
                        sizeof(d->last_image_stats.filter) - 1);
                d->last_image_stats.gain            = 100;
                d->last_image_stats.offset          = 50;
                d->last_image_stats.temperature     = prof->camera_temp;
                strncpy(d->last_image_stats.camera_name, prof->camera_name,
                        sizeof(d->last_image_stats.camera_name) - 1);
                strncpy(d->last_image_stats.telescope_name, prof->telescope_name,
                        sizeof(d->last_image_stats.telescope_name) - 1);
                d->last_image_stats.focal_length    = 550;

                /* Record session stats (must unlock first) */
                float rms_total = d->guider.rms_total;
                float cooler_pwr = d->camera.cooler_power;
                float temp = d->camera.temp;
                nina_client_unlock(d);

                nina_session_stats_record(i, rms_total, st->hfr, temp, stars, cooler_pwr);

                if (!nina_client_lock(d, 1000)) {
                    ESP_LOGW(TAG, "Failed to re-lock instance %d after stats", i);
                    continue;
                }

                /* Advance filter if iterations complete */
                if (st->exposure_count >= prof->filter_iterations[fidx]) {
                    st->current_filter_idx = (st->current_filter_idx + 1) % prof->filter_count;
                    st->exposure_count = 0;
                }
            }

            nina_client_unlock(d);
        }

        /* ── Target time countdown ───────────────────────────────── */
        target_time_s -= 2;
        if (target_time_s <= 0) target_time_s = 13500; /* 3:45:00 */

        /* ── AllSky data — every 5th cycle (10 seconds) ──────────── */
        if (allsky && (cycle % 5) == 0) {
            as_thermal  = random_walk(as_thermal,  0.3f,  8.0f, 18.0f);
            as_sqm      = random_walk(as_sqm,      0.05f, 19.5f, 21.5f);
            as_ambient   = random_walk(as_ambient,  0.2f,  8.0f, 22.0f);
            as_humidity  = random_walk(as_humidity,  1.5f, 35.0f, 80.0f);
            as_dewpoint  = random_walk(as_dewpoint,  0.2f,  0.0f, 12.0f);
            as_power_a   = random_walk(as_power_a,  0.15f, 3.0f,  7.0f);
            as_power_v   = random_walk(as_power_v,  0.05f, 11.5f, 13.0f);

            if (allsky_data_lock(allsky, 500)) {
                allsky->connected    = true;
                allsky->last_poll_ms = now_ms;

                float thermal_sub1 = as_thermal - rand_float(0.5f, 1.5f);
                float ambient_dew_spread = as_ambient - as_dewpoint;
                float watts = as_power_a * as_power_v;

                snprintf(allsky->field_values[ALLSKY_F_THERMAL_MAIN], 32,
                         "%.1f\xC2\xB0""C", as_thermal);
                snprintf(allsky->field_values[ALLSKY_F_THERMAL_SUB1], 32,
                         "%.1f\xC2\xB0""C", thermal_sub1);
                snprintf(allsky->field_values[ALLSKY_F_THERMAL_SUB2], 32,
                         "%.1f\xC2\xB0""C", ambient_dew_spread);
                snprintf(allsky->field_values[ALLSKY_F_SQM_MAIN], 32,
                         "%.1f", as_sqm);
                snprintf(allsky->field_values[ALLSKY_F_SQM_SUB1], 32,
                         "Bortle 5");
                snprintf(allsky->field_values[ALLSKY_F_SQM_SUB2], 32,
                         "%.1f", as_sqm + rand_float(-0.2f, 0.2f));
                snprintf(allsky->field_values[ALLSKY_F_AMBIENT_MAIN], 32,
                         "%.0f\xC2\xB0""C", as_ambient);
                snprintf(allsky->field_values[ALLSKY_F_AMBIENT_SUB1], 32,
                         "%.0f%%", as_humidity);
                snprintf(allsky->field_values[ALLSKY_F_AMBIENT_SUB2], 32,
                         "%.0f\xC2\xB0""C", as_dewpoint);
                allsky->field_values[ALLSKY_F_AMBIENT_DOT1][0] = '\0';
                allsky->field_values[ALLSKY_F_AMBIENT_DOT2][0] = '\0';
                snprintf(allsky->field_values[ALLSKY_F_POWER_MAIN], 32,
                         "%.1f A", as_power_a);
                snprintf(allsky->field_values[ALLSKY_F_POWER_SUB1], 32,
                         "%.0f W", watts);
                snprintf(allsky->field_values[ALLSKY_F_POWER_SUB2], 32,
                         "%.1f V", as_power_v);
                allsky->field_values[ALLSKY_F_SQM_DOT1][0] = '\0';

                allsky_data_unlock(allsky);
            }
        }

        cycle++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
