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

#include "esp_heap_caps.h"
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
    lv_obj_t *dot1;   /* THERMAL only — fan symbol (LV_SYMBOL_REFRESH) */
    lv_obj_t *dot2;   /* AMBIENT only — heater symbol (LV_SYMBOL_CHARGE) */
    /* Cached style values — only call lv_obj_set_style_* when changed to avoid
     * unnecessary LVGL invalidations that trigger expensive full redraws. */
    uint32_t cached_main_color;
    uint32_t cached_sub1_color;
    uint32_t cached_sub2_color;
    uint32_t cached_dot1_color;
    uint32_t cached_dot2_color;
    uint32_t cached_bar_color;
    int      cached_bar_value;
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
} quadrant_config_t;

static quadrant_config_t qcfg[4];

/* ── Per-field threshold cache ───────────────────────────────────────── */

typedef struct {
    bool  valid;
    float min;
    float max;
    uint32_t color_min;
    uint32_t color_max;
} bar_threshold_t;

/* Indexed by [quad][field_type]: 0=main, 1=sub1, 2=sub2 */
#define FIELD_MAIN 0
#define FIELD_SUB1 1
#define FIELD_SUB2 2
static bar_threshold_t field_thresholds[4][3];

/* Cached dew point threshold colors (parsed once in parse_thresholds) */
static uint32_t dew_safe_color  = 0x3b82f6;  /* blue (safe) */
static uint32_t dew_warn_color  = 0xef4444;  /* red (warning) */

/* Field names for building threshold lookup keys */
static const char *field_names[3] = { "main", "sub1", "sub2" };

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

/* Forward declaration — defined below in Helpers section */
static inline bool is_red_theme(void);

/* ── Moon phase canvas ───────────────────────────────────────────────── */

#define MOON_ICON_SIZE 64

static lv_obj_t *moon_canvas = NULL;
static uint8_t  *moon_canvas_buf = NULL;
static float     cached_moon_illum = -1.0f;

/**
 * Render a small moon phase icon on the canvas.
 * Uses the terminator equation: for a point at (dx, dy) on the disk,
 * the pixel is illuminated if dx > (1 - 2*f) * sqrt(R² - dy²),
 * where f = illumination fraction (0–1).
 */
static void render_moon_phase(float illum_pct) {
    if (!moon_canvas || !moon_canvas_buf) return;
    if (fabsf(illum_pct - cached_moon_illum) < 0.5f) return;
    cached_moon_illum = illum_pct;

    const int S = MOON_ICON_SIZE;
    const float R  = (S - 2) / 2.0f;
    const float cx = (S - 1) / 2.0f;
    const float cy = (S - 1) / 2.0f;
    const float f  = illum_pct / 100.0f;
    const float AA = 1.2f; /* antialiasing width in pixels */

    /* Moon colors — red-only for red themes, warm yellow/white otherwise */
    float lit_r, lit_g, lit_b;    /* lit portion base color */
    float dark_r, dark_g, dark_b; /* dark portion base color */
    if (is_red_theme()) {
        /* Use theme text_color for lit portion, bento_border for dark */
        uint32_t tc = current_theme->text_color;
        lit_r = (float)((tc >> 16) & 0xFF);
        lit_g = (float)((tc >>  8) & 0xFF);
        lit_b = (float)( tc        & 0xFF);
        uint32_t dc = current_theme->bento_border;
        dark_r = (float)((dc >> 16) & 0xFF);
        dark_g = (float)((dc >>  8) & 0xFF);
        dark_b = (float)( dc        & 0xFF);
    } else {
        lit_r = 220.0f; lit_g = 200.0f; lit_b = 160.0f;
        dark_r = 24.0f; dark_g = 24.0f; dark_b = 42.0f;
    }

    lv_canvas_fill_bg(moon_canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);

    for (int py = 0; py < S; py++) {
        float dy  = py - cy;
        float dy2 = dy * dy;

        for (int px = 0; px < S; px++) {
            float dx = px - cx;
            float r2 = dx * dx + dy2;
            float dist = sqrtf(r2);

            /* Outer edge alpha — smooth antialiased circle */
            float edge_alpha = R - dist;
            if (edge_alpha < 0.0f) continue;        /* fully outside */
            if (edge_alpha > AA) edge_alpha = 1.0f;
            else edge_alpha /= AA;                   /* fade 0→1 over AA pixels */

            /* Terminator position at this scanline */
            float max_x2 = R * R - dy2;
            float semi_w = (max_x2 > 0.0f) ? sqrtf(max_x2) : 0.0f;
            float term_x = (1.0f - 2.0f * f) * semi_w;

            /* Terminator blend — smooth transition across AA pixels */
            float lit = (dx - term_x) / AA;
            if (lit < 0.0f) lit = 0.0f;
            if (lit > 1.0f) lit = 1.0f;

            /* Limb darkening */
            float limb = 1.0f - 0.25f * (r2 / (R * R));

            /* Blend lit and dark colors */
            uint8_t rv = (uint8_t)(lit * lit_r * limb + (1.0f - lit) * dark_r);
            uint8_t gv = (uint8_t)(lit * lit_g * limb + (1.0f - lit) * dark_g);
            uint8_t bv = (uint8_t)(lit * lit_b * limb + (1.0f - lit) * dark_b);

            /* Final opacity: outer edge AA * base opacity (dark side is dimmer) */
            float base_opa = lit * 1.0f + (1.0f - lit) * 0.3f;
            uint8_t opa = (uint8_t)(edge_alpha * base_opa * 255.0f);

            lv_canvas_set_px(moon_canvas, px, py,
                             lv_color_make(rv, gv, bv), opa);
        }
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static inline void set_text_if_changed(lv_obj_t *lbl, const char *text) {
    if (strcmp(lv_label_get_text(lbl), text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

static inline void set_color_if_changed(lv_obj_t *obj, uint32_t *cached, uint32_t color) {
    if (*cached != color) {
        *cached = color;
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    }
}

static inline void set_bar_color_if_changed(lv_obj_t *bar, uint32_t *cached, uint32_t color) {
    if (*cached != color) {
        *cached = color;
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    }
}

static inline void set_bar_value_if_changed(lv_obj_t *bar, int *cached, int value) {
    if (*cached != value) {
        *cached = value;
        lv_bar_set_value(bar, value, LV_ANIM_OFF);
    }
}

static inline bool is_red_theme(void) {
    return current_theme &&
           (strcmp(current_theme->name, "Red Night") == 0 ||
            strcmp(current_theme->name, "Night Vision Red") == 0);
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

    }

    cJSON_Delete(root);
}

static void parse_thresholds(void) {
    const app_config_t *cfg = app_config_get();
    memset(field_thresholds, 0, sizeof(field_thresholds));

    cJSON *root = cJSON_Parse(cfg->allsky_thresholds);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse allsky_thresholds JSON");
        return;
    }

    /* Parse per-field thresholds using positional keys: {quad}_{field} */
    for (int q = 0; q < 4; q++) {
        for (int f = 0; f < 3; f++) {
            char key[32];
            snprintf(key, sizeof(key), "%s_%s", quad_json_keys[q], field_names[f]);

            cJSON *thr = cJSON_GetObjectItem(root, key);
            if (!thr) continue;

            cJSON *j_min  = cJSON_GetObjectItem(thr, "min");
            cJSON *j_max  = cJSON_GetObjectItem(thr, "max");
            cJSON *j_cmin = cJSON_GetObjectItem(thr, "color_min");
            cJSON *j_cmax = cJSON_GetObjectItem(thr, "color_max");

            if (j_min && j_max && cJSON_IsNumber(j_min) && cJSON_IsNumber(j_max)) {
                field_thresholds[q][f].valid = true;
                field_thresholds[q][f].min = (float)j_min->valuedouble;
                field_thresholds[q][f].max = (float)j_max->valuedouble;
                field_thresholds[q][f].color_min = parse_hex_color(
                    (j_cmin && cJSON_IsString(j_cmin)) ? j_cmin->valuestring : NULL, 0x3b82f6);
                field_thresholds[q][f].color_max = parse_hex_color(
                    (j_cmax && cJSON_IsString(j_cmax)) ? j_cmax->valuestring : NULL, 0xef4444);
            }
        }
    }

    /* Override threshold gradient colors for red-spectrum themes */
    if (is_red_theme()) {
        for (int q = 0; q < 4; q++) {
            for (int f = 0; f < 3; f++) {
                if (field_thresholds[q][f].valid) {
                    field_thresholds[q][f].color_min = current_theme->progress_color;
                    field_thresholds[q][f].color_max = current_theme->text_color;
                }
            }
        }
    }

    /* Cache dew_point threshold colors from ambient_sub2 */
    dew_safe_color = 0x3b82f6;
    dew_warn_color = 0xef4444;
    bar_threshold_t *dew_bt = &field_thresholds[2][FIELD_SUB2]; /* ambient sub2 */
    if (dew_bt->valid) {
        dew_safe_color = dew_bt->color_min;
        dew_warn_color = dew_bt->color_max;
    }

    /* Override dew colors for red-spectrum themes */
    if (is_red_theme()) {
        dew_safe_color  = current_theme->progress_color;
        dew_warn_color  = current_theme->text_color;
    }

    cJSON_Delete(root);
}

/* ── Superscript font fallback ───────────────────────────────────────── */

extern const lv_font_t lv_font_superscript_24;

/* Mutable copy of montserrat_24 with superscript fallback attached.
 * The built-in lv_font_montserrat_24 lives in const flash (DROM) on ESP32-P4
 * so we cannot modify its fallback pointer in-place. */
static lv_font_t montserrat_24_super;
static bool montserrat_24_super_init = false;

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

    /* Fan indicator symbol — top-right of THERMAL quadrant */
    if (quad_index == 0) {
        qd->dot1 = lv_label_create(title_row);
        lv_label_set_text(qd->dot1, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_font(qd->dot1, &lv_font_montserrat_22, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(qd->dot1,
                lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
        }
    }

    /* Moon phase canvas — floating overlay on SQM quadrant (top-right) */
    if (quad_index == 1) {
        if (!moon_canvas_buf) {
            moon_canvas_buf = heap_caps_malloc(
                LV_CANVAS_BUF_SIZE(MOON_ICON_SIZE, MOON_ICON_SIZE,
                                   32, LV_DRAW_BUF_STRIDE_ALIGN),
                MALLOC_CAP_SPIRAM);
        }
        if (moon_canvas_buf) {
            moon_canvas = lv_canvas_create(qd->box);
            lv_canvas_set_buffer(moon_canvas, moon_canvas_buf,
                                 MOON_ICON_SIZE, MOON_ICON_SIZE,
                                 LV_COLOR_FORMAT_ARGB8888);
            lv_canvas_fill_bg(moon_canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);
            lv_obj_add_flag(moon_canvas, LV_OBJ_FLAG_FLOATING);
            lv_obj_align(moon_canvas, LV_ALIGN_TOP_RIGHT, -4, 4);
            cached_moon_illum = -1.0f;
        }
    }

    /* GPS indicator symbol — top-right of POWER quadrant */
    if (quad_index == 3) {
        qd->dot1 = lv_label_create(title_row);
        lv_label_set_text(qd->dot1, LV_SYMBOL_GPS);
        lv_obj_set_style_text_font(qd->dot1, &lv_font_montserrat_22, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(qd->dot1,
                lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
        }
    }

    /* Heater indicator symbol — top-right of AMBIENT quadrant */
    if (quad_index == 2) {
        qd->dot2 = lv_label_create(title_row);
        lv_label_set_text(qd->dot2, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_font(qd->dot2, &lv_font_montserrat_22, 0);
        if (current_theme) {
            int gb = app_config_get()->color_brightness;
            lv_obj_set_style_text_color(qd->dot2,
                lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
        }
    }

    /* 2. Main value (48px font — no transform_scale to avoid expensive software rendering) */
    qd->lbl_main_value = lv_label_create(qd->box);
    lv_obj_set_style_text_font(qd->lbl_main_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(qd->lbl_main_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(qd->lbl_main_value, LV_PCT(100));
    lv_label_set_text(qd->lbl_main_value, "--");
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(qd->lbl_main_value,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* 3. Unit label (uses mutable font copy with superscript fallback) */
    qd->lbl_unit = lv_label_create(qd->box);
    lv_obj_set_style_text_font(qd->lbl_unit, &montserrat_24_super, 0);
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
    lv_obj_set_style_text_font(qd->lbl_sub1,
        (quad_index == 1) ? &lv_font_montserrat_22 : &lv_font_montserrat_28, 0);
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
    /* Create mutable font copy with superscript fallback for unit labels (², etc.) */
    if (!montserrat_24_super_init) {
        memcpy(&montserrat_24_super, &lv_font_montserrat_24, sizeof(lv_font_t));
        montserrat_24_super.fallback = &lv_font_superscript_24;
        montserrat_24_super_init = true;
    }

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
            set_bar_value_if_changed(qd->bar, &qd->cached_bar_value, 0);

            if (current_theme) {
                uint32_t tc = app_config_apply_brightness(current_theme->text_color, gb);
                set_color_if_changed(qd->lbl_main_value, &qd->cached_main_color, tc);
            }

            /* Clear moon canvas on disconnect */
            if (q == 1 && moon_canvas && cached_moon_illum >= 0.0f) {
                lv_canvas_fill_bg(moon_canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);
                cached_moon_illum = -1.0f;
            }

            /* Reset indicator symbols to dim */
            if (qd->dot1 && current_theme) {
                uint32_t dc = app_config_apply_brightness(current_theme->bento_border, gb);
                set_color_if_changed(qd->dot1, &qd->cached_dot1_color, dc);
            }
            if (qd->dot2 && current_theme) {
                uint32_t dc = app_config_apply_brightness(current_theme->bento_border, gb);
                set_color_if_changed(qd->dot2, &qd->cached_dot2_color, dc);
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

        /* SQM quadrant: render moon phase canvas from illumination % */
        if (q == 1 && sub2_val[0] != '\0') {
            render_moon_phase(strtof(sub2_val, NULL));
        }

        /* Bar + main value color from threshold gradient */
        bar_threshold_t *bt = &field_thresholds[q][FIELD_MAIN];
        if (bt->valid && main_val[0] != '\0') {
            float fval = strtof(main_val, NULL);
            float range = bt->max - bt->min;
            float position = 0.0f;
            if (range > 0.001f) {
                position = (fval - bt->min) / range;
            }
            if (position < 0.0f) position = 0.0f;
            if (position > 1.0f) position = 1.0f;

            set_bar_value_if_changed(qd->bar, &qd->cached_bar_value, (int)(position * 1000));

            /* Gradient color for bar indicator and main value text */
            uint32_t grad_color = color_lerp(bt->color_min, bt->color_max, position);
            uint32_t dimmed = app_config_apply_brightness(grad_color, gb);

            set_bar_color_if_changed(qd->bar, &qd->cached_bar_color, dimmed);
            set_color_if_changed(qd->lbl_main_value, &qd->cached_main_color, dimmed);
        } else {
            /* No valid threshold — use default theme text color */
            set_bar_value_if_changed(qd->bar, &qd->cached_bar_value, 0);
            if (current_theme) {
                uint32_t tc = app_config_apply_brightness(current_theme->text_color, gb);
                set_color_if_changed(qd->lbl_main_value, &qd->cached_main_color, tc);
            }
        }

        /* Sub1 gradient color from per-field threshold */
        bar_threshold_t *bt_s1 = &field_thresholds[q][FIELD_SUB1];
        if (bt_s1->valid && sub1_val[0] != '\0') {
            float fval = strtof(sub1_val, NULL);
            float range = bt_s1->max - bt_s1->min;
            float pos = 0.0f;
            if (range > 0.001f) pos = (fval - bt_s1->min) / range;
            if (pos < 0.0f) pos = 0.0f;
            if (pos > 1.0f) pos = 1.0f;
            uint32_t sc = color_lerp(bt_s1->color_min, bt_s1->color_max, pos);
            set_color_if_changed(qd->lbl_sub1, &qd->cached_sub1_color,
                                 app_config_apply_brightness(sc, gb));
        }

        /* Sub2 color: dew point special logic for ambient, gradient for others */
        if (q == 2) {
            /* Dew point special coloring (ambient quadrant, sub2 = dew point) */
            const char *ambient_str = data->field_values[ALLSKY_F_AMBIENT_MAIN];
            const char *dew_str     = data->field_values[ALLSKY_F_AMBIENT_SUB2];
            if (ambient_str[0] != '\0' && dew_str[0] != '\0') {
                float ambient_val = strtof(ambient_str, NULL);
                float dew_val     = strtof(dew_str, NULL);

                uint32_t dew_color;
                if (dew_val < ambient_val - dew_offset) {
                    dew_color = dew_safe_color;
                } else {
                    dew_color = dew_warn_color;
                }
                set_color_if_changed(qd->lbl_sub2, &qd->cached_sub2_color,
                                     app_config_apply_brightness(dew_color, gb));
            }
        } else {
            bar_threshold_t *bt_s2 = &field_thresholds[q][FIELD_SUB2];
            if (bt_s2->valid && sub2_val[0] != '\0') {
                float fval = strtof(sub2_val, NULL);
                float range = bt_s2->max - bt_s2->min;
                float pos = 0.0f;
                if (range > 0.001f) pos = (fval - bt_s2->min) / range;
                if (pos < 0.0f) pos = 0.0f;
                if (pos > 1.0f) pos = 1.0f;
                uint32_t sc = color_lerp(bt_s2->color_min, bt_s2->color_max, pos);
                set_color_if_changed(qd->lbl_sub2, &qd->cached_sub2_color,
                                     app_config_apply_brightness(sc, gb));
            }
        }

        /* Dot 1 indicator: thermal=fan (ambient config), power=GPS (sqm config) */
        if (qd->dot1 && current_theme) {
            const char *dot1_val;
            bool dot1_on;
            if (q == 3) {
                /* GPS indicator on POWER quadrant — uses SQM dot1 config */
                dot1_val = data->field_values[ALLSKY_F_SQM_DOT1];
                dot1_on = (dot1_val[0] != '\0' && qcfg[1].dot1_on_value[0] != '\0'
                           && strcmp(dot1_val, qcfg[1].dot1_on_value) == 0);
            } else {
                /* Fan indicator on THERMAL quadrant — uses ambient config */
                dot1_val = data->field_values[ALLSKY_F_AMBIENT_DOT1];
                dot1_on = (dot1_val[0] != '\0' && qcfg[2].dot1_on_value[0] != '\0'
                           && strcmp(dot1_val, qcfg[2].dot1_on_value) == 0);
            }
            uint32_t c = dot1_on ? current_theme->progress_color
                                 : current_theme->bento_border;
            set_color_if_changed(qd->dot1, &qd->cached_dot1_color,
                                 app_config_apply_brightness(c, gb));
        }

        /* Heater indicator symbol (on ambient quadrant) */
        if (qd->dot2 && current_theme) {
            const char *dot2_val = data->field_values[ALLSKY_F_AMBIENT_DOT2];
            bool dot2_on = (dot2_val[0] != '\0' && qcfg[2].dot2_on_value[0] != '\0'
                            && strcmp(dot2_val, qcfg[2].dot2_on_value) == 0);
            uint32_t c = dot2_on ? current_theme->progress_color
                                 : current_theme->bento_border;
            set_color_if_changed(qd->dot2, &qd->cached_dot2_color,
                                 app_config_apply_brightness(c, gb));
        }
    }
}

/* ── Theme application ───────────────────────────────────────────────── */

void allsky_page_apply_theme(void) {
    if (!allsky_page || !current_theme) return;

    /* Re-derive threshold colors for the new theme */
    parse_thresholds();

    /* Force moon canvas re-render with new theme colors */
    cached_moon_illum = -1.0f;

    /* Invalidate all cached colors so the next update re-applies everything */
    for (int q = 0; q < 4; q++) {
        quads[q].cached_main_color = UINT32_MAX;
        quads[q].cached_sub1_color = UINT32_MAX;
        quads[q].cached_sub2_color = UINT32_MAX;
        quads[q].cached_dot1_color = UINT32_MAX;
        quads[q].cached_dot2_color = UINT32_MAX;
        quads[q].cached_bar_color  = UINT32_MAX;
        quads[q].cached_bar_value  = -1;
    }

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

        /* Indicator symbols — reset to dim on theme change */
        if (qd->dot1) {
            lv_obj_set_style_text_color(qd->dot1,
                lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
        }
        if (qd->dot2) {
            lv_obj_set_style_text_color(qd->dot2,
                lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
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
