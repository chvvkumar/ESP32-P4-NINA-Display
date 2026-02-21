/**
 * @file nina_info_camera.c
 * @brief Camera + Weather detail overlay content builder.
 *
 * Two-column layout with 4 cards: SENSOR, EXPOSURE, COOLING, SETTINGS.
 * Optional full-width WEATHER card shown when weather device is connected.
 */

#include "nina_info_internal.h"
#include <stdio.h>
#include <string.h>

/* ── Static label pointers ───────────────────────────────────────── */

/* Sensor card */
static lv_obj_t *lbl_sensor_name  = NULL;
static lv_obj_t *lbl_resolution   = NULL;
static lv_obj_t *lbl_pixel_size   = NULL;
static lv_obj_t *lbl_bit_depth    = NULL;
static lv_obj_t *lbl_sensor_type  = NULL;

/* Cooling card */
static lv_obj_t *lbl_temp         = NULL;
static lv_obj_t *lbl_target_temp  = NULL;
static lv_obj_t *lbl_cooler       = NULL;
static lv_obj_t *lbl_at_target    = NULL;
static lv_obj_t *lbl_dew_heater   = NULL;

/* Exposure card */
static lv_obj_t *lbl_cam_state    = NULL;
static lv_obj_t *lbl_download     = NULL;
static lv_obj_t *lbl_binning      = NULL;

/* Settings card */
static lv_obj_t *lbl_gain         = NULL;
static lv_obj_t *lbl_offset       = NULL;
static lv_obj_t *lbl_readout      = NULL;
static lv_obj_t *lbl_usb_limit    = NULL;

/* Weather card */
static lv_obj_t *card_weather_obj = NULL;
static lv_obj_t *lbl_w_temp       = NULL;
static lv_obj_t *lbl_w_humidity   = NULL;
static lv_obj_t *lbl_w_dewpt      = NULL;
static lv_obj_t *lbl_w_wind       = NULL;
static lv_obj_t *lbl_w_cloud      = NULL;
static lv_obj_t *lbl_w_sqm        = NULL;

/* ── Weather stat block helper ───────────────────────────────────── */

static lv_obj_t *make_weather_block(lv_obj_t *parent, const char *title,
                                     lv_obj_t **out_val) {
    int gb = app_config_get()->color_brightness;

    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_flex_grow(block, 1);
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_title = lv_label_create(block);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl_title, 1, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    lv_obj_t *lbl_val = lv_label_create(block);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_18, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(lbl_val,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    *out_val = lbl_val;
    return block;
}

/* ── Build content ───────────────────────────────────────────────── */

void build_camera_content(lv_obj_t *content) {
    /* Reset all pointers */
    lbl_sensor_name = lbl_resolution = lbl_pixel_size = lbl_bit_depth = NULL;
    lbl_sensor_type = lbl_temp = lbl_target_temp = lbl_cooler = NULL;
    lbl_at_target = lbl_dew_heater = lbl_cam_state = lbl_download = NULL;
    lbl_binning = lbl_gain = lbl_offset = lbl_readout = lbl_usb_limit = NULL;
    card_weather_obj = lbl_w_temp = lbl_w_humidity = lbl_w_dewpt = NULL;
    lbl_w_wind = lbl_w_cloud = lbl_w_sqm = NULL;

    /* Content uses column flow */
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);

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

    /* ── SENSOR card (left) ── */
    {
        lv_obj_t *card = make_info_card(col_left);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "SENSOR");
        make_info_kv(card, "Name",       &lbl_sensor_name);
        lv_label_set_long_mode(lbl_sensor_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_sensor_name, 140);
        make_info_kv(card, "Resolution", &lbl_resolution);
        make_info_kv(card, "Pixel Size", &lbl_pixel_size);
        make_info_kv(card, "Bit Depth",  &lbl_bit_depth);
        make_info_kv(card, "Sensor",     &lbl_sensor_type);
    }

    /* ── EXPOSURE card (left) ── */
    {
        lv_obj_t *card = make_info_card(col_left);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "EXPOSURE");
        make_info_kv(card, "State",    &lbl_cam_state);
        make_info_kv(card, "Download", &lbl_download);
        make_info_kv(card, "Binning",  &lbl_binning);
    }

    /* ── COOLING card (right) ── */
    {
        lv_obj_t *card = make_info_card(col_right);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "COOLING");
        make_info_kv(card, "Temp",       &lbl_temp);
        make_info_kv(card, "Target",     &lbl_target_temp);
        make_info_kv(card, "Cooler",     &lbl_cooler);
        make_info_kv(card, "At Target",  &lbl_at_target);
        make_info_kv(card, "Dew Heater", &lbl_dew_heater);
    }

    /* ── SETTINGS card (right) ── */
    {
        lv_obj_t *card = make_info_card(col_right);
        lv_obj_set_flex_grow(card, 1);
        make_info_section(card, "SETTINGS");
        make_info_kv(card, "Gain",      &lbl_gain);
        make_info_kv(card, "Offset",    &lbl_offset);
        make_info_kv(card, "Readout",   &lbl_readout);
        make_info_kv(card, "USB Limit", &lbl_usb_limit);
    }

    /* ── WEATHER card (full width, hidden by default) ── */
    {
        card_weather_obj = make_info_card(content);
        lv_obj_set_width(card_weather_obj, LV_PCT(100));
        lv_obj_add_flag(card_weather_obj, LV_OBJ_FLAG_HIDDEN);
        make_info_section(card_weather_obj, "WEATHER");

        /* Row 1: TEMP, HUMIDITY, DEW PT */
        lv_obj_t *w_row1 = lv_obj_create(card_weather_obj);
        lv_obj_remove_style_all(w_row1);
        lv_obj_set_width(w_row1, LV_PCT(100));
        lv_obj_set_height(w_row1, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(w_row1, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(w_row1, 8, 0);

        make_weather_block(w_row1, "TEMP",     &lbl_w_temp);
        make_weather_block(w_row1, "HUMIDITY", &lbl_w_humidity);
        make_weather_block(w_row1, "DEW PT",   &lbl_w_dewpt);

        /* Row 2: WIND, CLOUD, SQM */
        lv_obj_t *w_row2 = lv_obj_create(card_weather_obj);
        lv_obj_remove_style_all(w_row2);
        lv_obj_set_width(w_row2, LV_PCT(100));
        lv_obj_set_height(w_row2, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(w_row2, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(w_row2, 8, 0);

        make_weather_block(w_row2, "WIND",  &lbl_w_wind);
        make_weather_block(w_row2, "CLOUD", &lbl_w_cloud);
        make_weather_block(w_row2, "SQM",   &lbl_w_sqm);
    }
}

/* ── Populate data ───────────────────────────────────────────────── */

void populate_camera_data(const camera_detail_data_t *data) {
    char buf[64];
    int gb = app_config_get()->color_brightness;

    /* Sensor */
    if (lbl_sensor_name) lv_label_set_text(lbl_sensor_name, data->name);

    if (lbl_resolution) {
        snprintf(buf, sizeof(buf), "%d x %d", data->x_size, data->y_size);
        lv_label_set_text(lbl_resolution, buf);
    }

    if (lbl_pixel_size) {
        snprintf(buf, sizeof(buf), "%.2f um", (double)data->pixel_size);
        lv_label_set_text(lbl_pixel_size, buf);
    }

    if (lbl_bit_depth) {
        snprintf(buf, sizeof(buf), "%d", data->bit_depth);
        lv_label_set_text(lbl_bit_depth, buf);
    }

    if (lbl_sensor_type) lv_label_set_text(lbl_sensor_type, data->sensor_type);

    /* Cooling */
    if (lbl_temp) {
        snprintf(buf, sizeof(buf), "%.1f C", (double)data->temperature);
        lv_label_set_text(lbl_temp, buf);
    }

    if (lbl_target_temp) {
        snprintf(buf, sizeof(buf), "%.1f C", (double)data->target_temp);
        lv_label_set_text(lbl_target_temp, buf);
    }

    if (lbl_cooler) {
        snprintf(buf, sizeof(buf), "%s (%.0f%%)",
                 data->cooler_on ? "On" : "Off", (double)data->cooler_power);
        lv_label_set_text(lbl_cooler, buf);

        /* Colorize cooler power: green <50%, yellow 50-80%, red >80% */
        if (current_theme) {
            uint32_t color;
            if (data->cooler_power < 50.0f) color = 0x4ade80;       /* green */
            else if (data->cooler_power < 80.0f) color = 0xeab308;  /* yellow */
            else color = 0xef4444;                                    /* red */
            if (info_is_red_night()) color = current_theme->text_color;
            lv_obj_set_style_text_color(lbl_cooler,
                lv_color_hex(app_config_apply_brightness(color, gb)), 0);
        }
    }

    if (lbl_at_target) {
        lv_label_set_text(lbl_at_target, data->at_target ? "Yes" : "No");
        if (current_theme) {
            uint32_t color = data->at_target ? 0x4ade80 : current_theme->label_color;
            if (info_is_red_night()) color = current_theme->text_color;
            lv_obj_set_style_text_color(lbl_at_target,
                lv_color_hex(app_config_apply_brightness(color, gb)), 0);
        }
    }

    if (lbl_dew_heater) {
        lv_label_set_text(lbl_dew_heater, data->dew_heater_on ? "On" : "Off");
    }

    /* Exposure */
    if (lbl_cam_state) lv_label_set_text(lbl_cam_state, data->camera_state);

    if (lbl_download) {
        snprintf(buf, sizeof(buf), "%.1fs", (double)data->last_download_time);
        lv_label_set_text(lbl_download, buf);
    }

    if (lbl_binning) {
        snprintf(buf, sizeof(buf), "%dx%d", data->bin_x, data->bin_y);
        lv_label_set_text(lbl_binning, buf);
    }

    /* Settings */
    if (lbl_gain) {
        snprintf(buf, sizeof(buf), "%d (%d-%d)", data->gain, data->gain_min, data->gain_max);
        lv_label_set_text(lbl_gain, buf);
    }

    if (lbl_offset) {
        snprintf(buf, sizeof(buf), "%d", data->offset);
        lv_label_set_text(lbl_offset, buf);
    }

    if (lbl_readout) lv_label_set_text(lbl_readout, data->readout_mode);

    if (lbl_usb_limit) {
        snprintf(buf, sizeof(buf), "%d", data->usb_limit);
        lv_label_set_text(lbl_usb_limit, buf);
    }

    /* Weather */
    if (data->weather_connected && card_weather_obj) {
        lv_obj_clear_flag(card_weather_obj, LV_OBJ_FLAG_HIDDEN);

        if (lbl_w_temp) {
            snprintf(buf, sizeof(buf), "%.1fC", (double)data->weather_temp);
            lv_label_set_text(lbl_w_temp, buf);
        }
        if (lbl_w_humidity) {
            snprintf(buf, sizeof(buf), "%.0f%%", (double)data->humidity);
            lv_label_set_text(lbl_w_humidity, buf);
        }
        if (lbl_w_dewpt) {
            snprintf(buf, sizeof(buf), "%.1fC", (double)data->dew_point);
            lv_label_set_text(lbl_w_dewpt, buf);
        }
        if (lbl_w_wind) {
            snprintf(buf, sizeof(buf), "%.0f km/h", (double)data->wind_speed);
            lv_label_set_text(lbl_w_wind, buf);
        }
        if (lbl_w_cloud) {
            snprintf(buf, sizeof(buf), "%d%%", data->cloud_cover);
            lv_label_set_text(lbl_w_cloud, buf);
        }
        if (lbl_w_sqm) {
            lv_label_set_text(lbl_w_sqm, data->sky_quality);
        }
    } else if (card_weather_obj) {
        lv_obj_add_flag(card_weather_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Theme update ────────────────────────────────────────────────── */

static void apply_theme_to_children(lv_obj_t *obj);

static void apply_theme_to_label_cam(lv_obj_t *obj) {
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;

    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    if (font == &lv_font_montserrat_14) {
        /* Section header */
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    } else if (font == &lv_font_montserrat_16 || font == &lv_font_montserrat_12) {
        /* Key label */
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    } else {
        /* Value label */
        lv_obj_set_style_text_color(obj,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
}

static void apply_theme_to_children(lv_obj_t *obj) {
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            apply_theme_to_label_cam(child);
        }
        apply_theme_to_children(child);
    }
}

void theme_camera_content(void) {
    if (!info_content || !current_theme) return;
    apply_theme_to_children(info_content);
}
