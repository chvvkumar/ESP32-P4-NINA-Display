/**
 * @file nina_dashboard_update.c
 * @brief Data update functions and arc animation for the NINA dashboard.
 */

#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define STALE_WARN_MS   30000   /* 30 s: show "Last update" label */
#define STALE_DIM_MS   120000   /* 2 min: dim the entire page */

/* Red Night theme forces all colors to the theme palette, ignoring filter/threshold overrides */
static bool theme_forces_colors(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

// Arc animation callback
static void arc_fill_complete_cb(lv_anim_t *a) {
    dashboard_page_t *p = (dashboard_page_t *)a->user_data;
    if (!p) return;

    /* Reset arc to 0 instantly (visually invisible since it was just at 100)
     * and let the interpolation timer ramp it up smoothly on its next tick. */
    lv_arc_set_value(p->arc_exposure, 0);
    p->arc_completing = false;
}

void nina_dashboard_update_status(int page_index, int rssi, bool nina_connected, bool api_active) {
    if (page_index < 0 || page_index >= page_count) return;
    dashboard_page_t *p = &pages[page_index];
    if (!p->page) return;

    p->nina_connected = nina_connected;

    uint32_t text_color;
    if (theme_forces_colors()) {
        text_color = nina_connected ? current_theme->text_color : current_theme->label_color;
    } else {
        text_color = nina_connected ? 0x4ade80 : 0xf87171;
    }

    int gb = app_config_get()->color_brightness;
    text_color = app_config_apply_brightness(text_color, gb);

    if (p->lbl_instance_name) {
        lv_obj_set_style_text_color(p->lbl_instance_name, lv_color_hex(text_color), 0);
    }
}

/* ── Smooth RMS / HFR value animation ─────────────────────────────── */
#define VALUE_ANIM_MS  500

static void arcsec_anim_exec(void *obj, int32_t v) {
    lv_label_set_text_fmt((lv_obj_t *)obj, "%.2f\"", v / 100.0f);
}

static void hfr_anim_exec(void *obj, int32_t v) {
    lv_label_set_text_fmt((lv_obj_t *)obj, "%.2f", v / 100.0f);
}

static void animate_value(lv_obj_t *label, int32_t from, int32_t to,
                          lv_anim_exec_xcb_t exec_cb) {
    lv_anim_delete(label, exec_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, VALUE_ANIM_MS);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ---- Sub-functions for update_nina_dashboard_page() ---- */

static void update_disconnected_state(dashboard_page_t *p, int page_index, int gb) {
    const char *url = app_config_get_instance_url(page_index);
    char host[64] = {0};
    extract_host_from_url(url, host, sizeof(host));
    if (host[0] != '\0') {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s - Not Connected", host);
        lv_label_set_text(p->lbl_instance_name, buf);
    } else {
        lv_label_set_text(p->lbl_instance_name, "N.I.N.A.");
    }
    lv_label_set_text(p->lbl_target_name, "----");
    lv_label_set_text(p->lbl_seq_container, "----");
    lv_label_set_text(p->lbl_seq_step, "----");
    lv_label_set_text(p->lbl_exposure_current, "--");
    lv_label_set_text(p->lbl_exposure_total, "");
    lv_arc_set_value(p->arc_exposure, 0);
    lv_label_set_text(p->lbl_loop_count, "-- / --");
    lv_anim_delete(p->lbl_rms_value, arcsec_anim_exec);
    lv_label_set_text(p->lbl_rms_value, "--");
    lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    p->anim_rms_total_x100 = 0;
    if (p->lbl_rms_ra_value)  { lv_anim_delete(p->lbl_rms_ra_value, arcsec_anim_exec);  p->anim_rms_ra_x100 = 0; }
    if (p->lbl_rms_dec_value) { lv_anim_delete(p->lbl_rms_dec_value, arcsec_anim_exec); p->anim_rms_dec_x100 = 0; }
    lv_anim_delete(p->lbl_hfr_value, hfr_anim_exec);
    lv_label_set_text(p->lbl_hfr_value, "--");
    lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    p->anim_hfr_x100 = 0;
    lv_label_set_text(p->lbl_flip_value, "--");
    lv_label_set_text(p->lbl_stars_value, "--");
    lv_label_set_text(p->lbl_target_time_value, "--");
    for (int i = 0; i < MAX_POWER_WIDGETS; i++) {
        lv_obj_add_flag(p->box_pwr[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_header(dashboard_page_t *p, const nina_client_t *d) {
    lv_label_set_text(p->lbl_instance_name,
        d->telescope_name[0] != '\0' ? d->telescope_name : "N.I.N.A.");
    lv_label_set_text(p->lbl_target_name, d->target_name[0] != '\0' ? d->target_name : "----");
}

static void update_sequence_info(dashboard_page_t *p, const nina_client_t *d) {
    lv_label_set_text(p->lbl_seq_container,
        d->container_name[0] != '\0' ? d->container_name : "----");
    lv_label_set_text(p->lbl_seq_step,
        d->container_step[0] != '\0' ? d->container_step : "----");
}

static void update_exposure_arc(dashboard_page_t *p, const nina_client_t *d,
                                int page_index, int gb) {
    uint32_t filter_color = app_config_apply_brightness(current_theme->progress_color, gb);
    if (!theme_forces_colors() && d->current_filter[0] != '\0' && strcmp(d->current_filter, "--") != 0) {
        filter_color = app_config_get_filter_color(d->current_filter, page_index);
    }
    lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(p->arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);

    // Detect filter change — reset arc animation state to avoid stale progress/color
    if (d->current_filter[0] != '\0' && strcmp(p->prev_filter, d->current_filter) != 0) {
        lv_anim_delete(p->arc_exposure, (lv_anim_exec_xcb_t)lv_arc_set_value);
        p->arc_completing = false;
        p->prev_target_progress = 0;
        p->pending_arc_progress = 0;
        lv_arc_set_value(p->arc_exposure, 0);
        snprintf(p->prev_filter, sizeof(p->prev_filter), "%s", d->current_filter);
    }

    if (d->exposure_total > 0 && d->exposure_end_epoch > 0) {
        float elapsed = d->exposure_current;
        float total = d->exposure_total;

        // Cache interpolation state for the LVGL timer
        p->interp_end_epoch = d->exposure_end_epoch;
        p->interp_total = total;
        p->interp_filter_color = filter_color;

        // Show total exposure duration inside the arc
        int total_sec = (int)total;
        lv_label_set_text_fmt(p->lbl_exposure_current, "%ds", total_sec);

        // Update filter label (below the duration)
        if (d->current_filter[0] != '\0' && strcmp(d->current_filter, "--") != 0) {
            lv_label_set_text(p->lbl_exposure_total, d->current_filter);
            lv_obj_set_style_text_color(p->lbl_exposure_total, lv_color_hex(filter_color), 0);
        } else {
            lv_label_set_text(p->lbl_exposure_total, "");
        }

        int progress = (int)((elapsed * 100) / total);
        if (progress > 100) progress = 100;
        if (progress < 0) progress = 0;

        int current_val = lv_arc_get_value(p->arc_exposure);
        bool new_exposure = (p->prev_target_progress > 70 && progress < 30);
        p->prev_target_progress = progress;

        if (new_exposure && current_val > 0) {
            p->arc_completing = true;
            p->pending_arc_progress = progress;

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, p->arc_exposure);
            lv_anim_set_values(&a, current_val, 100);
            lv_anim_set_time(&a, 300);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            a.user_data = p;
            lv_anim_set_ready_cb(&a, arc_fill_complete_cb);
            lv_anim_start(&a);
        } else if (p->arc_completing) {
            p->pending_arc_progress = progress;
            if (progress > 30) {
                p->arc_completing = false;
                lv_arc_set_value(p->arc_exposure, progress);
            }
        }
        /* Normal arc updates are now handled by the interpolation timer
         * for smooth sub-second progression. Only the new-exposure animation
         * above needs to run here. */

        if (d->exposure_iterations > 0) {
            lv_label_set_text_fmt(p->lbl_loop_count, "%d / %d",
                d->exposure_count, d->exposure_iterations);
        } else {
            lv_label_set_text(p->lbl_loop_count, "-- / --");
        }
    } else {
        p->interp_end_epoch = 0;  // Not exposing — stop interpolation
        lv_label_set_text(p->lbl_exposure_current, "--");
        lv_label_set_text(p->lbl_exposure_total, "");
        lv_arc_set_value(p->arc_exposure, 0);
        lv_label_set_text(p->lbl_loop_count, "-- / --");
    }
}

/* ── Exposure Interpolation Timer ────────────────────────────────────
 * Fires every 200ms in LVGL context (display lock held) to smoothly
 * update the exposure countdown label and arc progress between polls.
 */
void exposure_interp_timer_cb(lv_timer_t *timer) {
    (void)timer;
    int idx = active_page - 1;  // active_page 1..N maps to page index 0..N-1
    if (idx < 0 || idx >= page_count) return;

    dashboard_page_t *p = &pages[idx];
    if (!p->page || p->interp_end_epoch == 0 || p->interp_total <= 0) return;
    if (p->arc_completing) return;  // Don't interfere with new-exposure animation

    time_t now = time(NULL);
    double remaining = difftime((time_t)p->interp_end_epoch, now);
    if (remaining < 0) remaining = 0;

    float elapsed = p->interp_total - (float)remaining;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > p->interp_total) elapsed = p->interp_total;

    int progress = (int)((elapsed * 100) / p->interp_total);
    if (progress > 100) progress = 100;
    if (progress < 0) progress = 0;

    // Animate arc smoothly only when the integer progress actually changes
    int current_val = lv_arc_get_value(p->arc_exposure);
    if (progress != current_val) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, p->arc_exposure);
        lv_anim_set_values(&a, current_val, progress);
        lv_anim_set_time(&a, 400);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

static void update_guider_stats(dashboard_page_t *p, const nina_client_t *d,
                                int page_index, int gb) {
    /* ── RMS Total ── */
    if (d->guider.rms_total > 0) {
        int32_t new_val = (int32_t)(d->guider.rms_total * 100.0f + 0.5f);
        uint32_t rms_color = theme_forces_colors()
            ? app_config_apply_brightness(current_theme->rms_color, gb)
            : app_config_get_rms_color(d->guider.rms_total, page_index);
        lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(rms_color), 0);

        if (p->anim_rms_total_x100 > 0 && new_val != p->anim_rms_total_x100) {
            animate_value(p->lbl_rms_value, p->anim_rms_total_x100, new_val, arcsec_anim_exec);
        } else {
            lv_anim_delete(p->lbl_rms_value, arcsec_anim_exec);
            lv_label_set_text_fmt(p->lbl_rms_value, "%.2f\"", d->guider.rms_total);
        }
        p->anim_rms_total_x100 = new_val;
    } else {
        lv_anim_delete(p->lbl_rms_value, arcsec_anim_exec);
        lv_label_set_text(p->lbl_rms_value, "--");
        lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        p->anim_rms_total_x100 = 0;
    }

    /* ── RMS RA ── */
    if (p->lbl_rms_ra_value) {
        if (d->guider.rms_ra > 0) {
            int32_t new_val = (int32_t)(d->guider.rms_ra * 100.0f + 0.5f);
            if (p->anim_rms_ra_x100 > 0 && new_val != p->anim_rms_ra_x100) {
                animate_value(p->lbl_rms_ra_value, p->anim_rms_ra_x100, new_val, arcsec_anim_exec);
            } else {
                lv_anim_delete(p->lbl_rms_ra_value, arcsec_anim_exec);
                lv_label_set_text_fmt(p->lbl_rms_ra_value, "%.2f\"", d->guider.rms_ra);
            }
            p->anim_rms_ra_x100 = new_val;
        } else {
            lv_anim_delete(p->lbl_rms_ra_value, arcsec_anim_exec);
            lv_label_set_text(p->lbl_rms_ra_value, "--");
            p->anim_rms_ra_x100 = 0;
        }
    }

    /* ── RMS DEC ── */
    if (p->lbl_rms_dec_value) {
        if (d->guider.rms_dec > 0) {
            int32_t new_val = (int32_t)(d->guider.rms_dec * 100.0f + 0.5f);
            if (p->anim_rms_dec_x100 > 0 && new_val != p->anim_rms_dec_x100) {
                animate_value(p->lbl_rms_dec_value, p->anim_rms_dec_x100, new_val, arcsec_anim_exec);
            } else {
                lv_anim_delete(p->lbl_rms_dec_value, arcsec_anim_exec);
                lv_label_set_text_fmt(p->lbl_rms_dec_value, "%.2f\"", d->guider.rms_dec);
            }
            p->anim_rms_dec_x100 = new_val;
        } else {
            lv_anim_delete(p->lbl_rms_dec_value, arcsec_anim_exec);
            lv_label_set_text(p->lbl_rms_dec_value, "--");
            p->anim_rms_dec_x100 = 0;
        }
    }

    /* ── HFR ── */
    if (d->hfr > 0) {
        int32_t new_val = (int32_t)(d->hfr * 100.0f + 0.5f);
        uint32_t hfr_color = theme_forces_colors()
            ? app_config_apply_brightness(current_theme->hfr_color, gb)
            : app_config_get_hfr_color(d->hfr, page_index);
        lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(hfr_color), 0);

        if (p->anim_hfr_x100 > 0 && new_val != p->anim_hfr_x100) {
            animate_value(p->lbl_hfr_value, p->anim_hfr_x100, new_val, hfr_anim_exec);
        } else {
            lv_anim_delete(p->lbl_hfr_value, hfr_anim_exec);
            lv_label_set_text_fmt(p->lbl_hfr_value, "%.2f", d->hfr);
        }
        p->anim_hfr_x100 = new_val;
    } else {
        lv_anim_delete(p->lbl_hfr_value, hfr_anim_exec);
        lv_label_set_text(p->lbl_hfr_value, "--");
        lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        p->anim_hfr_x100 = 0;
    }
}

static void update_mount_and_image_stats(dashboard_page_t *p, const nina_client_t *d) {
    lv_label_set_text(p->lbl_flip_value,
        (d->meridian_flip[0] != '\0' && strcmp(d->meridian_flip, "--") != 0)
            ? d->meridian_flip : "--");

    lv_label_set_text_fmt(p->lbl_stars_value,
        d->stars >= 0 ? "%d" : "--", d->stars);

    lv_label_set_text(p->lbl_target_time_value,
        d->target_time_remaining[0] != '\0' ? d->target_time_remaining : "--");
}

static void update_power(dashboard_page_t *p, const nina_client_t *d) {
    int pwr_idx = 0;
    bool sw = d->power.switch_connected;

    #define UPPER(buf) do { for (int _c = 0; (buf)[_c]; _c++) \
        if ((buf)[_c] >= 'a' && (buf)[_c] <= 'z') (buf)[_c] -= 32; } while(0)

    if (sw && pwr_idx < MAX_POWER_WIDGETS) {
        char title[32];
        strncpy(title, d->power.amps_name[0] ? d->power.amps_name : "Amps", sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        UPPER(title);
        lv_label_set_text(p->lbl_pwr_title[pwr_idx], title);
        lv_label_set_text_fmt(p->lbl_pwr_value[pwr_idx], "%.2fA", d->power.total_amps);
        lv_obj_clear_flag(p->box_pwr[pwr_idx], LV_OBJ_FLAG_HIDDEN);
        pwr_idx++;
    }
    if (sw && pwr_idx < MAX_POWER_WIDGETS) {
        char title[32];
        strncpy(title, d->power.watts_name[0] ? d->power.watts_name : "Watts", sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        UPPER(title);
        lv_label_set_text(p->lbl_pwr_title[pwr_idx], title);
        lv_label_set_text_fmt(p->lbl_pwr_value[pwr_idx], "%.1fW", d->power.total_watts);
        lv_obj_clear_flag(p->box_pwr[pwr_idx], LV_OBJ_FLAG_HIDDEN);
        pwr_idx++;
    }
    if (sw) {
        for (int i = 0; i < d->power.pwm_count && pwr_idx < MAX_POWER_WIDGETS; i++, pwr_idx++) {
            char title[32];
            strncpy(title, d->power.pwm_names[i], sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';
            UPPER(title);
            lv_label_set_text(p->lbl_pwr_title[pwr_idx], title);
            lv_label_set_text_fmt(p->lbl_pwr_value[pwr_idx], "%.0f%%", d->power.pwm[i]);
            lv_obj_clear_flag(p->box_pwr[pwr_idx], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = pwr_idx; i < MAX_POWER_WIDGETS; i++) {
        lv_obj_add_flag(p->box_pwr[i], LV_OBJ_FLAG_HIDDEN);
    }

    #undef UPPER
}

static void update_stale_indicator(dashboard_page_t *p, const nina_client_t *d) {
    if (!p->lbl_stale) return;

    /* Nothing to compare against until the first successful poll */
    if (d->last_successful_poll_ms == 0) {
        lv_obj_add_flag(p->lbl_stale, LV_OBJ_FLAG_HIDDEN);
        if (p->stale_overlay)
            lv_obj_add_flag(p->stale_overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t stale_ms = now_ms - d->last_successful_poll_ms;

    if (stale_ms > STALE_WARN_MS) {
        int stale_sec = (int)(stale_ms / 1000);
        if (stale_sec >= 120)
            lv_label_set_text_fmt(p->lbl_stale, "Last update: %dm ago", stale_sec / 60);
        else
            lv_label_set_text_fmt(p->lbl_stale, "Last update: %ds ago", stale_sec);

        /* Red text when severely stale */
        uint32_t stale_color = (stale_ms > STALE_DIM_MS) ? 0xf87171 : 0xfbbf24;
        lv_obj_set_style_text_color(p->lbl_stale, lv_color_hex(stale_color), 0);

        lv_obj_clear_flag(p->lbl_stale, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(p->lbl_stale, LV_OBJ_FLAG_HIDDEN);
    }

    /* Dim overlay when data is very stale (> 2 min) */
    if (p->stale_overlay) {
        if (stale_ms > STALE_DIM_MS)
            lv_obj_clear_flag(p->stale_overlay, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(p->stale_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void update_nina_dashboard_page(int page_index, const nina_client_t *data) {
    if (page_index < 0 || page_index >= page_count) return;
    if (!data) return;

    dashboard_page_t *p = &pages[page_index];
    if (!p->page) return;

    int gb = app_config_get()->color_brightness;

    if (!data->connected) {
        update_disconnected_state(p, page_index, gb);
        update_stale_indicator(p, data);
        return;
    }

    update_header(p, data);
    update_sequence_info(p, data);
    update_exposure_arc(p, data, page_index, gb);
    update_guider_stats(p, data, page_index, gb);
    update_mount_and_image_stats(p, data);
    update_power(p, data);
    update_stale_indicator(p, data);
}
