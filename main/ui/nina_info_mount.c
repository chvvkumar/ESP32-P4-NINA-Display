/**
 * @file nina_info_mount.c
 * @brief Mount Position detail overlay content builder.
 *
 * Hero RA/Dec coordinates at top, two-column cards below (POSITION, MERIDIAN,
 * TRACKING, SITE), and a status pill row at the bottom.
 */

#include "nina_info_internal.h"
#include <stdio.h>
#include <string.h>

/* ── Static label pointers ───────────────────────────────────────── */

/* Hero coordinates */
static lv_obj_t *lbl_ra_value    = NULL;
static lv_obj_t *lbl_dec_value   = NULL;

/* Position card */
static lv_obj_t *lbl_altitude    = NULL;
static lv_obj_t *lbl_azimuth     = NULL;
static lv_obj_t *lbl_pier_side   = NULL;
static lv_obj_t *lbl_alignment   = NULL;

/* Meridian card */
static lv_obj_t *lbl_flip_time   = NULL;

/* Tracking card */
static lv_obj_t *lbl_track_mode  = NULL;
static lv_obj_t *lbl_track_enabled = NULL;
static lv_obj_t *lbl_sidereal    = NULL;

/* Site card */
static lv_obj_t *lbl_latitude    = NULL;
static lv_obj_t *lbl_longitude   = NULL;
static lv_obj_t *lbl_elevation   = NULL;

/* Status pills */
static lv_obj_t *lbl_parked      = NULL;
static lv_obj_t *lbl_home        = NULL;
static lv_obj_t *lbl_slewing     = NULL;
static lv_obj_t *lbl_mount_name  = NULL;

/* Divider object (for theme updates) */
static lv_obj_t *coord_divider   = NULL;

/* ── Status pill helper ──────────────────────────────────────────── */

static lv_obj_t *make_status_pill(lv_obj_t *parent, const char *text, lv_obj_t **out_lbl) {
    int gb = app_config_get()->color_brightness;

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
    *out_lbl = lbl;
    return lbl;
}

/* ── Build content ───────────────────────────────────────────────── */

void build_mount_content(lv_obj_t *content) {
    /* Reset all pointers */
    lbl_ra_value = lbl_dec_value = NULL;
    lbl_altitude = lbl_azimuth = lbl_pier_side = lbl_alignment = NULL;
    lbl_flip_time = lbl_track_mode = lbl_track_enabled = lbl_sidereal = NULL;
    lbl_latitude = lbl_longitude = lbl_elevation = NULL;
    lbl_parked = lbl_home = lbl_slewing = lbl_mount_name = NULL;
    coord_divider = NULL;

    int gb = app_config_get()->color_brightness;

    /* Content uses column flow */
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);

    /* ── Hero: Coordinates card (full width) ── */
    {
        lv_obj_t *card = make_info_card(content);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_style_pad_all(card, 16, 0);
        make_info_section(card, "COORDINATES");

        lv_obj_t *coord_row = lv_obj_create(card);
        lv_obj_remove_style_all(coord_row);
        lv_obj_set_width(coord_row, LV_PCT(100));
        lv_obj_set_height(coord_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(coord_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(coord_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* RA block */
        lv_obj_t *ra_block = lv_obj_create(coord_row);
        lv_obj_remove_style_all(ra_block);
        lv_obj_set_size(ra_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ra_block, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ra_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *ra_lbl = lv_label_create(ra_block);
        lv_label_set_text(ra_lbl, "RA");
        lv_obj_set_style_text_font(ra_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_letter_space(ra_lbl, 2, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(ra_lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }

        lbl_ra_value = lv_label_create(ra_block);
        lv_label_set_text(lbl_ra_value, "--");
        lv_obj_set_style_text_font(lbl_ra_value, &lv_font_montserrat_36, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_ra_value,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        /* Vertical divider */
        coord_divider = lv_obj_create(coord_row);
        lv_obj_remove_style_all(coord_divider);
        lv_obj_set_size(coord_divider, 1, 48);
        if (current_theme) {
            lv_obj_set_style_bg_color(coord_divider,
                lv_color_hex(current_theme->bento_border), 0);
        }
        lv_obj_set_style_bg_opa(coord_divider, LV_OPA_COVER, 0);

        /* Dec block */
        lv_obj_t *dec_block = lv_obj_create(coord_row);
        lv_obj_remove_style_all(dec_block);
        lv_obj_set_size(dec_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(dec_block, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(dec_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *dec_lbl = lv_label_create(dec_block);
        lv_label_set_text(dec_lbl, "DEC");
        lv_obj_set_style_text_font(dec_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_letter_space(dec_lbl, 2, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(dec_lbl,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }

        lbl_dec_value = lv_label_create(dec_block);
        lv_label_set_text(lbl_dec_value, "--");
        lv_obj_set_style_text_font(lbl_dec_value, &lv_font_montserrat_36, 0);
        if (current_theme) {
            lv_obj_set_style_text_color(lbl_dec_value,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
    }

    /* ── Two-column row ── */
    lv_obj_t *cols = lv_obj_create(content);
    lv_obj_remove_style_all(cols);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_flex_grow(cols, 1);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, 10, 0);
    lv_obj_clear_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

    /* Left column */
    lv_obj_t *col_left = lv_obj_create(cols);
    lv_obj_remove_style_all(col_left);
    lv_obj_set_flex_grow(col_left, 1);
    lv_obj_set_height(col_left, LV_PCT(100));
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_left, 10, 0);
    lv_obj_clear_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);

    /* Right column */
    lv_obj_t *col_right = lv_obj_create(cols);
    lv_obj_remove_style_all(col_right);
    lv_obj_set_flex_grow(col_right, 1);
    lv_obj_set_height(col_right, LV_PCT(100));
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_right, 10, 0);
    lv_obj_clear_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);

    /* ── POSITION card (left) ── */
    {
        lv_obj_t *card = make_info_card(col_left);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "POSITION");
        make_info_kv(card, "Altitude",  &lbl_altitude);
        make_info_kv(card, "Azimuth",   &lbl_azimuth);
        make_info_kv(card, "Pier Side", &lbl_pier_side);
        make_info_kv(card, "Alignment", &lbl_alignment);
    }

    /* ── MERIDIAN card (left) ── */
    {
        lv_obj_t *card = make_info_card(col_left);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "MERIDIAN");
        make_info_kv_wide(card, "Flip In", &lbl_flip_time);
    }

    /* ── TRACKING card (right) ── */
    {
        lv_obj_t *card = make_info_card(col_right);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "TRACKING");
        make_info_kv(card, "Mode",       &lbl_track_mode);
        make_info_kv(card, "Enabled",    &lbl_track_enabled);
        make_info_kv(card, "Sidereal T", &lbl_sidereal);
    }

    /* ── SITE card (right) ── */
    {
        lv_obj_t *card = make_info_card(col_right);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "SITE");
        make_info_kv(card, "Latitude",  &lbl_latitude);
        make_info_kv(card, "Longitude", &lbl_longitude);
        make_info_kv(card, "Elevation", &lbl_elevation);
    }

    /* ── Status row (full width, compact pills) ── */
    {
        lv_obj_t *status_row = lv_obj_create(content);
        lv_obj_remove_style_all(status_row);
        lv_obj_set_width(status_row, LV_PCT(100));
        lv_obj_set_height(status_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(status_row, 4, 0);

        lv_obj_set_style_pad_right(status_row, INFO_BACK_BTN_ZONE, 0);
        make_status_pill(status_row, "Parked: --",  &lbl_parked);
        make_status_pill(status_row, "Home: --",    &lbl_home);
        make_status_pill(status_row, "Slewing: --", &lbl_slewing);
        make_status_pill(status_row, "Mount: --",   &lbl_mount_name);
    }
}

/* ── Populate data ───────────────────────────────────────────────── */

void populate_mount_data(const mount_detail_data_t *data) {
    char buf[64];
    int gb = app_config_get()->color_brightness;

    /* Hero coordinates */
    if (lbl_ra_value) lv_label_set_text(lbl_ra_value, data->ra_string);
    if (lbl_dec_value) lv_label_set_text(lbl_dec_value, data->dec_string);

    /* Position */
    if (lbl_altitude) {
        snprintf(buf, sizeof(buf), "%.1fd", (double)data->altitude);
        lv_label_set_text(lbl_altitude, buf);
    }
    if (lbl_azimuth) {
        snprintf(buf, sizeof(buf), "%.1fd", (double)data->azimuth);
        lv_label_set_text(lbl_azimuth, buf);
    }
    if (lbl_pier_side)  lv_label_set_text(lbl_pier_side, data->pier_side);
    if (lbl_alignment)  lv_label_set_text(lbl_alignment, data->alignment_mode);

    /* Meridian */
    if (lbl_flip_time) lv_label_set_text(lbl_flip_time, data->flip_time);

    /* Tracking */
    if (lbl_track_mode) lv_label_set_text(lbl_track_mode, data->tracking_mode);

    if (lbl_track_enabled) {
        lv_label_set_text(lbl_track_enabled, data->tracking_enabled ? "Yes" : "No");
        if (current_theme) {
            uint32_t color = data->tracking_enabled ? 0x4ade80 : current_theme->label_color;
            if (info_is_red_night()) color = current_theme->text_color;
            lv_obj_set_style_text_color(lbl_track_enabled,
                lv_color_hex(app_config_apply_brightness(color, gb)), 0);
        }
    }

    if (lbl_sidereal) lv_label_set_text(lbl_sidereal, data->sidereal_time);

    /* Site */
    if (lbl_latitude) {
        snprintf(buf, sizeof(buf), "%.4f %s",
                 (double)(data->latitude >= 0 ? data->latitude : -data->latitude),
                 data->latitude >= 0 ? "N" : "S");
        lv_label_set_text(lbl_latitude, buf);
    }
    if (lbl_longitude) {
        snprintf(buf, sizeof(buf), "%.4f %s",
                 (double)(data->longitude >= 0 ? data->longitude : -data->longitude),
                 data->longitude >= 0 ? "E" : "W");
        lv_label_set_text(lbl_longitude, buf);
    }
    if (lbl_elevation) {
        snprintf(buf, sizeof(buf), "%.0fm", (double)data->elevation);
        lv_label_set_text(lbl_elevation, buf);
    }

    /* Status pills */
    if (lbl_parked) {
        snprintf(buf, sizeof(buf), "Parked: %s", data->at_park ? "Yes" : "No");
        lv_label_set_text(lbl_parked, buf);
    }
    if (lbl_home) {
        snprintf(buf, sizeof(buf), "Home: %s", data->at_home ? "Yes" : "No");
        lv_label_set_text(lbl_home, buf);
    }
    if (lbl_slewing) {
        snprintf(buf, sizeof(buf), "Slewing: %s", data->slewing ? "Yes" : "No");
        lv_label_set_text(lbl_slewing, buf);
        if (current_theme) {
            uint32_t color = data->slewing ? 0xeab308 : current_theme->label_color;  /* yellow when slewing */
            if (info_is_red_night()) color = current_theme->text_color;
            lv_obj_set_style_text_color(lbl_slewing,
                lv_color_hex(app_config_apply_brightness(color, gb)), 0);
        }
    }
    if (lbl_mount_name) {
        snprintf(buf, sizeof(buf), "Mount: %s",
                 data->name[0] ? data->name : "--");
        lv_label_set_text(lbl_mount_name, buf);
    }
}

/* ── Theme update ────────────────────────────────────────────────── */

static void apply_theme_to_children_mount(lv_obj_t *obj);

static void apply_theme_to_label_mount(lv_obj_t *obj) {
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;

    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    if (font == &lv_font_montserrat_16) {
        /* Section header / coordinate sublabel / status pill */
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    } else if (font == &lv_font_montserrat_20) {
        /* Key label */
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    } else if (font == &lv_font_montserrat_36) {
        /* Hero RA/Dec values */
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    } else {
        /* Other value labels */
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
}

static void apply_theme_to_children_mount(lv_obj_t *obj) {
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            apply_theme_to_label_mount(child);
        }
        apply_theme_to_children_mount(child);
    }
}

void theme_mount_content(void) {
    if (!info_content || !current_theme) return;

    apply_theme_to_children_mount(info_content);

    /* Divider between RA and Dec */
    if (coord_divider) {
        lv_obj_set_style_bg_color(coord_divider,
            lv_color_hex(current_theme->bento_border), 0);
    }
}
