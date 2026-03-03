/**
 * @file nina_allsky.c
 * @brief AllSky four-quadrant page — thermal, SQM, ambient, power.
 *
 * Displays a 2x2 grid of bento boxes with main value, threshold bar,
 * sub-values, and indicator dots (ambient quadrant).
 */

#include "nina_allsky.h"
#include "nina_dashboard_internal.h"
#include "app_config.h"
#include "themes.h"
#include "ui_styles.h"
#include "ui_helpers.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
static const char *TAG = "allsky_ui";

/* ── Quadrant widget storage ─────────────────────────────────────────── */

typedef struct {
    lv_obj_t *box;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_main_value;
    lv_obj_t *lbl_unit;
    lv_obj_t *bar;
    lv_obj_t *lbl_sub1;
    lv_obj_t *lbl_sub2;
    lv_obj_t *dot1;   /* AMBIENT only */
    lv_obj_t *dot2;   /* AMBIENT only */
} allsky_quadrant_t;

static lv_obj_t *allsky_page = NULL;
static allsky_quadrant_t quads[4]; /* 0=thermal, 1=sqm, 2=ambient, 3=power */

/* ── Per-quadrant config parsed from JSON ────────────────────────────── */

typedef struct {
    char unit[32];
    char sub1_label[16];
    char sub1_suffix[16];
    char sub2_label[16];
    char sub2_suffix[16];
    char dot1_on_value[16];
    char dot2_on_value[16];
    char bar_threshold[32]; /* key into allsky_thresholds JSON */
} quadrant_config_t;

static quadrant_config_t qcfg[4];

/* ── Threshold cache per quadrant bar ────────────────────────────────── */

typedef struct {
    bool  valid;
    float min;
    float max;
    uint32_t color_min;
    uint32_t color_max;
} bar_threshold_t;

static bar_threshold_t bar_thresholds[4];

/* Cached dew point threshold colors (parsed once in parse_thresholds) */
static uint32_t dew_safe_color  = 0x3b82f6;  /* blue (safe) */
static uint32_t dew_warn_color  = 0xef4444;  /* red (warning) */

/* Default threshold key per quadrant (fallback when bar_threshold not in config) */
static const char *default_bar_threshold_keys[4] = {
    "cpu_temp",  /* thermal */
    "sqm",       /* sqm */
    "ambient",   /* ambient */
    "amps",      /* power — maps to current draw */
};

/* Quadrant titles */
static const char *quad_titles[4] = {
    "THERMAL", "SQM", "AMBIENT", "POWER"
};

/* Field index mapping: [quadrant][slot] → ALLSKY_F_* index
 *   slot 0 = main, 1 = sub1, 2 = sub2 */
static const int field_map[4][3] = {
    { ALLSKY_F_THERMAL_MAIN, ALLSKY_F_THERMAL_SUB1, ALLSKY_F_THERMAL_SUB2 },
    { ALLSKY_F_SQM_MAIN,     ALLSKY_F_SQM_SUB1,     ALLSKY_F_SQM_SUB2     },
    { ALLSKY_F_AMBIENT_MAIN,  ALLSKY_F_AMBIENT_SUB1,  ALLSKY_F_AMBIENT_SUB2 },
    { ALLSKY_F_POWER_MAIN,    ALLSKY_F_POWER_SUB1,    ALLSKY_F_POWER_SUB2   },
};

/* Quadrant key names in the field_config JSON */
static const char *quad_json_keys[4] = {
    "thermal", "sqm", "ambient", "power"
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

static inline void set_text_if_changed(lv_obj_t *lbl, const char *text) {
    if (strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

static uint32_t parse_hex_color(const char *str, uint32_t fallback) {
    if (!str || str[0] == '\0') return fallback;
    const char *p = str;
    if (*p == '#') p++;
    char *end;
    unsigned long val = strtoul(p, &end, 16);
    if (end == p) return fallback;
    return (uint32_t)val;
}

static uint32_t color_lerp(uint32_t c1, uint32_t c2, float t) {
    if (t <= 0.0f) return c1;
    if (t >= 1.0f) return c2;
    uint8_t r = (uint8_t)((1.0f - t) * ((c1 >> 16) & 0xFF) + t * ((c2 >> 16) & 0xFF));
    uint8_t g = (uint8_t)((1.0f - t) * ((c1 >>  8) & 0xFF) + t * ((c2 >>  8) & 0xFF));
    uint8_t b = (uint8_t)((1.0f - t) * ( c1        & 0xFF) + t * ( c2        & 0xFF));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ── Config parsing ──────────────────────────────────────────────────── */

static void parse_field_config(void) {
    const app_config_t *cfg = app_config_get();
    memset(qcfg, 0, sizeof(qcfg));

    /* Set default bar threshold keys */
    for (int i = 0; i < 4; i++) {
        strncpy(qcfg[i].bar_threshold, default_bar_threshold_keys[i],
                sizeof(qcfg[i].bar_threshold) - 1);
    }

    cJSON *root = cJSON_Parse(cfg->allsky_field_config);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse allsky_field_config JSON");
        return;
    }

    for (int q = 0; q < 4; q++) {
        cJSON *quad = cJSON_GetObjectItem(root, quad_json_keys[q]);
        if (!quad) continue;

        /* main.unit */
        cJSON *main_obj = cJSON_GetObjectItem(quad, "main");
        if (main_obj) {
            cJSON *unit = cJSON_GetObjectItem(main_obj, "unit");
            if (unit && cJSON_IsString(unit)) {
                strncpy(qcfg[q].unit, unit->valuestring, sizeof(qcfg[q].unit) - 1);
            }
        }

        /* sub1: label, suffix */
        cJSON *sub1 = cJSON_GetObjectItem(quad, "sub1");
        if (sub1) {
            cJSON *lbl = cJSON_GetObjectItem(sub1, "label");
            if (lbl && cJSON_IsString(lbl))
                strncpy(qcfg[q].sub1_label, lbl->valuestring, sizeof(qcfg[q].sub1_label) - 1);
            cJSON *sfx = cJSON_GetObjectItem(sub1, "suffix");
            if (sfx && cJSON_IsString(sfx))
                strncpy(qcfg[q].sub1_suffix, sfx->valuestring, sizeof(qcfg[q].sub1_suffix) - 1);
        }

        /* sub2: label, suffix */
        cJSON *sub2 = cJSON_GetObjectItem(quad, "sub2");
        if (sub2) {
            cJSON *lbl = cJSON_GetObjectItem(sub2, "label");
            if (lbl && cJSON_IsString(lbl))
                strncpy(qcfg[q].sub2_label, lbl->valuestring, sizeof(qcfg[q].sub2_label) - 1);
            cJSON *sfx = cJSON_GetObjectItem(sub2, "suffix");
            if (sfx && cJSON_IsString(sfx))
                strncpy(qcfg[q].sub2_suffix, sfx->valuestring, sizeof(qcfg[q].sub2_suffix) - 1);
        }

        /* dot1, dot2 (ambient only but parsed generically) */
        cJSON *dot1 = cJSON_GetObjectItem(quad, "dot1");
        if (dot1) {
            cJSON *on = cJSON_GetObjectItem(dot1, "on_value");
            if (on && cJSON_IsString(on))
                strncpy(qcfg[q].dot1_on_value, on->valuestring, sizeof(qcfg[q].dot1_on_value) - 1);
        }
        cJSON *dot2 = cJSON_GetObjectItem(quad, "dot2");
        if (dot2) {
            cJSON *on = cJSON_GetObjectItem(dot2, "on_value");
            if (on && cJSON_IsString(on))
                strncpy(qcfg[q].dot2_on_value, on->valuestring, sizeof(qcfg[q].dot2_on_value) - 1);
        }

        /* bar_threshold override (optional) */
        cJSON *bt = cJSON_GetObjectItem(quad, "bar_threshold");
        if (bt && cJSON_IsString(bt) && bt->valuestring[0] != '\0') {
            strncpy(qcfg[q].bar_threshold, bt->valuestring, sizeof(qcfg[q].bar_threshold) - 1);
        }
    }

    cJSON_Delete(root);
}

static void parse_thresholds(void) {
    const app_config_t *cfg = app_config_get();
    memset(bar_thresholds, 0, sizeof(bar_thresholds));

    cJSON *root = cJSON_Parse(cfg->allsky_thresholds);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse allsky_thresholds JSON");
        return;
    }

    for (int q = 0; q < 4; q++) {
        const char *key = qcfg[q].bar_threshold;
        if (key[0] == '\0') continue;

        cJSON *thr = cJSON_GetObjectItem(root, key);
        if (!thr) continue;

        cJSON *j_min = cJSON_GetObjectItem(thr, "min");
        cJSON *j_max = cJSON_GetObjectItem(thr, "max");
        cJSON *j_cmin = cJSON_GetObjectItem(thr, "color_min");
        cJSON *j_cmax = cJSON_GetObjectItem(thr, "color_max");

        if (j_min && j_max && cJSON_IsNumber(j_min) && cJSON_IsNumber(j_max)) {
            bar_thresholds[q].valid = true;
            bar_thresholds[q].min = (float)j_min->valuedouble;
            bar_thresholds[q].max = (float)j_max->valuedouble;
            bar_thresholds[q].color_min = parse_hex_color(
                (j_cmin && cJSON_IsString(j_cmin)) ? j_cmin->valuestring : NULL, 0x3b82f6);
            bar_thresholds[q].color_max = parse_hex_color(
                (j_cmax && cJSON_IsString(j_cmax)) ? j_cmax->valuestring : NULL, 0xef4444);
        }
    }

    /* Cache dew_point threshold colors (avoids re-parsing JSON every update) */
    dew_safe_color = 0x3b82f6;
    dew_warn_color = 0xef4444;
    cJSON *dew_thr = cJSON_GetObjectItem(root, "dew_point");
    if (dew_thr) {
        cJSON *cmin = cJSON_GetObjectItem(dew_thr, "color_min");
        cJSON *cmax = cJSON_GetObjectItem(dew_thr, "color_max");
        if (cmin && cJSON_IsString(cmin))
            dew_safe_color = parse_hex_color(cmin->valuestring, dew_safe_color);
        if (cmax && cJSON_IsString(cmax))
            dew_warn_color = parse_hex_color(cmax->valuestring, dew_warn_color);
    }

    cJSON_Delete(root);
}

/* ── Quadrant creation helper ────────────────────────────────────────── */

static void create_quadrant(allsky_quadrant_t *qd, lv_obj_t *parent,
                            int quad_index)
{
    memset(qd, 0, sizeof(*qd));

    qd->box = create_bento_box(parent);
    lv_obj_set_flex_grow(qd->box, 1);
    lv_obj_set_height(qd->box, LV_PCT(100));
    lv_obj_set_flex_flow(qd->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qd->box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(qd->box, 16, 0);
    lv_obj_set_style_pad_row(qd->box, 4, 0);

    /* 1. Title row */
    lv_obj_t *title_row = lv_obj_create(qd->box);
    lv_obj_remove_style_all(title_row);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    qd->lbl_title = lv_label_create(title_row);
    lv_label_set_text(qd->lbl_title, quad_titles[quad_index]);
    lv_obj_set_style_text_font(qd->lbl_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(qd->lbl_title, 2, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(qd->lbl_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    /* Indicator dots for AMBIENT quadrant */
    if (quad_index == 2) {
        lv_obj_t *dot_cont = lv_obj_create(title_row);
        lv_obj_remove_style_all(dot_cont);
        lv_obj_clear_flag(dot_cont, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(dot_cont, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(dot_cont, 6, 0);

        for (int d = 0; d < 2; d++) {
            lv_obj_t *dot = lv_obj_create(dot_cont);
            lv_obj_remove_style_all(dot);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(dot, 14, 14);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 2, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            if (current_theme) {
                int gb = app_config_get()->color_brightness;
                lv_obj_set_style_border_color(dot,
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
            }
            if (d == 0) qd->dot1 = dot;
            else        qd->dot2 = dot;
        }
    }

    /* 2. Main value (48px font scaled to ~54px: 54/48 = 1.125 → 288/256) */
    qd->lbl_main_value = lv_label_create(qd->box);
    lv_obj_set_style_text_font(qd->lbl_main_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_transform_scale(qd->lbl_main_value, 288, 0);
    lv_obj_set_style_transform_pivot_x(qd->lbl_main_value, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(qd->lbl_main_value, LV_PCT(50), 0);
    lv_obj_set_style_text_align(qd->lbl_main_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(qd->lbl_main_value, LV_PCT(100));
    lv_label_set_text(qd->lbl_main_value, "--");
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(qd->lbl_main_value,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* 3. Unit label */
    qd->lbl_unit = lv_label_create(qd->box);
    lv_obj_set_style_text_font(qd->lbl_unit, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(qd->lbl_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(qd->lbl_unit, LV_PCT(100));
    lv_label_set_text(qd->lbl_unit, qcfg[quad_index].unit);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(qd->lbl_unit,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    /* 4. Threshold bar */
    qd->bar = lv_bar_create(qd->box);
    lv_obj_set_width(qd->bar, LV_PCT(90));
    lv_obj_set_height(qd->bar, 8);
    lv_bar_set_range(qd->bar, 0, 1000);
    lv_bar_set_value(qd->bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(qd->bar, 4, 0);
    lv_obj_set_style_radius(qd->bar, 4, LV_PART_INDICATOR);
    if (current_theme) {
        lv_obj_set_style_bg_color(qd->bar,
            lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_opa(qd->bar, LV_OPA_30, 0);
        lv_obj_set_style_bg_color(qd->bar,
            lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(qd->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }

    /* 5. Sub-value row */
    lv_obj_t *sub_row = lv_obj_create(qd->box);
    lv_obj_remove_style_all(sub_row);
    lv_obj_clear_flag(sub_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(sub_row, LV_PCT(100));
    lv_obj_set_height(sub_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sub_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sub_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    qd->lbl_sub1 = lv_label_create(sub_row);
    lv_obj_set_style_text_font(qd->lbl_sub1, &lv_font_montserrat_28, 0);
    lv_label_set_text(qd->lbl_sub1, "--");
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(qd->lbl_sub1,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    qd->lbl_sub2 = lv_label_create(sub_row);
    lv_obj_set_style_text_font(qd->lbl_sub2, &lv_font_montserrat_28, 0);
    lv_label_set_text(qd->lbl_sub2, "--");
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(qd->lbl_sub2,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }
}

/* ── Page creation ───────────────────────────────────────────────────── */

lv_obj_t *allsky_page_create(lv_obj_t *parent) {
    /* Parse config before building widgets */
    parse_field_config();
    parse_thresholds();

    allsky_page = lv_obj_create(parent);
    lv_obj_remove_style_all(allsky_page);
    lv_obj_set_size(allsky_page, SCREEN_SIZE - 2 * OUTER_PADDING,
                    SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_flex_flow(allsky_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(allsky_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(allsky_page, GRID_GAP, 0);

    /* Top row: thermal + SQM */
    lv_obj_t *top_row = lv_obj_create(allsky_page);
    lv_obj_remove_style_all(top_row);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(top_row, LV_PCT(100));
    lv_obj_set_flex_grow(top_row, 1);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(top_row, GRID_GAP, 0);

    create_quadrant(&quads[0], top_row, 0); /* thermal */
    create_quadrant(&quads[1], top_row, 1); /* sqm */

    /* Bottom row: ambient + power */
    lv_obj_t *bot_row = lv_obj_create(allsky_page);
    lv_obj_remove_style_all(bot_row);
    lv_obj_clear_flag(bot_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(bot_row, LV_PCT(100));
    lv_obj_set_flex_grow(bot_row, 1);
    lv_obj_set_flex_flow(bot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(bot_row, GRID_GAP, 0);

    create_quadrant(&quads[2], bot_row, 2); /* ambient */
    create_quadrant(&quads[3], bot_row, 3); /* power */

    return allsky_page;
}

/* ── Format sub-value text ───────────────────────────────────────────── */

static void format_sub_value(char *buf, size_t buf_size,
                             const char *label, const char *value,
                             const char *suffix)
{
    if (value[0] == '\0') {
        buf[0] = '\0';
        return;
    }
    if (label[0] != '\0') {
        snprintf(buf, buf_size, "%s: %s%s", label, value, suffix);
    } else if (suffix[0] != '\0') {
        snprintf(buf, buf_size, "%s%s", value, suffix);
    } else {
        snprintf(buf, buf_size, "%s", value);
    }
}

/* ── Update ──────────────────────────────────────────────────────────── */

void allsky_page_update(const allsky_data_t *data) {
    if (!allsky_page || !data) return;

    int gb = app_config_get()->color_brightness;
    float dew_offset = app_config_get()->allsky_dew_offset;

    for (int q = 0; q < 4; q++) {
        allsky_quadrant_t *qd = &quads[q];

        if (!data->connected) {
            /* Disconnected: clear all values */
            set_text_if_changed(qd->lbl_main_value, "--");
            set_text_if_changed(qd->lbl_sub1, "--");
            set_text_if_changed(qd->lbl_sub2, "--");
            lv_bar_set_value(qd->bar, 0, LV_ANIM_OFF);

            if (current_theme) {
                lv_obj_set_style_text_color(qd->lbl_main_value,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }

            /* Reset dots */
            if (qd->dot1) {
                lv_obj_set_style_bg_opa(qd->dot1, LV_OPA_TRANSP, 0);
            }
            if (qd->dot2) {
                lv_obj_set_style_bg_opa(qd->dot2, LV_OPA_TRANSP, 0);
            }
            continue;
        }

        /* Main value */
        const char *main_val = data->field_values[field_map[q][0]];
        set_text_if_changed(qd->lbl_main_value, main_val[0] ? main_val : "--");

        /* Sub-values */
        char sub_buf[64];
        const char *sub1_val = data->field_values[field_map[q][1]];
        format_sub_value(sub_buf, sizeof(sub_buf),
                         qcfg[q].sub1_label, sub1_val, qcfg[q].sub1_suffix);
        set_text_if_changed(qd->lbl_sub1, sub_buf[0] ? sub_buf : "--");

        const char *sub2_val = data->field_values[field_map[q][2]];
        format_sub_value(sub_buf, sizeof(sub_buf),
                         qcfg[q].sub2_label, sub2_val, qcfg[q].sub2_suffix);
        set_text_if_changed(qd->lbl_sub2, sub_buf[0] ? sub_buf : "--");

        /* Bar + main value color from threshold gradient */
        bar_threshold_t *bt = &bar_thresholds[q];
        if (bt->valid && main_val[0] != '\0') {
            float fval = strtof(main_val, NULL);
            float range = bt->max - bt->min;
            float position = 0.0f;
            if (range > 0.001f) {
                position = (fval - bt->min) / range;
            }
            if (position < 0.0f) position = 0.0f;
            if (position > 1.0f) position = 1.0f;

            lv_bar_set_value(qd->bar, (int)(position * 1000), LV_ANIM_ON);

            /* Gradient color for bar indicator and main value text */
            uint32_t grad_color = color_lerp(bt->color_min, bt->color_max, position);
            uint32_t dimmed = app_config_apply_brightness(grad_color, gb);

            lv_obj_set_style_bg_color(qd->bar,
                lv_color_hex(dimmed), LV_PART_INDICATOR);
            lv_obj_set_style_text_color(qd->lbl_main_value,
                lv_color_hex(dimmed), 0);
        } else {
            /* No valid threshold — use default theme text color */
            lv_bar_set_value(qd->bar, 0, LV_ANIM_OFF);
            if (current_theme) {
                lv_obj_set_style_text_color(qd->lbl_main_value,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }
        }

        /* Dew point special coloring (ambient quadrant, sub2 = dew point) */
        if (q == 2) {
            const char *ambient_str = data->field_values[ALLSKY_F_AMBIENT_MAIN];
            const char *dew_str     = data->field_values[ALLSKY_F_AMBIENT_SUB2];
            if (ambient_str[0] != '\0' && dew_str[0] != '\0') {
                float ambient_val = strtof(ambient_str, NULL);
                float dew_val     = strtof(dew_str, NULL);

                /* Use cached dew_point threshold colors (parsed in parse_thresholds) */
                uint32_t dew_color;
                if (dew_val < ambient_val - dew_offset) {
                    dew_color = dew_safe_color;   /* safe — dew point well below ambient */
                } else {
                    dew_color = dew_warn_color;   /* warning — approaching condensation */
                }
                lv_obj_set_style_text_color(qd->lbl_sub2,
                    lv_color_hex(app_config_apply_brightness(dew_color, gb)), 0);
            }

            /* Indicator dots */
            if (qd->dot1) {
                const char *dot1_val = data->field_values[ALLSKY_F_AMBIENT_DOT1];
                bool dot1_on = (dot1_val[0] != '\0' && qcfg[2].dot1_on_value[0] != '\0'
                                && strcmp(dot1_val, qcfg[2].dot1_on_value) == 0);
                if (dot1_on && current_theme) {
                    lv_obj_set_style_bg_color(qd->dot1,
                        lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)), 0);
                    lv_obj_set_style_bg_opa(qd->dot1, LV_OPA_COVER, 0);
                } else {
                    lv_obj_set_style_bg_opa(qd->dot1, LV_OPA_TRANSP, 0);
                }
            }
            if (qd->dot2) {
                const char *dot2_val = data->field_values[ALLSKY_F_AMBIENT_DOT2];
                bool dot2_on = (dot2_val[0] != '\0' && qcfg[2].dot2_on_value[0] != '\0'
                                && strcmp(dot2_val, qcfg[2].dot2_on_value) == 0);
                if (dot2_on && current_theme) {
                    lv_obj_set_style_bg_color(qd->dot2,
                        lv_color_hex(app_config_apply_brightness(current_theme->progress_color, gb)), 0);
                    lv_obj_set_style_bg_opa(qd->dot2, LV_OPA_COVER, 0);
                } else {
                    lv_obj_set_style_bg_opa(qd->dot2, LV_OPA_TRANSP, 0);
                }
            }
        }
    }
}

/* ── Theme application ───────────────────────────────────────────────── */

void allsky_page_apply_theme(void) {
    if (!allsky_page || !current_theme) return;

    int gb = app_config_get()->color_brightness;

    for (int q = 0; q < 4; q++) {
        allsky_quadrant_t *qd = &quads[q];

        /* Title labels */
        if (qd->lbl_title) {
            lv_obj_set_style_text_color(qd->lbl_title,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }

        /* Unit labels */
        if (qd->lbl_unit) {
            lv_obj_set_style_text_color(qd->lbl_unit,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }

        /* Sub-value labels */
        if (qd->lbl_sub1) {
            lv_obj_set_style_text_color(qd->lbl_sub1,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }
        if (qd->lbl_sub2) {
            lv_obj_set_style_text_color(qd->lbl_sub2,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
        }

        /* Bar background */
        if (qd->bar) {
            lv_obj_set_style_bg_color(qd->bar,
                lv_color_hex(current_theme->bento_border), 0);
            lv_obj_set_style_bg_opa(qd->bar, LV_OPA_30, 0);
        }

        /* Indicator dots border */
        if (qd->dot1) {
            lv_obj_set_style_border_color(qd->dot1,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }
        if (qd->dot2) {
            lv_obj_set_style_border_color(qd->dot2,
                lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
        }
    }

    lv_obj_invalidate(allsky_page);
}

/* ── Refresh config ──────────────────────────────────────────────────── */

void allsky_page_refresh_config(void) {
    parse_field_config();
    parse_thresholds();

    /* Update unit labels from refreshed config */
    for (int q = 0; q < 4; q++) {
        if (quads[q].lbl_unit) {
            set_text_if_changed(quads[q].lbl_unit, qcfg[q].unit);
        }
    }
}
