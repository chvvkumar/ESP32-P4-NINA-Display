/**
 * @file nina_clock.c
 * @brief Clock page — editorial dark clockface with weather data.
 *
 * Typography-driven layout with two thin horizontal rules framing a massive
 * centered time display. Weather data split above and below the time.
 * 10-hour forecast bar chart at the bottom. Fixed editorial color palette.
 */

#include "nina_clock.h"
#include "nina_dashboard_internal.h"
#include "nina_idle_indicator.h"
#include "display_defs.h"
#include "app_config.h"
#include "weather_client.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#include "esp_log.h"
static const char *TAG = "clock_ui";

/* ── Editorial color palette (fixed, not theme-dependent) ────────────── */

#define CLK_BG          0x121110  /* Warm near-black */
#define CLK_PRIMARY     0xE8E2D4  /* Warm cream (time, temp) */
#define CLK_SECONDARY   0xC8C2B4  /* Muted cream (stat values) */
#define CLK_TERTIARY    0x908A7E  /* Warm gray (date) */
#define CLK_DIM         0x6A6458  /* Warm gray (labels, AM/PM, hi/lo) */
#define CLK_CONDITION   0x908A7E  /* Warm gray (condition text) */
#define CLK_RULE        0x2A2622  /* Barely visible warm divider */
#define CLK_BAR_LABEL   0x5A5448  /* Forecast bar time labels */
#define CLK_BAR_HOT     0xB86A3A  /* >75 F / >24 C */
#define CLK_BAR_WARM    0xA8924A  /* 65-75 F / 18-24 C */
#define CLK_BAR_COOL    0x5A7A5A  /* 55-65 F / 13-18 C */
#define CLK_BAR_COLD    0x4A6A7A  /* <55 F / <13 C */

#define CLOCK_PADDING   40
#define FORECAST_BARS   10
#define BAR_WIDTH       22
#define BAR_GAP         4
#define BAR_MIN_H       21
#define BAR_MAX_H       63

/* ── Font declarations (generated .c files in main/ui/) ──────────────── */

extern const lv_font_t lv_font_playfair_228;
extern const lv_font_t lv_font_playfair_90;
extern const lv_font_t lv_font_overpass_27;
extern const lv_font_t lv_font_overpass_16;

/* ── Static widget pointers ──────────────────────────────────────────── */

static lv_obj_t *clock_root = NULL;

/* Header — date stack */
static lv_obj_t *lbl_day  = NULL;
static lv_obj_t *lbl_date = NULL;
static lv_obj_t *lbl_year = NULL;

/* Header — weather stack */
static lv_obj_t *lbl_temp = NULL;
static lv_obj_t *lbl_deg  = NULL;
static lv_obj_t *lbl_cond = NULL;
static lv_obj_t *lbl_hilo = NULL;

/* Time */
static lv_obj_t *lbl_time = NULL;
static lv_obj_t *lbl_ampm = NULL;

/* Stats strip */
static lv_obj_t *lbl_humid_val = NULL;
static lv_obj_t *lbl_dew_val   = NULL;
static lv_obj_t *lbl_wind_val  = NULL;
static lv_obj_t *lbl_uv_val   = NULL;

/* Forecast */
static lv_obj_t *forecast_row  = NULL;
static lv_obj_t *forecast_bars[FORECAST_BARS];
static lv_obj_t *forecast_lbls[FORECAST_BARS];

/* Dividers — captured so apply_theme can recolor them */
#define CLK_RULE_COUNT 2
#define CLK_VDIV_COUNT 3
static lv_obj_t *rules[CLK_RULE_COUNT];
static int rule_count = 0;
static lv_obj_t *vdividers[CLK_VDIV_COUNT];
static int vdivider_count = 0;

/* Last-known metric flag, so apply_theme can recolor forecast bars
 * (which depend on temperature) without live weather data on hand. */
static bool last_is_metric = false;

/* Timer */
static lv_timer_t *clock_timer = NULL;

/* ── Helpers ─────────────────────────────────────────────────────────── */

/**
 * Make a transparent, non-scrollable container.
 */
static lv_obj_t *make_container(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

/**
 * Create a label with font, color, and letter spacing.
 */
static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                             uint32_t color_hex, int letter_space,
                             const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex), 0);
    if (letter_space != 0) {
        lv_obj_set_style_text_letter_space(lbl, letter_space, 0);
    }
    lv_label_set_text(lbl, text);
    return lbl;
}

/**
 * Create a 1px horizontal rule across full width.
 */
static lv_obj_t *make_rule(lv_obj_t *parent) {
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(CLK_RULE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    if (rule_count < CLK_RULE_COUNT) {
        rules[rule_count++] = rule;
    }
    return rule;
}

/**
 * Convert temperature to Fahrenheit for bar color thresholds.
 */
static float to_fahrenheit(float temp, bool is_metric) {
    if (is_metric) return temp * 9.0f / 5.0f + 32.0f;
    return temp;
}

/**
 * Get bar color based on temperature (compared in Fahrenheit).
 *
 * When @p red_night is true, every band maps to a shade of pure red whose
 * brightness rises with temperature (cold = dark red, hot = bright red).
 * No green/blue/orange under Red Night. Otherwise the fixed editorial
 * green/blue/orange/olive palette is returned unchanged.
 */
static uint32_t bar_color_for_temp(float temp_f, bool red_night) {
    if (red_night) {
        if (temp_f > 75.0f)  return 0xFF0000;  /* hot  — bright red */
        if (temp_f >= 65.0f) return 0xC00000;  /* warm — medium-bright red */
        if (temp_f >= 55.0f) return 0x800000;  /* cool — medium-dark red */
        return 0x500000;                        /* cold — dark red */
    }
    if (temp_f > 75.0f) return CLK_BAR_HOT;
    if (temp_f >= 65.0f) return CLK_BAR_WARM;
    if (temp_f >= 55.0f) return CLK_BAR_COOL;
    return CLK_BAR_COLD;
}

/**
 * Create a stat column (value + label) for the stats strip.
 */
static lv_obj_t *make_stat_col(lv_obj_t *parent, const char *value_text,
                                const char *label_text, lv_obj_t **out_val) {
    lv_obj_t *col = make_container(parent);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(col, 2, 0);

    *out_val = make_label(col, &lv_font_overpass_27, CLK_SECONDARY, 0, value_text);
    make_label(col, &lv_font_overpass_16, CLK_DIM, 1, label_text);

    return col;
}

/**
 * Create a 1px vertical divider for the stats strip.
 */
static lv_obj_t *make_vdivider(lv_obj_t *parent) {
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 1, 40);
    lv_obj_set_style_bg_color(div, lv_color_hex(CLK_RULE), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_remove_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
    if (vdivider_count < CLK_VDIV_COUNT) {
        vdividers[vdivider_count++] = div;
    }
    return div;
}

/* ── Timer callback ──────────────────────────────────────────────────── */

static void clock_timer_cb(lv_timer_t *timer) {
    (void)timer;
    clock_page_update();

    /* Re-align timer to fire at the top of the next minute.
     * This corrects for any drift and ensures the display
     * always updates exactly when the minute changes. */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    uint32_t ms_to_next_min = (uint32_t)(60 - tm_now.tm_sec) * 1000;
    if (ms_to_next_min < 1000) ms_to_next_min = 60000;  /* Just ticked, wait full minute */
    lv_timer_set_period(clock_timer, ms_to_next_min);
}

/* ── Page creation ───────────────────────────────────────────────────── */

lv_obj_t *clock_page_create(lv_obj_t *parent) {
    /* Reset divider registries so re-creation cannot overflow the arrays */
    rule_count = 0;
    vdivider_count = 0;

    /* Root — full screen, own background, overrides parent padding */
    clock_root = lv_obj_create(parent);
    lv_obj_set_size(clock_root, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(clock_root, -OUTER_PADDING, -OUTER_PADDING);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(CLK_BG), 0);
    lv_obj_set_style_bg_opa(clock_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(clock_root, 0, 0);
    lv_obj_set_style_radius(clock_root, 0, 0);
    lv_obj_set_style_pad_all(clock_root, CLOCK_PADDING, 0);
    lv_obj_set_flex_flow(clock_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_root, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(clock_root, 0, 0);  /* justify-between handles spacing */
    lv_obj_remove_flag(clock_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Header row (date left, weather right) ── */
    lv_obj_t *header_row = make_container(clock_root);
    lv_obj_set_size(header_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Date stack (left) */
    lv_obj_t *date_stack = make_container(header_row);
    lv_obj_set_size(date_stack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(date_stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(date_stack, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(date_stack, 0, 0);

    lbl_day  = make_label(date_stack, &lv_font_overpass_27, CLK_TERTIARY, 2, "---");
    lbl_date = make_label(date_stack, &lv_font_overpass_27, CLK_TERTIARY, 2, "---");
    lbl_year = make_label(date_stack, &lv_font_overpass_27, CLK_TERTIARY, 2, "---");

    /* Weather stack (right) */
    lv_obj_t *weather_stack = make_container(header_row);
    lv_obj_set_size(weather_stack, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(weather_stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(weather_stack, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(weather_stack, 2, 0);

    /* Temperature row: number + degree symbol side by side, aligned to top */
    lv_obj_t *temp_row = make_container(weather_stack);
    lv_obj_set_size(temp_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(temp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(temp_row, 0, 0);

    lbl_temp = make_label(temp_row, &lv_font_playfair_90, CLK_PRIMARY, 0, "");
    /* Degree symbol using overpass font — aligns inline at top of digits */
    lbl_deg = make_label(temp_row, &lv_font_overpass_16, CLK_PRIMARY, 0, "\xc2\xb0");
    lv_obj_set_style_pad_top(lbl_deg, 2, 0);
    lv_obj_add_flag(lbl_deg, LV_OBJ_FLAG_HIDDEN);  /* Hidden until weather data */

    lbl_cond = make_label(weather_stack, &lv_font_overpass_27, CLK_CONDITION, 1, "--");
    lv_obj_set_style_pad_top(lbl_cond, 4, 0);
    lbl_hilo = make_label(weather_stack, &lv_font_overpass_27, CLK_DIM, 1, "--");

    /* ── Rule 1 ── */
    make_rule(clock_root);

    /* ── Time container (centered) ── */
    lv_obj_t *time_container = make_container(clock_root);
    lv_obj_set_size(time_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(time_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_time = make_label(time_container, &lv_font_playfair_228, CLK_PRIMARY, -4, "");
    lv_obj_set_style_text_line_space(lbl_time, 0, 0);
    lbl_ampm = make_label(time_container, &lv_font_overpass_16, CLK_DIM, 3, "");
    lv_obj_set_style_pad_top(lbl_ampm, 4, 0);

    /* ── Rule 2 ── */
    make_rule(clock_root);

    /* ── Stats strip (4 columns with vertical dividers) ── */
    lv_obj_t *stats_strip = make_container(clock_root);
    lv_obj_set_size(stats_strip, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(stats_strip, 8, 0);

    make_stat_col(stats_strip, "--", "HUMID",  &lbl_humid_val);
    make_vdivider(stats_strip);
    make_stat_col(stats_strip, "--", "DEW PT", &lbl_dew_val);
    make_vdivider(stats_strip);
    make_stat_col(stats_strip, "--", "WIND",   &lbl_wind_val);
    make_vdivider(stats_strip);
    make_stat_col(stats_strip, "--", "UV IDX", &lbl_uv_val);

    /* ── Forecast row (10 bars + labels, hidden until data arrives) ── */
    forecast_row = make_container(clock_root);
    lv_obj_set_size(forecast_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(forecast_row, BAR_GAP, 0);
    lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < FORECAST_BARS; i++) {
        /* Per-bar column: bar on top, label below */
        lv_obj_t *bar_col = make_container(forecast_row);
        lv_obj_set_size(bar_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bar_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bar_col, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(bar_col, 4, 0);

        /* Bar rectangle */
        lv_obj_t *bar = lv_obj_create(bar_col);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, BAR_WIDTH, BAR_MIN_H);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CLK_BAR_COOL), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        forecast_bars[i] = bar;

        /* Hour label */
        forecast_lbls[i] = make_label(bar_col, &lv_font_overpass_16,
                                       CLK_BAR_LABEL, 0, "--");
    }

    /* ── Idle connection indicator (floating overlay at top center) ── */
    nina_idle_indicator_create(clock_root, LV_ALIGN_TOP_MID, true);

    /* Compute delay until the top of the next minute so the first
     * real tick aligns with the minute change.  Fire once immediately
     * for the initial draw, then the callback re-aligns each tick. */
    time_t t0 = time(NULL);
    struct tm tm0;
    localtime_r(&t0, &tm0);
    uint32_t init_ms = (uint32_t)(60 - tm0.tm_sec) * 1000;
    if (init_ms < 1000) init_ms = 60000;

    clock_timer = lv_timer_create(clock_timer_cb, init_ms, NULL);
    lv_timer_set_repeat_count(clock_timer, -1);
    lv_timer_ready(clock_timer);  /* Fire immediately for initial draw */

    /* Start paused — timer runs only when the page is visible */
    lv_timer_pause(clock_timer);

    ESP_LOGI(TAG, "Clock page created");
    return clock_root;
}

/* ── Update logic ────────────────────────────────────────────────────── */

void clock_page_update(void) {
    if (!clock_root) return;

    const app_config_t *cfg = app_config_get();
    bool is_24h = (cfg->weather_time_format == 1);
    bool is_metric = (cfg->weather_units == 1);
    last_is_metric = is_metric;
    bool red_night = theme_is_red_night(current_theme);

    /* ── Time & date ── */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    /* Time string */
    char time_buf[16];
    if (is_24h) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    } else {
        int h12 = tm_now.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(time_buf, sizeof(time_buf), "%d:%02d", h12, tm_now.tm_min);
    }
    lv_label_set_text(lbl_time, time_buf);

    /* AM/PM */
    if (is_24h) {
        lv_obj_add_flag(lbl_ampm, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(lbl_ampm, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_ampm, tm_now.tm_hour < 12 ? "A M" : "P M");
    }

    /* Date labels — uppercase day name */
    static const char *days[] = {
        "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
        "THURSDAY", "FRIDAY", "SATURDAY"
    };
    static const char *months[] = {
        "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
        "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
    };

    if (tm_now.tm_wday >= 0 && tm_now.tm_wday < 7) {
        lv_label_set_text(lbl_day, days[tm_now.tm_wday]);
    }

    char date_buf[32];
    if (tm_now.tm_mon >= 0 && tm_now.tm_mon < 12) {
        snprintf(date_buf, sizeof(date_buf), "%s %d", months[tm_now.tm_mon], tm_now.tm_mday);
    } else {
        snprintf(date_buf, sizeof(date_buf), "--- %d", tm_now.tm_mday);
    }
    lv_label_set_text(lbl_date, date_buf);

    char year_buf[16];
    snprintf(year_buf, sizeof(year_buf), "%d", tm_now.tm_year + 1900);
    lv_label_set_text(lbl_year, year_buf);

    /* ── Weather data ── */
    weather_data_t wd;
    weather_client_get_data(&wd);

    if (!wd.valid) {
        /* No weather data — show placeholders, hide forecast */
        lv_label_set_text(lbl_temp, "");
        lv_obj_add_flag(lbl_deg, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_cond, "--");
        lv_label_set_text(lbl_hilo, "--");
        lv_label_set_text(lbl_humid_val, "--");
        lv_label_set_text(lbl_dew_val, "--");
        lv_label_set_text(lbl_wind_val, "--");
        lv_label_set_text(lbl_uv_val, "--");
        lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Temperature (current) — degree symbol is a separate label */
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%.0f", wd.temp_current);
    lv_label_set_text(lbl_temp, temp_buf);
    lv_obj_remove_flag(lbl_deg, LV_OBJ_FLAG_HIDDEN);

    /* Condition — uppercase */
    char cond_upper[sizeof(wd.condition)];
    int cu_i = 0;
    for (; cu_i < (int)sizeof(wd.condition) - 1 && wd.condition[cu_i]; cu_i++) {
        cond_upper[cu_i] = (char)toupper((unsigned char)wd.condition[cu_i]);
    }
    cond_upper[cu_i] = '\0';
    if (wd.condition[0] == '\0') {
        strcpy(cond_upper, "--");
    }
    lv_label_set_text(lbl_cond, cond_upper);

    /* Hi / Lo */
    char hilo_buf[32];
    snprintf(hilo_buf, sizeof(hilo_buf), "H %.0f  -  L %.0f",
             wd.temp_high, wd.temp_low);
    lv_label_set_text(lbl_hilo, hilo_buf);

    /* Stats */
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", wd.humidity);
    lv_label_set_text(lbl_humid_val, buf);

    snprintf(buf, sizeof(buf), "%.0f%s", wd.dew_point, "\xc2\xb0");
    lv_label_set_text(lbl_dew_val, buf);

    snprintf(buf, sizeof(buf), "%s %.0f", wd.wind_dir, wd.wind_speed);
    lv_label_set_text(lbl_wind_val, buf);

    if (wd.uv_index < 0.0f) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        snprintf(buf, sizeof(buf), "%.0f", wd.uv_index);
    }
    lv_label_set_text(lbl_uv_val, buf);

    /* ── Forecast bars ── */
    /* Find min/max across hourly temps for proportional scaling */
    float t_min = wd.hourly_temps[0];
    float t_max = wd.hourly_temps[0];
    for (int i = 1; i < FORECAST_BARS; i++) {
        if (wd.hourly_temps[i] < t_min) t_min = wd.hourly_temps[i];
        if (wd.hourly_temps[i] > t_max) t_max = wd.hourly_temps[i];
    }
    float t_range = t_max - t_min;
    if (t_range < 1.0f) t_range = 1.0f;  /* Avoid division by near-zero */

    for (int i = 0; i < FORECAST_BARS; i++) {
        /* Bar height: proportional between BAR_MIN_H and BAR_MAX_H */
        float frac = (wd.hourly_temps[i] - t_min) / t_range;
        int h = BAR_MIN_H + (int)(frac * (BAR_MAX_H - BAR_MIN_H) + 0.5f);
        if (h < BAR_MIN_H) h = BAR_MIN_H;
        if (h > BAR_MAX_H) h = BAR_MAX_H;
        lv_obj_set_height(forecast_bars[i], h);

        /* Bar color based on temperature in Fahrenheit */
        float temp_f = to_fahrenheit(wd.hourly_temps[i], is_metric);
        lv_obj_set_style_bg_color(forecast_bars[i],
                                   lv_color_hex(bar_color_for_temp(temp_f, red_night)), 0);

        /* Hour label */
        uint8_t hr = wd.hourly_hours[i];
        char hr_buf[8];
        if (is_24h) {
            snprintf(hr_buf, sizeof(hr_buf), "%02d", hr);
        } else {
            int h12 = hr % 12;
            if (h12 == 0) h12 = 12;
            snprintf(hr_buf, sizeof(hr_buf), "%d%c", h12, hr < 12 ? 'a' : 'p');
        }
        lv_label_set_text(forecast_lbls[i], hr_buf);
    }

    lv_obj_remove_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);
}

/* ── Theme ───────────────────────────────────────────────────────────── */

/**
 * Set a label's text color, guarding against a NULL widget.
 */
static void set_label_color(lv_obj_t *lbl, uint32_t color_hex) {
    if (lbl) {
        lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex), 0);
    }
}

/**
 * Recolor the whole clock page for the active theme.
 *
 * Under Red Night every pixel becomes a red shade or black: background black,
 * text bright/secondary red, dividers dark red, and forecast bars red shades
 * scaled by temperature. In every non-red theme the fixed editorial cream/warm
 * palette is restored byte-for-byte, so those themes look identical to before.
 */
void clock_page_apply_theme(void) {
    if (!clock_root) return;

    bool red_night = theme_is_red_night(current_theme);

    /* Resolve the palette: red-night pulls from the active theme, otherwise
     * the fixed editorial constants are used verbatim. */
    uint32_t bg, primary, secondary, tertiary, dim, condition, rule, bar_label;
    if (red_night) {
        bg        = current_theme->bg_main;        /* black */
        primary   = current_theme->text_color;     /* bright red */
        secondary = current_theme->text_color;     /* bright red */
        tertiary  = current_theme->label_color;    /* dim red */
        dim       = current_theme->label_color;    /* dim red */
        condition = current_theme->label_color;    /* dim red */
        rule      = current_theme->bento_border;   /* dark red */
        bar_label = current_theme->label_color;    /* dim red */
    } else {
        bg        = CLK_BG;
        primary   = CLK_PRIMARY;
        secondary = CLK_SECONDARY;
        tertiary  = CLK_TERTIARY;
        dim       = CLK_DIM;
        condition = CLK_CONDITION;
        rule      = CLK_RULE;
        bar_label = CLK_BAR_LABEL;
    }

    /* Background */
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(bg), 0);

    /* Date stack (tertiary) */
    set_label_color(lbl_day,  tertiary);
    set_label_color(lbl_date, tertiary);
    set_label_color(lbl_year, tertiary);

    /* Weather header */
    set_label_color(lbl_temp, primary);
    set_label_color(lbl_deg,  primary);
    set_label_color(lbl_cond, condition);
    set_label_color(lbl_hilo, dim);

    /* Time */
    set_label_color(lbl_time, primary);
    set_label_color(lbl_ampm, dim);

    /* Stats strip values (secondary). Their labels are children created
     * inline with CLK_DIM; recolor those too via the stat columns. */
    set_label_color(lbl_humid_val, secondary);
    set_label_color(lbl_dew_val,   secondary);
    set_label_color(lbl_wind_val,  secondary);
    set_label_color(lbl_uv_val,    secondary);

    /* Stat unit labels: the second child of each value's parent column. */
    lv_obj_t *val_labels[4] = {
        lbl_humid_val, lbl_dew_val, lbl_wind_val, lbl_uv_val
    };
    for (int i = 0; i < 4; i++) {
        if (!val_labels[i]) continue;
        lv_obj_t *col = lv_obj_get_parent(val_labels[i]);
        if (!col) continue;
        uint32_t child_cnt = lv_obj_get_child_count(col);
        for (uint32_t c = 0; c < child_cnt; c++) {
            lv_obj_t *child = lv_obj_get_child(col, c);
            if (child && child != val_labels[i]) {
                set_label_color(child, dim);
            }
        }
    }

    /* Horizontal rules */
    for (int i = 0; i < rule_count; i++) {
        if (rules[i]) {
            lv_obj_set_style_bg_color(rules[i], lv_color_hex(rule), 0);
        }
    }

    /* Vertical dividers */
    for (int i = 0; i < vdivider_count; i++) {
        if (vdividers[i]) {
            lv_obj_set_style_bg_color(vdividers[i], lv_color_hex(rule), 0);
        }
    }

    /* Forecast labels */
    for (int i = 0; i < FORECAST_BARS; i++) {
        set_label_color(forecast_lbls[i], bar_label);
    }

    /* Forecast bars: recolor from the last-known weather sample so the red
     * map applies immediately on theme switch. When no weather is present
     * the bars stay hidden, so the placeholder color is harmless. */
    weather_data_t wd;
    weather_client_get_data(&wd);
    if (wd.valid) {
        for (int i = 0; i < FORECAST_BARS; i++) {
            if (!forecast_bars[i]) continue;
            float temp_f = to_fahrenheit(wd.hourly_temps[i], last_is_metric);
            lv_obj_set_style_bg_color(forecast_bars[i],
                lv_color_hex(bar_color_for_temp(temp_f, red_night)), 0);
        }
    } else {
        /* No data: give every bar the neutral cool/cold base for the theme. */
        uint32_t base = red_night ? 0x500000 : CLK_BAR_COOL;
        for (int i = 0; i < FORECAST_BARS; i++) {
            if (forecast_bars[i]) {
                lv_obj_set_style_bg_color(forecast_bars[i], lv_color_hex(base), 0);
            }
        }
    }
}

void clock_page_request_update(void) {
    if (clock_timer) {
        lv_timer_ready(clock_timer);  /* Fire on next LVGL tick */
    }
}

void clock_page_on_hide(void) {
    if (clock_timer) {
        lv_timer_pause(clock_timer);
    }
}

void clock_page_on_show(void) {
    if (clock_timer) {
        /* Re-align to next minute boundary before resuming */
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        uint32_t ms_to_next_min = (uint32_t)(60 - tm_now.tm_sec) * 1000;
        if (ms_to_next_min < 1000) ms_to_next_min = 60000;
        lv_timer_set_period(clock_timer, ms_to_next_min);
        lv_timer_resume(clock_timer);
        lv_timer_ready(clock_timer);  /* Immediate refresh on show */
    }
}
