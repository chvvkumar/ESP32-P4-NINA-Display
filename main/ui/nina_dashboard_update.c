/**
 * @file nina_dashboard_update.c
 * @brief Data update functions and arc animation for the NINA dashboard.
 */

#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include <stdio.h>
#include <string.h>

// Arc animation callbacks
static void arc_reset_complete_cb(lv_anim_t *a) {
    dashboard_page_t *p = (dashboard_page_t *)a->user_data;
    if (p) p->arc_completing = false;
}

static void arc_fill_complete_cb(lv_anim_t *a) {
    dashboard_page_t *p = (dashboard_page_t *)a->user_data;
    if (!p) return;

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, p->arc_exposure);
    lv_anim_set_values(&a2, 0, p->pending_arc_progress);
    lv_anim_set_time(&a2, 350);
    lv_anim_set_exec_cb(&a2, (lv_anim_exec_xcb_t)lv_arc_set_value);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_out);
    a2.user_data = p;
    lv_anim_set_ready_cb(&a2, arc_reset_complete_cb);
    lv_anim_start(&a2);
}

void nina_dashboard_update_status(int page_index, int rssi, bool nina_connected, bool api_active) {
    if (page_index < 0 || page_index >= page_count) return;
    dashboard_page_t *p = &pages[page_index];
    if (!p->page) return;

    uint32_t text_color = nina_connected ? 0x4ade80 : 0xf87171;

    int gb = app_config_get()->color_brightness;
    text_color = app_config_apply_brightness(text_color, gb);

    if (p->lbl_instance_name) {
        lv_obj_set_style_text_color(p->lbl_instance_name, lv_color_hex(text_color), 0);
    }
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
    lv_label_set_text(p->lbl_rms_value, "--");
    lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    lv_label_set_text(p->lbl_hfr_value, "--");
    lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
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
    if (d->current_filter[0] != '\0' && strcmp(d->current_filter, "--") != 0) {
        filter_color = app_config_get_filter_color(d->current_filter, page_index);
    }
    lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(p->arc_exposure, lv_color_hex(filter_color), LV_PART_INDICATOR);

    if (d->exposure_total > 0) {
        float elapsed = d->exposure_current;
        float total = d->exposure_total;
        int total_sec = (int)total;

        lv_label_set_text_fmt(p->lbl_exposure_current, "%ds", total_sec);

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
        } else {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, p->arc_exposure);
            lv_anim_set_values(&a, current_val, progress);
            lv_anim_set_time(&a, 350);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }

        if (d->exposure_iterations > 0) {
            lv_label_set_text_fmt(p->lbl_loop_count, "%d / %d",
                d->exposure_count, d->exposure_iterations);
        } else {
            lv_label_set_text(p->lbl_loop_count, "-- / --");
        }
    } else {
        lv_label_set_text(p->lbl_exposure_current, "--");
        lv_label_set_text(p->lbl_exposure_total, "");
        lv_arc_set_value(p->arc_exposure, 0);
        lv_label_set_text(p->lbl_loop_count, "-- / --");
    }
}

static void update_guider_stats(dashboard_page_t *p, const nina_client_t *d,
                                int page_index, int gb) {
    if (d->guider.rms_total > 0) {
        lv_label_set_text_fmt(p->lbl_rms_value, "%.2f\"", d->guider.rms_total);
        uint32_t rms_color = app_config_get_rms_color(d->guider.rms_total, page_index);
        lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(rms_color), 0);
    } else {
        lv_label_set_text(p->lbl_rms_value, "--");
        lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    if (p->lbl_rms_ra_value) {
        lv_label_set_text_fmt(p->lbl_rms_ra_value,
            d->guider.rms_ra > 0 ? "%.2f\"" : "--", d->guider.rms_ra);
    }
    if (p->lbl_rms_dec_value) {
        lv_label_set_text_fmt(p->lbl_rms_dec_value,
            d->guider.rms_dec > 0 ? "%.2f\"" : "--", d->guider.rms_dec);
    }

    if (d->hfr > 0) {
        lv_label_set_text_fmt(p->lbl_hfr_value, "%.2f", d->hfr);
        uint32_t hfr_color = app_config_get_hfr_color(d->hfr, page_index);
        lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(hfr_color), 0);
    } else {
        lv_label_set_text(p->lbl_hfr_value, "--");
        lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
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

void update_nina_dashboard_page(int page_index, const nina_client_t *data) {
    if (page_index < 0 || page_index >= page_count) return;
    if (!data) return;

    dashboard_page_t *p = &pages[page_index];
    if (!p->page) return;

    int gb = app_config_get()->color_brightness;

    if (!data->connected) {
        update_disconnected_state(p, page_index, gb);
        return;
    }

    update_header(p, data);
    update_sequence_info(p, data);
    update_exposure_arc(p, data, page_index, gb);
    update_guider_stats(p, data, page_index, gb);
    update_mount_and_image_stats(p, data);
    update_power(p, data);
}
