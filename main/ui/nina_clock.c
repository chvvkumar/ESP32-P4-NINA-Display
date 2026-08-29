/**
 * @file nina_clock.c
 * @brief Clock page — editorial dark clockface with weather data.
 *
 * Seven layouts selected by app_config clock_layout:
 *   0 = Classic    — two thin horizontal rules framing a massive centered
 *                    time display, weather split above and below, 10-hour
 *                    forecast bar chart at the bottom.
 *   1 = Console 92 — amber observatory console: hairline frame with corner
 *                    brackets, hero condensed time, engraved ATMOSPHERICS
 *                    rule over a four-cell readout grid, baseline bar chart.
 *   2 = Broadside  — grotesk poster: spread dateline, huge black time with
 *                    a rotated PM, thin condition line, temp + high/low,
 *                    forecast as numerals sized and weighted by temperature.
 *   3 = Evensong   — didone word clock: the time spoken in words over a
 *                    dot-leader timetable of readings and a numeral forecast.
 *   4 = Blueprint  — cyanotype drafting sheet: registration marks, stencil
 *                    hero time with a dimension chain, leader-dot annotation
 *                    column, dimensioned elevation bar chart.
 *   5 = Transit Line — metro map: red roundel names the time, the day is
 *                    one line snaking three runs with temperature-colored
 *                    segments and ten stations.
 *   6 = Night Network — two-line network map: vertical Temperature line
 *                    crosses the horizontal Conditions line, corner name
 *                    block and legend panel.
 *
 * Fixed per-layout color palettes; Red Night remaps everything to red.
 */

#include "nina_clock.h"
#include "nina_dashboard_internal.h"
#include "nina_clock_internal.h"
#include "nina_idle_indicator.h"
#include "display_defs.h"
#include "app_config.h"
#include "weather_client.h"
#include "ui_round.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
static const char *TAG = "clock_ui";

/* The Classic, Console 92, Blueprint and Transit / Night Network palettes,
 * FORECAST_BARS, MET_CSTATIONS and the Console bar range live in
 * ui/nina_clock_internal.h: the round builders need them too. BS_* and EV_*
 * stay here because no round builder uses them. */

/* ── Broadside palette (mock typ-b1) ─────────────────────────────────── */

#define BS_BG           0x101013
#define BS_BONE         0xE8E4DA
#define BS_HAIR         0x767269
#define BS_DIM          0x3D3C40
#define BS_SIG          0xFF5C33

/* ── Evensong palette (mock typ-b2) ──────────────────────────────────── */

#define EV_BG           0x0D0C0A
#define EV_PARCH        0xECE4D4
#define EV_BRASS        0xD8A648
#define EV_MUTE         0x7D766A
#define EV_DIM          0x46423A

#define CLOCK_PADDING   40
#define BAR_WIDTH       22
#define BAR_GAP         4
#define BAR_MIN_H       21
#define BAR_MAX_H       63

/* ── Font declarations (generated .c files in main/ui/) ──────────────── */

extern const lv_font_t lv_font_playfair_228;
extern const lv_font_t lv_font_playfair_90;
extern const lv_font_t lv_font_overpass_27;
extern const lv_font_t lv_font_overpass_16;
LV_FONT_DECLARE(lv_font_saira_black_300)
LV_FONT_DECLARE(lv_font_saira_black_110)
LV_FONT_DECLARE(lv_font_saira_black_66)
LV_FONT_DECLARE(lv_font_saira_thin_240)
LV_FONT_DECLARE(lv_font_saira_thin_84)
LV_FONT_DECLARE(lv_font_saira_light_46)
LV_FONT_DECLARE(lv_font_bodoni_black_150)
LV_FONT_DECLARE(lv_font_bodoni_ital_90)
LV_FONT_DECLARE(lv_font_bodoni_ital_44)
LV_FONT_DECLARE(lv_font_stencil_190)       /* digits + colon */
LV_FONT_DECLARE(lv_font_hanken_black_96)   /* digits + colon */
LV_FONT_DECLARE(lv_font_hanken_bold_44)    /* digits, minus, degree */
LV_FONT_DECLARE(lv_font_hanken_black_40)   /* space + A-Z */

/* ── Widget pointers ─────────────────────────────────────────────────── *
 * The handles ui/nina_clock_internal.h declares have external linkage so the
 * round builders in ui/nina_clock_round.c can write them; the rest stay
 * file static. */

lv_obj_t *clock_root = NULL;

/* Active layout (0 = Classic, 1 = Console 92, 2 = Broadside, 3 = Evensong,
 * 4 = Blueprint, 5 = Transit Line, 6 = Night Network), cached at build */
static uint8_t s_layout = 0;

/* True while the clock page is the visible page (set by on_show/on_hide) */
static bool s_page_visible = false;

/* Header — date stack */
lv_obj_t *lbl_day  = NULL;          /* Classic + Broadside */
lv_obj_t *lbl_date = NULL;          /* all layouts (per-layout format) */
static lv_obj_t *lbl_year = NULL;   /* Classic + Broadside */
static lv_obj_t *lbl_mday = NULL;   /* Broadside only (day-of-month) */

/* Weather */
lv_obj_t *lbl_temp = NULL;
lv_obj_t *lbl_deg  = NULL;          /* Classic only */
lv_obj_t *lbl_cond = NULL;
lv_obj_t *lbl_hilo = NULL;

/* Time */
lv_obj_t *lbl_time = NULL;
lv_obj_t *lbl_ampm = NULL;

/* Stats strip */
lv_obj_t *lbl_humid_val = NULL;
lv_obj_t *lbl_dew_val   = NULL;
lv_obj_t *lbl_wind_val  = NULL;
lv_obj_t *lbl_uv_val   = NULL;

/* Forecast (Classic + Console bars; hour labels shared by all layouts) */
lv_obj_t *forecast_row  = NULL;
lv_obj_t *forecast_bars[FORECAST_BARS];
lv_obj_t *forecast_lbls[FORECAST_BARS];
/* Forecast temp numerals (Console / Broadside / Evensong) */
lv_obj_t *forecast_temp_lbls[FORECAST_BARS];

/* Console 92-only widgets */
static lv_obj_t *console_corners[4];         /* 2px corner brackets */
lv_obj_t *console_atmo = NULL;               /* ATMOSPHERICS knockout label */
lv_obj_t *console_caps[4];                   /* readout cell captions */
lv_obj_t *console_rulers[4];                 /* readout tick rulers */
static lv_obj_t *console_fc_cap = NULL;      /* forecast caption, left */
static lv_obj_t *console_range = NULL;       /* forecast range, right */
lv_obj_t *lbl_cond_big = NULL;               /* SKY cell big value */

/* Evensong-only widgets */
static lv_obj_t *ev_itis = NULL;             /* "IT IS" */
static lv_obj_t *ev_w1 = NULL;               /* hour word */
static lv_obj_t *ev_w2 = NULL;               /* minute words */
static lv_obj_t *ev_w3 = NULL;               /* part-of-day words */
static lv_obj_t *ev_keys[6];                 /* timetable keys */
static lv_obj_t *ev_leaders[6];              /* timetable dot leaders */

/* Generic recolor collectors for the Blueprint / Transit / Network
 * layouts (chrome that apply_theme repaints without a name each). */
#define X_DIM_LBL_MAX 12
#define X_TER_LBL_MAX 6
#define X_INK_LBL_MAX 6
#define X_DIM_BG_MAX  24
#define X_TER_BG_MAX  20
static lv_obj_t *x_dim_lbls[X_DIM_LBL_MAX]; static int x_dim_lbl_cnt = 0;
static lv_obj_t *x_ter_lbls[X_TER_LBL_MAX]; static int x_ter_lbl_cnt = 0;
static lv_obj_t *x_ink_lbls[X_INK_LBL_MAX]; static int x_ink_lbl_cnt = 0;
static lv_obj_t *x_dim_bg[X_DIM_BG_MAX];    static int x_dim_bg_cnt = 0;
static lv_obj_t *x_ter_bg[X_TER_BG_MAX];    static int x_ter_bg_cnt = 0;

/* Blueprint-only widgets */
lv_obj_t *blu_frame_o = NULL;                /* outer frame (line border) */
lv_obj_t *blu_frame_i = NULL;                /* inner frame (dim border) */
static lv_obj_t *blu_plot = NULL;            /* chart plot (baseline border) */
lv_obj_t *blu_dimlbl = NULL;                 /* "LOCAL TIME 1:38 AM" */
static lv_obj_t *blu_max_lbl = NULL;         /* MAX callout, moves with peak */
static lv_obj_t *blu_peak_tick = NULL;       /* dimension tick over peak bar */
lv_obj_t *blu_arrow[2];                      /* dimension-line arrowheads */

/* Transit / Network shared widgets */
lv_obj_t *met_name = NULL;                   /* live weekday header */
lv_obj_t *met_segs[FORECAST_BARS];           /* route/line segments */
static lv_obj_t *met_arcs[2];                /* Transit U-bends */
lv_obj_t *met_dots[FORECAST_BARS];           /* station dots */
static lv_obj_t *met_ring5 = NULL;           /* Transit roundel ring */
static lv_obj_t *met_bar = NULL;             /* Transit roundel navy bar */
static lv_obj_t *met_pill = NULL;            /* YOU ARE HERE pill */
static lv_obj_t *met_peak_lbl = NULL;        /* PEAK tag, moves with peak */
lv_obj_t *met_peak_ring = NULL;              /* Network peak double ring */
lv_obj_t *met_peak_core = NULL;              /* Network peak core dot */
static lv_obj_t *met_sun_dia = NULL;         /* sunrise diamond */
static lv_obj_t *met_sun_lbl = NULL;         /* SUNRISE label */
static lv_obj_t *met_terminus = NULL;        /* Transit terminus cap */
lv_obj_t *met_panel = NULL;                  /* Network corner block */
static lv_obj_t *met_legend = NULL;          /* Network legend panel */
lv_obj_t *met_tick_a = NULL;                 /* Network temp-line end ticks */
lv_obj_t *met_tick_b = NULL;
lv_obj_t *met_cline[3];                      /* Network conditions line + ends */
lv_obj_t *met_cdots[MET_CSTATIONS];          /* Network conditions dots */
static lv_obj_t *met_chips[9];               /* Network legend swatches */
static lv_obj_t *met_band_lbl[4];            /* Network legend band ranges */
lv_obj_t *met_hi_val = NULL;                 /* Network HIGH station value */
lv_obj_t *met_lo_val = NULL;                 /* Network LOW station value */

/* Network legend swatch -> temperature band (-1 = Conditions blue) */
static const int8_t met_chip_band[9] = { 0, 1, 2, 3, -1, 0, 1, 2, 3 };

/* Transit station geometry: runs A (0-3), B (4-7), C (8-9) */
static const int16_t m5_sx[FORECAST_BARS] =
    { 110, 260, 410, 560, 560, 410, 260, 110, 180, 330 };
#if !CONFIG_NINA_FAMILY_ROUND
/* Read only by build_layout_transit(), which is round-guarded below. */
static const int16_t m5_seg_x[FORECAST_BARS] =
    { 90, 190, 340, 490, 490, 340, 190, 90, 90, 260 };
static const int16_t m5_seg_w[FORECAST_BARS] =
    { 100, 150, 150, 110, 110, 150, 150, 100, 170, 170 };
#endif /* !CONFIG_NINA_FAMILY_ROUND */
static const int16_t m5_line_y[3] = { 408, 528, 648 };   /* run tops */
#if !CONFIG_NINA_FAMILY_ROUND
static const int16_t m5_band_y[3] = { 365, 552, 604 };   /* label band tops */
#endif /* !CONFIG_NINA_FAMILY_ROUND */
static const int16_t m5_tag_y[3]  = { 434, 484, 674 };   /* PEAK/SUNRISE band */

/* Blueprint dimension-line arrowheads (3-point lv_line triangles) */
const lv_point_precise_t blu_arr_l_pts[3] = { {8, 0}, {0, 4}, {8, 8} };
const lv_point_precise_t blu_arr_r_pts[3] = { {0, 0}, {8, 4}, {0, 8} };

/* Idle indicator dot — kept alive across layout rebuilds because the
 * indicator registry in nina_idle_indicator.c has no per-entry removal. */
static lv_obj_t *idle_dot = NULL;

/* Dividers — captured so apply_theme can recolor them */
#define CLK_RULE_COUNT 3
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

/* ── Round-only shape handles (see nina_clock_internal.h) ────────────── *
 * NULL on the square family and on every round face that does not draw
 * them. Defined here, not in the round builder file, so reset_widget_ptrs()
 * and the update paths reach them on both families with no conditional. */
lv_obj_t *clk_fc_arc[FORECAST_BARS];
lv_obj_t *clk_minute_arc = NULL;
lv_obj_t *clk_now_tick = NULL;
lv_obj_t *clk_arc_cond = NULL;
lv_obj_t *clk_arc_stats = NULL;
lv_obj_t *clk_con_tick[4];
lv_obj_t *clk_blu_ray[FORECAST_BARS];
lv_obj_t *clk_blu_peak_tick = NULL;
lv_obj_t *clk_blu_max_lbl = NULL;

clock_round_restyle_cb_t clock_round_restyle = NULL;
uint16_t clock_round_tick_ms = 0;

/* Shadows for the two round Classic rim arclabels: see the header. File scope
 * so reset_widget_ptrs() can clear them on a layout rebuild. */
char clk_arc_cond_prev[192];
char clk_arc_stats_prev[160];

/* ── Palette resolution ──────────────────────────────────────────────── */

typedef struct {
    uint32_t bg;
    uint32_t primary;
    uint32_t secondary;
    uint32_t tertiary;
    uint32_t dim;
    uint32_t condition;
    uint32_t rule;
    uint32_t bar_label;
    uint32_t accent;
} clk_palette_t;

/**
 * Resolve the working palette for the active layout. Under Red Night every
 * layout maps onto the theme's red ramp; otherwise each layout uses its own
 * fixed palette (Classic keeps the original editorial constants verbatim).
 */
static void resolve_palette(clk_palette_t *p, bool red_night) {
    if (red_night) {
        p->bg        = current_theme->bg_main;        /* black */
        p->primary   = current_theme->text_color;     /* bright red */
        p->secondary = current_theme->text_color;     /* bright red */
        p->tertiary  = current_theme->label_color;    /* dim red */
        p->dim       = current_theme->label_color;    /* dim red */
        p->condition = current_theme->label_color;    /* dim red */
        p->rule      = current_theme->bento_border;   /* dark red */
        p->bar_label = current_theme->label_color;    /* dim red */
        p->accent    = current_theme->text_color;     /* bright red */
        return;
    }
    switch (s_layout) {
    case 1:  /* Console 92 */
        p->bg        = CON_BG;
        p->primary   = CON_CREAM;
        p->secondary = CON_CREAM;
        p->tertiary  = CON_DIM;
        p->dim       = CON_DIM;
        p->condition = CON_DIM;
        p->rule      = CON_LINE;
        p->bar_label = CON_DIM;
        p->accent    = CON_AMBER;
        break;
    case 2:  /* Broadside */
        p->bg        = BS_BG;
        p->primary   = BS_BONE;
        p->secondary = BS_BONE;
        p->tertiary  = BS_HAIR;
        p->dim       = BS_HAIR;
        p->condition = BS_BONE;
        p->rule      = BS_DIM;
        p->bar_label = BS_HAIR;
        p->accent    = BS_SIG;
        break;
    case 3:  /* Evensong */
        p->bg        = EV_BG;
        p->primary   = EV_PARCH;
        p->secondary = EV_PARCH;
        p->tertiary  = EV_MUTE;
        p->dim       = EV_MUTE;
        p->condition = EV_PARCH;
        p->rule      = EV_DIM;
        p->bar_label = EV_MUTE;
        p->accent    = EV_BRASS;
        break;
    case 4:  /* Blueprint */
        p->bg        = BLU_BG;
        p->primary   = BLU_HI;
        p->secondary = BLU_HI;
        p->tertiary  = BLU_LINE;
        p->dim       = BLU_DIM;
        p->condition = BLU_HI;
        p->rule      = BLU_DIM;
        p->bar_label = BLU_DIM;
        p->accent    = BLU_HI;
        break;
    case 5:  /* Transit Line */
    case 6:  /* Night Network */
        p->bg        = MET_BG;
        p->primary   = MET_INK;
        p->secondary = MET_INK;
        p->tertiary  = MET_DIM;
        p->dim       = MET_DIM;
        p->condition = MET_GOLD;
        p->rule      = MET_EDGE;
        p->bar_label = MET_DIM;
        p->accent    = MET_GOLD;
        break;
    default: /* Classic */
        p->bg        = CLK_BG;
        p->primary   = CLK_PRIMARY;
        p->secondary = CLK_SECONDARY;
        p->tertiary  = CLK_TERTIARY;
        p->dim       = CLK_DIM;
        p->condition = CLK_CONDITION;
        p->rule      = CLK_RULE;
        p->bar_label = CLK_BAR_LABEL;
        p->accent    = CLK_ACCENT;
        break;
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

/**
 * Make a transparent, non-scrollable container.
 */
lv_obj_t *make_container(lv_obj_t *parent) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

/**
 * Create a label with font, color, and letter spacing.
 */
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
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
 * Fit a one-line condition label: drop to the small face when the text
 * would overflow the label's width at the big face, and pin the height
 * to one line so LV_LABEL_LONG_DOT clips instead of wrapping.
 */
static void fit_cond_one_line(lv_obj_t *lbl, const lv_font_t *big,
                              const lv_font_t *small) {
    if (!lbl) return;
    int32_t w = lv_obj_get_width(lbl);
    if (w <= 0) {
        /* First update runs before the initial layout pass — force it so
         * the width is real and the fit applies on page build, not a
         * minute later on the next timer tick. */
        lv_obj_update_layout(lbl);
        w = lv_obj_get_width(lbl);
        if (w <= 0) return;
    }
    int32_t ls = lv_obj_get_style_text_letter_space(lbl, LV_PART_MAIN);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(lbl), big, ls, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *f = (sz.x <= w) ? big : small;
    lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_height(lbl, lv_font_get_line_height(f));
}

/**
 * Create a 1px horizontal rule across full width.
 */
lv_obj_t *make_rule(lv_obj_t *parent) {
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
 * Convert temperature to Fahrenheit for bar color thresholds. Shared with
 * the round builders (nina_clock_internal.h) so they do not re-derive the
 * same single-precision arithmetic inline.
 */
float to_fahrenheit(float temp, bool is_metric) {
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
uint32_t bar_color_for_temp(float temp_f, bool red_night) {
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

#if !CONFIG_NINA_FAMILY_ROUND
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
#endif /* !CONFIG_NINA_FAMILY_ROUND */

/* ── Helpers for the Blueprint / Transit / Network layouts ───────────── */

/** Push a label onto the dim recolor collector. */
void reg_dim_lbl(lv_obj_t *o) {
    if (x_dim_lbl_cnt < X_DIM_LBL_MAX) x_dim_lbls[x_dim_lbl_cnt++] = o;
}

/** Push a label onto the tertiary (hairline ink) recolor collector. */
void reg_ter_lbl(lv_obj_t *o) {
    if (x_ter_lbl_cnt < X_TER_LBL_MAX) x_ter_lbls[x_ter_lbl_cnt++] = o;
}

/** Push a label onto the primary-ink recolor collector. */
void reg_ink_lbl(lv_obj_t *o) {
    if (x_ink_lbl_cnt < X_INK_LBL_MAX) x_ink_lbls[x_ink_lbl_cnt++] = o;
}

/** Push a bg-colored object onto the dim recolor collector. */
void reg_dim_bg(lv_obj_t *o) {
    if (x_dim_bg_cnt < X_DIM_BG_MAX) x_dim_bg[x_dim_bg_cnt++] = o;
}

/** Push a bg-colored object onto the tertiary recolor collector. */
void reg_ter_bg(lv_obj_t *o) {
    if (x_ter_bg_cnt < X_TER_BG_MAX) x_ter_bg[x_ter_bg_cnt++] = o;
}

/** Absolutely positioned filled rectangle. */
lv_obj_t *make_bg_rect(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t color) {
    lv_obj_t *o = make_container(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

/**
 * Metro-map temperature band color: quartiles of the forecast range map
 * onto teal / gold / orange / red; Red Night uses the same dark-to-bright
 * red ramp as the Classic bars.
 */
uint32_t met_band_color(int band, bool red_night) {
    static const uint32_t ramp[4]    = { MET_TEAL, MET_GOLD, MET_ORANGE, MET_RED };
    static const uint32_t ramp_rn[4] = { 0x500000, 0x800000, 0xC00000, 0xFF0000 };
    if (band < 0) band = 0;
    if (band > 3) band = 3;
    return red_night ? ramp_rn[band] : ramp[band];
}

/** Transit/Network fixed accents, remapped under Red Night. */
static uint32_t met_red_col(bool red_night) {
    return red_night ? current_theme->text_color : MET_RED;
}
static uint32_t met_blue_col(bool red_night) {
    return red_night ? 0x900000 : MET_BLUE;
}
static uint32_t met_navy_col(bool red_night) {
    return red_night ? 0x300000 : MET_NAVY;
}
static uint32_t met_box_col(bool red_night) {
    return red_night ? current_theme->bg_main : MET_BOX;
}
static uint32_t met_fill_col(bool red_night) {
    return red_night ? current_theme->bg_main : 0xFFFFFF;
}

/** Transit run index (0=A, 1=B, 2=C) for station @p i. */
static int m5_row_of(int i) {
    if (i < 4) return 0;
    if (i < 8) return 1;
    return 2;
}

/**
 * White-filled station dot with a colored ring, centered on (cx, cy).
 */
lv_obj_t *make_station_dot(lv_obj_t *parent, int cx, int cy,
                           int inner, int bw, uint32_t border) {
    int sz = inner + 2 * bw;
    lv_obj_t *d = make_container(parent);
    lv_obj_set_size(d, sz, sz);
    lv_obj_set_pos(d, cx - sz / 2, cy - sz / 2);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, bw, 0);
    lv_obj_set_style_border_color(d, lv_color_hex(border), 0);
    lv_obj_set_style_border_opa(d, LV_OPA_COVER, 0);
    return d;
}

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * Thick half-ring U-bend for the Transit route, drawn with an lv_arc's
 * background arc only. @p right_half true = opens left (right-side bend).
 * The arc centerline radius is (size - width) / 2.
 */
static lv_obj_t *make_bend_arc(lv_obj_t *parent, int x, int y, int size,
                               bool right_half, uint32_t color) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_pos(arc, x, y);
    if (right_half) {
        lv_arc_set_bg_angles(arc, 270, 90);
    } else {
        lv_arc_set_bg_angles(arc, 90, 270);
    }
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

/**
 * In-place sanitize a string to the saira_thin_84 glyph range:
 * uppercase, then keep only A-Z, 0-9 and space (others dropped).
 */
static void sanitize_thin84(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        char c = (char)toupper((unsigned char)*r);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') {
            *w++ = c;
        }
    }
    *w = '\0';
}

/**
 * Set the H/L label text for the active layout.
 *
 * Classic keeps today's exact "H %.0f  -  L %.0f" text. Console uses the
 * compact "%.0f / %.0f" grid-sub form (its TEMP caption names the pair).
 * Broadside embeds LVGL recolor
 * tags to print HIGH/LOW captions in hairline gray and values in bone
 * (both red under Red Night). Evensong has no lbl_hilo (merged into the
 * Temperature timetable row).
 */
static void set_hilo_text(const weather_data_t *wd, bool red_night) {
    if (s_layout == 6) {
        /* Night Network: H/L are two Conditions-line stations */
        char hb[32];
        if (met_hi_val) {
            if (wd->valid) {
                snprintf(hb, sizeof(hb), "%.0f\xc2\xb0", wd->temp_high);
            } else {
                snprintf(hb, sizeof(hb), "--");
            }
            lv_label_set_text(met_hi_val, hb);
        }
        if (met_lo_val) {
            if (wd->valid) {
                snprintf(hb, sizeof(hb), "%.0f\xc2\xb0", wd->temp_low);
            } else {
                snprintf(hb, sizeof(hb), "--");
            }
            lv_label_set_text(met_lo_val, hb);
        }
        return;
    }
    if (!lbl_hilo) return;
    if (!wd->valid) {
        lv_label_set_text(lbl_hilo, "--");
        return;
    }
    char buf[128];
    if (s_layout == 1) {
        /* TEMP caption carries the meaning; bare values fit the 152px cell */
        snprintf(buf, sizeof(buf), "%.0f / %.0f", wd->temp_high, wd->temp_low);
    } else if (s_layout == 4) {
        /* Blueprint annotation row */
        snprintf(buf, sizeof(buf), "H %.0f L %.0f", wd->temp_high, wd->temp_low);
    } else if (s_layout == 2) {
        uint32_t cap = red_night ? current_theme->label_color : BS_HAIR;
        uint32_t val = red_night ? current_theme->text_color : BS_BONE;
        snprintf(buf, sizeof(buf),
                 "#%06x HIGH# #%06x %.0f\xc2\xb0#\n#%06x LOW# #%06x %.0f\xc2\xb0#",
                 (unsigned)cap, (unsigned)val, wd->temp_high,
                 (unsigned)cap, (unsigned)val, wd->temp_low);
    } else {
        snprintf(buf, sizeof(buf), "H %.0f  -  L %.0f", wd->temp_high, wd->temp_low);
    }
    lv_label_set_text(lbl_hilo, buf);
}

/* ── Evensong word tables ────────────────────────────────────────────── */

/* Hour words indexed by tm_hour % 12 (0 = TWELVE). bodoni_black_150 range
 * is A-Z + space only. */
static const char *ev_hour_words[12] = {
    "TWELVE", "ONE", "TWO", "THREE", "FOUR", "FIVE",
    "SIX", "SEVEN", "EIGHT", "NINE", "TEN", "ELEVEN"
};

/**
 * Compose the minute in words (lowercase, a-z + space + hyphen only,
 * matching the bodoni_ital_90 glyph range; no apostrophes).
 * 0 -> "on the hour", 1-9 -> "oh one".."oh nine", hyphenated tens.
 */
static void minute_words(int m, char *buf, size_t n) {
    static const char *ones[10] = {
        "", "one", "two", "three", "four",
        "five", "six", "seven", "eight", "nine"
    };
    static const char *teens[10] = {
        "ten", "eleven", "twelve", "thirteen", "fourteen",
        "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"
    };
    static const char *tens[4] = { "twenty", "thirty", "forty", "fifty" };

    if (m <= 0) {
        snprintf(buf, n, "on the hour");
    } else if (m < 10) {
        snprintf(buf, n, "oh %s", ones[m]);
    } else if (m < 20) {
        snprintf(buf, n, "%s", teens[m - 10]);
    } else {
        int t = m / 10 - 2;
        int o = m % 10;
        if (t > 3) t = 3;  /* defensive: minutes are 0-59 */
        if (o) {
            snprintf(buf, n, "%s-%s", tens[t], ones[o]);
        } else {
            snprintf(buf, n, "%s", tens[t]);
        }
    }
}

/** Part-of-day words for the Evensong third line. */
static const char *part_of_day_words(int hour) {
    if (hour >= 5 && hour <= 11)  return "in the morning";
    if (hour >= 12 && hour <= 17) return "in the afternoon";
    if (hour >= 18 && hour <= 21) return "in the evening";
    return "at night";
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
    /* A round face that sweeps seconds (Classic) ticks every second instead. */
    lv_timer_set_period(clock_timer, clock_round_tick_ms ? clock_round_tick_ms
                                                         : ms_to_next_min);
}

/**
 * Create the minute-aligned clock timer, started paused.
 */
static void create_clock_timer(void) {
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
}

/* ── Shared sub-builders ─────────────────────────────────────────────── */

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * Build the 4-column stats strip (HUMID / DEW PT / WIND / UV IDX) with
 * vertical dividers. Used by the Classic layout.
 */
static void build_stats_strip(lv_obj_t *parent) {
    lv_obj_t *stats_strip = make_container(parent);
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
}

/**
 * Build the 10-bar hourly forecast row (bars + hour labels), hidden
 * until data arrives. Used by the Classic layout.
 *
 * @param bar_w  bar rectangle width in px.
 * @param spread true = spread cells evenly across the row,
 *               false = pack centered with BAR_GAP (Classic).
 */
static void build_forecast_row(lv_obj_t *parent, int bar_w, bool spread) {
    forecast_row = make_container(parent);
    lv_obj_set_size(forecast_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(forecast_row,
                          spread ? LV_FLEX_ALIGN_SPACE_EVENLY : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(forecast_row, spread ? 0 : BAR_GAP, 0);
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
        lv_obj_set_size(bar, bar_w, BAR_MIN_H);
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
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

/* ── Layout builders ─────────────────────────────────────────────────── */

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * Layout 0 — Classic. Pixel-identical to the original single layout.
 */
static void build_layout_classic(void) {
    lv_obj_set_style_pad_all(clock_root, CLOCK_PADDING, 0);
    lv_obj_set_flex_flow(clock_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_root, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(clock_root, 0, 0);  /* justify-between handles spacing */

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
    build_stats_strip(clock_root);

    /* ── Forecast row (10 bars + labels, hidden until data arrives) ── */
    build_forecast_row(clock_root, BAR_WIDTH, false);
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * Create a 2px corner bracket for the Console 92 frame.
 * @p sides is an OR of LV_BORDER_SIDE_* values.
 */
static lv_obj_t *make_console_corner(lv_obj_t *parent, int x, int y,
                                      lv_border_side_t sides) {
    lv_obj_t *c = make_container(parent);
    lv_obj_set_size(c, 14, 14);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_side(c, sides, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(CON_DIM), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    return c;
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

/**
 * Create a small tick ruler (96x6, ticks every 12px over a baseline) for
 * a Console 92 readout cell.
 */
static lv_obj_t *make_console_ruler(lv_obj_t *parent) {
    lv_obj_t *ruler = make_container(parent);
    lv_obj_set_size(ruler, 96, 6);
    lv_obj_set_style_border_width(ruler, 1, 0);
    lv_obj_set_style_border_side(ruler, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(ruler, lv_color_hex(CON_LINE), 0);
    lv_obj_set_style_border_opa(ruler, LV_OPA_COVER, 0);
    for (int t = 0; t < 8; t++) {
        lv_obj_t *tick = make_container(ruler);
        lv_obj_set_size(tick, 1, 6);
        lv_obj_set_pos(tick, t * 12, 0);
        lv_obj_set_style_bg_color(tick, lv_color_hex(CON_LINE), 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    }
    return ruler;
}

/**
 * Create one Console 92 readout cell: caption, big value, tick ruler,
 * and an empty sub-row the caller fills. Returns the sub-row.
 */
lv_obj_t *make_console_cell(lv_obj_t *grid, int idx, const char *cap,
                            const lv_font_t *val_font,
                            lv_obj_t **out_val) {
    lv_obj_t *cell = make_container(grid);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_height(cell, LV_PCT(100));
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 8, 0);
    lv_obj_set_style_pad_top(cell, 4, 0);
    if (idx > 0) {
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(CON_LINE), 0);
        lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
    }

    console_caps[idx] = make_label(cell, &lv_font_overpass_16, CON_DIM, 2, cap);
    *out_val = make_label(cell, val_font, CON_CREAM, 0, "--");
    console_rulers[idx] = make_console_ruler(cell);

    lv_obj_t *sub = make_container(cell);
    lv_obj_set_size(sub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sub, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sub, 8, 0);
    return sub;
}

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * Layout 1 — Console 92. Amber observatory console: corner brackets,
 * header captions, hero condensed time with a PM chip, engraved
 * ATMOSPHERICS rule over a 4-cell readout grid, and a captioned
 * forecast bar strip.
 */
static void build_layout_console(void) {
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(CON_BG), 0);

    /* ── Corner brackets ── */
    console_corners[0] = make_console_corner(clock_root, 12, 12,
                            LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
    console_corners[1] = make_console_corner(clock_root, screen_size() - 26, 12,
                            LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT);
    console_corners[2] = make_console_corner(clock_root, 12, screen_size() - 26,
                            LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT);
    console_corners[3] = make_console_corner(clock_root, screen_size() - 26,
                            screen_size() - 26,
                            LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);

    /* ── Hero time (centered, in the old header band) + PM chip ── */
    lbl_time = make_label(clock_root, &lv_font_saira_thin_240, CON_AMBER, 0, "");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 44);

    lbl_ampm = make_label(clock_root, &lv_font_overpass_16, CON_AMBER, 3, "");
    lv_obj_align(lbl_ampm, LV_ALIGN_TOP_RIGHT, -74, 210);
    lv_obj_set_style_border_width(lbl_ampm, 1, 0);
    lv_obj_set_style_border_color(lbl_ampm, lv_color_hex(CON_LINE), 0);
    lv_obj_set_style_border_opa(lbl_ampm, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(lbl_ampm, 6, 0);
    lv_obj_set_style_pad_bottom(lbl_ampm, 5, 0);
    lv_obj_set_style_pad_left(lbl_ampm, 10, 0);
    lv_obj_set_style_pad_right(lbl_ampm, 8, 0);

    /* ── Date, centered between the time glyphs and the ATMOSPHERICS rule ── */
    lbl_date = make_label(clock_root, &lv_font_overpass_27, CON_CREAM, 2, "---");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, 290);

    /* ── Engraved section rule with knocked-out label ── */
    lv_obj_t *atmo_rule = make_rule(clock_root);
    lv_obj_set_size(atmo_rule, screen_size() - 112, 1);
    lv_obj_set_pos(atmo_rule, 56, 368);
    lv_obj_set_style_bg_color(atmo_rule, lv_color_hex(CON_LINE), 0);

    console_atmo = make_label(clock_root, &lv_font_overpass_16, CON_DIM, 3,
                              "ATMOSPHERICS");
    lv_obj_set_style_bg_color(console_atmo, lv_color_hex(CON_BG), 0);
    lv_obj_set_style_bg_opa(console_atmo, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(console_atmo, 14, 0);
    lv_obj_align(console_atmo, LV_ALIGN_TOP_MID, 0, 360);

    /* ── 4-cell readout grid ── */
    lv_obj_t *grid = make_container(clock_root);
    /* Cell needs 4 pad + 25 cap + 8 + 44 val + 8 + 6 ruler + 8 + 39 sub
     * = 142px; 150 leaves breathing room so nothing clips. */
    lv_obj_set_size(grid, screen_size() - 112, 150);
    lv_obj_set_pos(grid, 56, 396);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* TEMP cell — value + H/L sub */
    lv_obj_t *sub = make_console_cell(grid, 0, "TEMP", &lv_font_saira_light_46,
                                      &lbl_temp);
    lbl_hilo = make_label(sub, &lv_font_overpass_27, CON_DIM, 0, "--");

    /* SKY cell — first word big, no sub (the value says it all) */
    sub = make_console_cell(grid, 1, "SKY", &lv_font_saira_light_46,
                            &lbl_cond_big);
    /* Long single-word conditions ("THUNDERSTORM") exceed the cell width */
    lv_obj_set_width(lbl_cond_big, 148);
    lv_label_set_long_mode(lbl_cond_big, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl_cond_big, LV_TEXT_ALIGN_CENTER, 0);

    /* HUMIDITY cell — DEW sub */
    sub = make_console_cell(grid, 2, "HUMIDITY", &lv_font_saira_light_46,
                            &lbl_humid_val);
    make_label(sub, &lv_font_overpass_27, CON_DIM, 0, "DEW");
    lbl_dew_val = make_label(sub, &lv_font_overpass_27, CON_CREAM, 0, "--");

    /* WIND cell — UV sub */
    sub = make_console_cell(grid, 3, "WIND", &lv_font_saira_light_46,
                            &lbl_wind_val);
    make_label(sub, &lv_font_overpass_27, CON_DIM, 0, "UV");
    lbl_uv_val = make_label(sub, &lv_font_overpass_27, CON_CREAM, 0, "--");

    /* ── Forecast strip ── */
    forecast_row = make_container(clock_root);
    lv_obj_set_size(forecast_row, screen_size() - 112, LV_SIZE_CONTENT);
    lv_obj_set_pos(forecast_row, 56, 554);
    lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(forecast_row, 6, 0);
    lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *fc_head = make_container(forecast_row);
    lv_obj_set_size(fc_head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fc_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fc_head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    console_fc_cap = make_label(fc_head, &lv_font_overpass_16, CON_DIM, 2,
                                "TEMP FORECAST");
    console_range = make_label(fc_head, &lv_font_overpass_16, CON_DIM, 2,
                               "RANGE --");

    lv_obj_t *bars_row = make_container(forecast_row);
    lv_obj_set_size(bars_row, LV_PCT(100), CON_BAR_MAX + 24);
    lv_obj_set_flex_flow(bars_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    for (int i = 0; i < FORECAST_BARS; i++) {
        lv_obj_t *bcol = make_container(bars_row);
        lv_obj_set_size(bcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bcol, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bcol, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(bcol, 4, 0);

        forecast_temp_lbls[i] = make_label(bcol, &lv_font_overpass_16,
                                           CON_DIM, 0, "--");

        lv_obj_t *bar = lv_obj_create(bcol);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 24, CON_BAR_MIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CON_DIM), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        forecast_bars[i] = bar;
    }

    lv_obj_t *fc_base = make_rule(forecast_row);
    lv_obj_set_style_bg_color(fc_base, lv_color_hex(CON_LINE), 0);

    lv_obj_t *hours_row = make_container(forecast_row);
    lv_obj_set_size(hours_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hours_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hours_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int i = 0; i < FORECAST_BARS; i++) {
        forecast_lbls[i] = make_label(hours_row, &lv_font_overpass_16,
                                      CON_DIM, 0, "--");
        lv_obj_set_width(forecast_lbls[i], 32);
        lv_obj_set_style_text_align(forecast_lbls[i], LV_TEXT_ALIGN_CENTER, 0);
    }
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

/**
 * Create a caption + value metric pair (overpass_27) for the Broadside
 * metrics row.
 */
static void make_metric_pair(lv_obj_t *parent, const char *cap,
                              lv_obj_t **out_val) {
    lv_obj_t *pair = make_container(parent);
    lv_obj_set_size(pair, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pair, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(pair, 8, 0);
    make_label(pair, &lv_font_overpass_27, BS_HAIR, 1, cap);
    *out_val = make_label(pair, &lv_font_overpass_27, BS_BONE, 1, "--");
}

/**
 * Layout 2 — Broadside. Grotesk poster: spread dateline, huge black time
 * with a rotated PM at its right, thin uppercase condition line, current
 * temp beside a HIGH/LOW block, mono-style metrics row, and a forecast of
 * numerals sized and weighted by temperature.
 */
static void build_layout_broadside(void) {
    /* 30 / 26 / 34 on square (all larger than ui_page_inset()'s 16), the safe
     * inset on round: 105 at 720, 118 at 800. Every block below is
     * LV_PCT(100) or LV_SIZE_CONTENT in a SPACE_BETWEEN column, so the pads
     * are the only geometry that moves. */
    lv_obj_set_style_pad_top(clock_root, LV_MAX(30, ui_page_inset()), 0);
    lv_obj_set_style_pad_bottom(clock_root, LV_MAX(26, ui_page_inset()), 0);
    lv_obj_set_style_pad_hor(clock_root, LV_MAX(34, ui_page_inset()), 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_flex_flow(clock_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_root, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(BS_BG), 0);

    /* ── Dateline: THURSDAY / AUGUST / 21 / 2026 spread across ── */
    lv_obj_t *dateline = make_container(clock_root);
    lv_obj_set_size(dateline, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dateline, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dateline, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lbl_day  = make_label(dateline, &lv_font_overpass_27, BS_BONE, 8, "---");
    lbl_date = make_label(dateline, &lv_font_overpass_27, BS_HAIR, 8, "---");
    lbl_mday = make_label(dateline, &lv_font_overpass_27, BS_BONE, 8, "--");
    lbl_year = make_label(dateline, &lv_font_overpass_27, BS_HAIR, 8, "----");

    /* ── Hero: huge time, rotated PM on its right ── */
    lv_obj_t *hero = make_container(clock_root);
    /* 24 px wider than the content column so the PM mark can sit in the
     * root's horizontal padding, clear of a four-digit time ("10:12"). The
     * pad is the same expression as pad_hor above: 676 on square
     * (720 - 68 + 24), 534 at 720 round, 588 at 800 round. */
    lv_obj_set_size(hero, screen_size() - 2 * LV_MAX(34, ui_page_inset()) + 24, 212);

    lbl_time = make_label(hero, &lv_font_saira_black_300, BS_BONE, -4, "");
    lv_obj_align(lbl_time, LV_ALIGN_LEFT_MID, -6, 0);

    lbl_ampm = make_label(hero, &lv_font_saira_thin_84, BS_SIG, 8, "");
    lv_obj_set_style_transform_pivot_x(lbl_ampm, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(lbl_ampm, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(lbl_ampm, 900, 0);
    /* Rotation does not change the layout box, so the glyphs sit (w-h)/2
     * left of the box's right edge; slide right by that to hug hero's edge. */
    lv_label_set_text(lbl_ampm, "PM");
    lv_obj_update_layout(lbl_ampm);
    int32_t ampm_slide = (lv_obj_get_width(lbl_ampm) - lv_obj_get_height(lbl_ampm)) / 2;
    lv_obj_align(lbl_ampm, LV_ALIGN_RIGHT_MID, ampm_slide, 0);

    /* ── Condition line (sanitized to the thin_84 glyph range) ── */
    lbl_cond = make_label(clock_root, &lv_font_saira_thin_84, BS_BONE, 2, "");

    /* ── Now row: temp + HIGH/LOW block ── */
    lv_obj_t *now_row = make_container(clock_root);
    lv_obj_set_size(now_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(now_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(now_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(now_row, 26, 0);

    lbl_temp = make_label(now_row, &lv_font_saira_black_110, BS_BONE, 0, "");
    lbl_hilo = make_label(now_row, &lv_font_saira_light_46, BS_HAIR, 1, "--");
    lv_label_set_recolor(lbl_hilo, true);   /* values printed in bone */

    /* ── Metrics row ── */
    lv_obj_t *metrics = make_container(clock_root);
    lv_obj_set_size(metrics, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    make_metric_pair(metrics, "HUM",  &lbl_humid_val);
    make_metric_pair(metrics, "DEW",  &lbl_dew_val);
    make_metric_pair(metrics, "WIND", &lbl_wind_val);
    make_metric_pair(metrics, "UV",   &lbl_uv_val);

    /* round: no forecast row on the inset faces (ledger ruling B3); the
     * budget only fits the head, hero, condition, now row and metrics. */
    if (!SCREEN_ROUND) {
        /* ── Forecast: numerals sized+weighted by temp, bottoms aligned ── */
        forecast_row = make_container(clock_root);
        lv_obj_set_size(forecast_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

        for (int i = 0; i < FORECAST_BARS; i++) {
            lv_obj_t *cell = make_container(forecast_row);
            lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_END,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(cell, 8, 0);

            forecast_temp_lbls[i] = make_label(cell, &lv_font_saira_light_46,
                                               BS_HAIR, 0, "--");
            forecast_lbls[i] = make_label(cell, &lv_font_overpass_16,
                                          BS_HAIR, 0, "--");
        }
    }
}

/**
 * Create an Evensong timetable row: key, dot leader, value.
 */
static void make_ev_row(lv_obj_t *parent, int idx, const char *key,
                         lv_obj_t **out_val) {
    /* Long enough to always overflow its clipped span */
    static const char *dots =
        ". . . . . . . . . . . . . . . . . . . . . . . . . . . . . . "
        ". . . . . . . . . . . . . . . . . . . . . . . . . . . . . . ";

    lv_obj_t *row = make_container(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    ev_keys[idx] = make_label(row, &lv_font_overpass_27, EV_MUTE, 2, key);

    ev_leaders[idx] = make_label(row, &lv_font_overpass_16, EV_DIM, 2, dots);
    lv_label_set_long_mode(ev_leaders[idx], LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(ev_leaders[idx], 1);
    lv_obj_set_style_pad_hor(ev_leaders[idx], 12, 0);
    lv_obj_set_style_pad_bottom(ev_leaders[idx], 4, 0);

    *out_val = make_label(row, &lv_font_overpass_27, EV_PARCH, 0, "--");
}

/**
 * Layout 3 — Evensong. Didone word clock: "IT IS" head with a small
 * digital time, the time written out in words, a letterspaced date line,
 * a dot-leader timetable of readings, and a numeral forecast.
 */
static void build_layout_evensong(void) {
    /* Vertical budget (720px, worst case = forecast visible):
     * pad_top 28 + head 59 (39 + 20 reserved gap) + words 273
     * (154 + 116 + 51 - 2*24 pad_row) + date 25 + table 234 (6*39)
     * + forecast 68 (39 + 4 + 25) + pad_bottom 24 = 711, leaving +9
     * slack so SPACE_BETWEEN never goes negative (a negative flex gap
     * stacks the blocks and rams the hour word into "IT IS"). */
    /* Horizontal: 50 on square, the safe inset on round (105 at 720, 118 at
     * 800). Vertical: the block above budgets 659 px of content (this
     * includes the forecast row round no longer builds; the extra slack on
     * round is deliberate). Raising the vertical pads to the safe inset would
     * leave 510 and drive the SPACE_BETWEEN gaps negative, which stacks the
     * blocks. Clamp the vertical pad to the slack that actually exists: 28
     * and 24 on square (the literals win), 30 at 720 round, 70 at 800 round. */
    const int ev_pad_v = LV_MIN(ui_page_inset(), (screen_size() - 659) / 2);
    lv_obj_set_style_pad_top(clock_root, LV_MAX(28, ev_pad_v), 0);
    lv_obj_set_style_pad_bottom(clock_root, LV_MAX(24, ev_pad_v), 0);
    lv_obj_set_style_pad_hor(clock_root, LV_MAX(50, ui_page_inset()), 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_flex_flow(clock_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_root, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(EV_BG), 0);

    /* ── Head row: IT IS ... digital time ── */
    lv_obj_t *head = make_container(clock_root);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    /* Reserve the gap below the head inside its own box so the hour
     * word's cap height always starts well under the "IT IS" baseline,
     * whatever the flex gap resolves to. */
    lv_obj_set_style_pad_bottom(head, 20, 0);

    ev_itis = make_label(head, &lv_font_overpass_16, EV_BRASS, 8, "IT IS");

    lv_obj_t *digital = make_container(head);
    lv_obj_set_size(digital, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(digital, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(digital, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(digital, 8, 0);
    lbl_time = make_label(digital, &lv_font_overpass_27, EV_PARCH, 2, "");
    lbl_ampm = make_label(digital, &lv_font_overpass_27, EV_PARCH, 2, "");

    /* ── Words block ── */
    lv_obj_t *words = make_container(clock_root);
    lv_obj_set_size(words, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(words, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* Tighten the hour-word seam: the hour words are caps-only, so
     * bodoni_black_150's 38px descender band is dead space. A -24
     * row gap reclaims it; ev_w2's +24 pad_bottom cancels the same
     * gap at the w2/w3 seam (minute words do use descenders). */
    lv_obj_set_style_pad_row(words, -24, 0);

    ev_w1 = make_label(words, &lv_font_bodoni_black_150, EV_PARCH, 1, "");
    ev_w2 = make_label(words, &lv_font_bodoni_ital_90, EV_PARCH, 1, "");
    lv_obj_set_style_pad_bottom(ev_w2, 24, 0);
    ev_w3 = make_label(words, &lv_font_bodoni_ital_44, EV_BRASS, 1, "");
    lv_obj_set_style_pad_top(ev_w3, 6, 0);

    /* ── Date line ── */
    lbl_date = make_label(clock_root, &lv_font_overpass_16, EV_MUTE, 3, "---");

    /* ── Timetable with dot leaders ── */
    lv_obj_t *table = make_container(clock_root);
    lv_obj_set_size(table, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(table, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(table, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(table, 0, 0);

    make_ev_row(table, 0, "CONDITION",   &lbl_cond);
    /* Long conditions must not squeeze out the dot leader */
    lv_obj_set_style_max_width(lbl_cond, 340, 0);
    lv_label_set_long_mode(lbl_cond, LV_LABEL_LONG_DOT);
    make_ev_row(table, 1, "TEMPERATURE", &lbl_temp);
    make_ev_row(table, 2, "HUMIDITY",    &lbl_humid_val);
    make_ev_row(table, 3, "DEW POINT",   &lbl_dew_val);
    make_ev_row(table, 4, "WIND",        &lbl_wind_val);
    make_ev_row(table, 5, "UV INDEX",    &lbl_uv_val);

    /* round: no forecast row on the inset faces (ledger ruling B3); the 659 px
     * budget above already accounts for it (see the pad clamp above). */
    if (!SCREEN_ROUND) {
        /* ── Forecast: temps row over hours row ── */
        forecast_row = make_container(clock_root);
        lv_obj_set_size(forecast_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(forecast_row, 4, 0);
        lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *temps_row = make_container(forecast_row);
        lv_obj_set_size(temps_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(temps_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(temps_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_t *hours_row = make_container(forecast_row);
        lv_obj_set_size(hours_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hours_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hours_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        for (int i = 0; i < FORECAST_BARS; i++) {
            forecast_temp_lbls[i] = make_label(temps_row, &lv_font_overpass_27,
                                               EV_PARCH, 0, "--");
            lv_obj_set_width(forecast_temp_lbls[i], 56);
            lv_obj_set_style_text_align(forecast_temp_lbls[i],
                                        LV_TEXT_ALIGN_CENTER, 0);
            forecast_lbls[i] = make_label(hours_row, &lv_font_overpass_16,
                                          EV_MUTE, 0, "--");
            lv_obj_set_width(forecast_lbls[i], 56);
            lv_obj_set_style_text_align(forecast_lbls[i], LV_TEXT_ALIGN_CENTER, 0);
        }
    }
}

/**
 * One Blueprint annotation row: leader dot + leader line + value label.
 * Returned label is the value; siblings are chrome (collected for theme).
 */
lv_obj_t *make_blu_callout(int x, int y, const lv_font_t *font) {
    lv_obj_t *row = make_container(clock_root);
    lv_obj_set_size(row, 218, LV_SIZE_CONTENT);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *dot = make_bg_rect(row, 0, 0, 6, 6, BLU_LINE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    reg_ter_bg(dot);
    lv_obj_t *lead = make_bg_rect(row, 0, 0, 26, 1, BLU_DIM);
    reg_dim_bg(lead);

    return make_label(row, font, BLU_HI, 0, "--");
}

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * Layout 4 — Blueprint. Cyanotype drafting sheet: double frame with
 * registration crosshairs, stencil hero time with an arrowed dimension
 * chain, leader-dot annotation column, and a dimensioned elevation
 * bar chart in the lower half.
 */
static void build_layout_blueprint(void) {
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(BLU_BG), 0);

    /* ── Sheet frames ── */
    blu_frame_o = make_container(clock_root);
    lv_obj_set_size(blu_frame_o, screen_size() - 36, screen_size() - 36);
    lv_obj_set_pos(blu_frame_o, 18, 18);
    lv_obj_set_style_border_width(blu_frame_o, 2, 0);
    lv_obj_set_style_border_color(blu_frame_o, lv_color_hex(BLU_LINE), 0);
    lv_obj_set_style_border_opa(blu_frame_o, LV_OPA_COVER, 0);

    blu_frame_i = make_container(clock_root);
    lv_obj_set_size(blu_frame_i, screen_size() - 88, screen_size() - 88);
    lv_obj_set_pos(blu_frame_i, 44, 44);
    lv_obj_set_style_border_width(blu_frame_i, 1, 0);
    lv_obj_set_style_border_color(blu_frame_i, lv_color_hex(BLU_DIM), 0);
    lv_obj_set_style_border_opa(blu_frame_i, LV_OPA_COVER, 0);

    /* ── Registration crosshairs ── */
    static const int16_t reg_x[4] = { 22, 681, 22, 681 };
    static const int16_t reg_y[4] = { 22, 22, 681, 681 };
    for (int i = 0; i < 4; i++) {
        reg_ter_bg(make_bg_rect(clock_root, reg_x[i] + 8, reg_y[i],
                                1, 17, BLU_LINE));
        reg_ter_bg(make_bg_rect(clock_root, reg_x[i], reg_y[i] + 8,
                                17, 1, BLU_LINE));
    }

    /* ── Top line: date left, revision right ── */
    lbl_date = make_label(clock_root, &lv_font_overpass_27, BLU_LINE, 3, "---");
    lv_obj_set_pos(lbl_date, 64, 52);
    lv_obj_t *rev = make_label(clock_root, &lv_font_overpass_27, BLU_DIM, 3,
                               "REV C");
    lv_obj_align(rev, LV_ALIGN_TOP_RIGHT, -64, 52);
    reg_dim_lbl(rev);

    /* ── Hero time + AM chip ── */
    lbl_time = make_label(clock_root, &lv_font_stencil_190, BLU_HI, 2, "");
    lv_obj_set_pos(lbl_time, 56, 100);

    lbl_ampm = make_label(clock_root, &lv_font_overpass_27, BLU_LINE, 2, "");
    lv_obj_align(lbl_ampm, LV_ALIGN_TOP_RIGHT, -64, 104);
    lv_obj_set_style_border_width(lbl_ampm, 1, 0);
    lv_obj_set_style_border_color(lbl_ampm, lv_color_hex(BLU_LINE), 0);
    lv_obj_set_style_border_opa(lbl_ampm, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(lbl_ampm, 4, 0);
    lv_obj_set_style_pad_hor(lbl_ampm, 8, 0);

    /* ── Dimension chain under the digits ── */
    reg_dim_bg(make_bg_rect(clock_root, 64, 262, 1, 26, BLU_DIM));   /* ext L */
    reg_dim_bg(make_bg_rect(clock_root, 458, 262, 1, 26, BLU_DIM));  /* ext R */
    reg_ter_bg(make_bg_rect(clock_root, 75, 296, 372, 1, BLU_LINE)); /* line */

    for (int a = 0; a < 2; a++) {
        blu_arrow[a] = lv_line_create(clock_root);
        lv_obj_clear_flag(blu_arrow[a], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(blu_arrow[a], 2, 0);
        lv_obj_set_style_line_color(blu_arrow[a], lv_color_hex(BLU_LINE), 0);
    }
    lv_line_set_points(blu_arrow[0], blu_arr_l_pts, 3);
    lv_obj_set_pos(blu_arrow[0], 65, 292);
    lv_line_set_points(blu_arrow[1], blu_arr_r_pts, 3);
    lv_obj_set_pos(blu_arrow[1], 447, 292);

    blu_dimlbl = make_label(clock_root, &lv_font_overpass_27, BLU_LINE, 1,
                            "---");
    lv_obj_set_pos(blu_dimlbl, 64, 306);
    lv_obj_set_width(blu_dimlbl, 394);
    lv_obj_set_style_text_align(blu_dimlbl, LV_TEXT_ALIGN_CENTER, 0);
    reg_ter_lbl(blu_dimlbl);

    /* ── Annotation column (leader-dot callouts) ── */
    lbl_temp      = make_blu_callout(472, 150, &lv_font_saira_light_46);
    lbl_cond      = make_blu_callout(472, 216, &lv_font_overpass_27);
    lv_obj_set_width(lbl_cond, 172);
    lv_label_set_long_mode(lbl_cond, LV_LABEL_LONG_DOT);
    lbl_hilo      = make_blu_callout(472, 258, &lv_font_overpass_27);
    lbl_humid_val = make_blu_callout(472, 300, &lv_font_overpass_27);
    lbl_dew_val   = make_blu_callout(472, 342, &lv_font_overpass_27);
    lbl_wind_val  = make_blu_callout(472, 384, &lv_font_overpass_27);
    lbl_uv_val    = make_blu_callout(472, 426, &lv_font_overpass_27);

    /* ── Dimensioned elevation chart ── */
    forecast_row = make_container(clock_root);
    lv_obj_set_size(forecast_row, 608, 252);
    lv_obj_set_pos(forecast_row, 56, 424);
    lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(forecast_row, &lv_font_overpass_27,
                                 BLU_LINE, 2, "TEMP / 10 HR");
    lv_obj_set_pos(title, 8, 0);
    reg_ter_lbl(title);

    blu_plot = make_container(forecast_row);
    lv_obj_set_size(blu_plot, 592, 158);
    lv_obj_set_pos(blu_plot, 8, 44);
    lv_obj_set_style_border_width(blu_plot, 1, 0);
    lv_obj_set_style_border_side(blu_plot, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(blu_plot, lv_color_hex(BLU_LINE), 0);
    lv_obj_set_style_border_opa(blu_plot, LV_OPA_COVER, 0);

    for (int i = 0; i < FORECAST_BARS; i++) {
        lv_obj_t *bar = make_bg_rect(blu_plot, 8 + 59 * i, 145, 26, 12,
                                     BLU_DIM);
        forecast_bars[i] = bar;

        /* Baseline extension tick + hour label (forecast_row coords) */
        reg_dim_bg(make_bg_rect(forecast_row, 29 + 59 * i, 203, 1, 6,
                                BLU_DIM));
        forecast_lbls[i] = make_label(forecast_row, &lv_font_overpass_27,
                                      BLU_DIM, 0, "--");
        lv_obj_set_pos(forecast_lbls[i], 1 + 59 * i, 212);
        lv_obj_set_width(forecast_lbls[i], 56);
        lv_obj_set_style_text_align(forecast_lbls[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    /* MAX dimension callout — repositioned over the tallest bar */
    blu_peak_tick = make_bg_rect(forecast_row, 16, 73, 26, 1, BLU_LINE);
    blu_max_lbl = make_label(forecast_row, &lv_font_overpass_27, BLU_HI, 0,
                             "MAX --");
    lv_obj_set_pos(blu_max_lbl, 50, 41);
    lv_obj_set_width(blu_max_lbl, 140);
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * Layout 5 — Transit Line. Metro map: red roundel with the time in a navy
 * bar, right column headed by the live weekday, and the day as one metro
 * line snaking three runs with two U-bends, temperature-colored segments
 * and ten stations. The roundel keeps to x < 360 so the right-aligned
 * column (x >= 370) never collides with the ring or bar.
 */
static void build_layout_transit(void) {
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(MET_BG), 0);

    /* ── Roundel: red ring + navy bar with the time. Ring 200px with a
     * 340px bar through its center, 70px symmetric overhang each side
     * (same disc/bar proportion as the original 240/340/50). ── */
    met_ring5 = make_container(clock_root);
    lv_obj_set_size(met_ring5, 200, 200);
    lv_obj_set_pos(met_ring5, 90, 30);
    lv_obj_set_style_radius(met_ring5, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(met_ring5, 20, 0);
    lv_obj_set_style_border_color(met_ring5, lv_color_hex(MET_RED), 0);
    lv_obj_set_style_border_opa(met_ring5, LV_OPA_COVER, 0);

    met_bar = make_container(clock_root);
    lv_obj_set_size(met_bar, 340, 100);
    lv_obj_set_pos(met_bar, 20, 80);
    lv_obj_set_style_bg_color(met_bar, lv_color_hex(MET_NAVY), 0);
    lv_obj_set_style_bg_opa(met_bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(met_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(met_bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(met_bar, 12, 0);

    lbl_time = make_label(met_bar, &lv_font_hanken_black_96, MET_INK, -2, "");
    lbl_ampm = make_label(met_bar, &lv_font_overpass_27, MET_GOLD, 1, "");
    lv_obj_set_style_pad_top(lbl_ampm, 30, 0);

    /* ── Right column: weekday header, date, accent line, readings ── */
    met_name = make_label(clock_root, &lv_font_hanken_black_40, MET_INK, 3,
                          "");
    lv_obj_set_style_text_align(met_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(met_name, LV_ALIGN_TOP_RIGHT, -20, 40);

    lbl_date = make_label(clock_root, &lv_font_overpass_27, MET_DIM, 2, "---");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_RIGHT, -20, 100);

    lbl_cond = make_label(clock_root, &lv_font_overpass_27, MET_GOLD, 1, "--");
    lv_obj_set_width(lbl_cond, 310);
    lv_obj_set_style_text_align(lbl_cond, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(lbl_cond, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_cond, LV_ALIGN_TOP_RIGHT, -20, 146);

    lv_obj_t *stats = make_container(clock_root);
    lv_obj_set_size(stats, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(stats, 4, 0);
    lbl_humid_val = make_label(stats, &lv_font_overpass_27, MET_DIM, 1, "--");
    lbl_dew_val   = make_label(stats, &lv_font_overpass_27, MET_DIM, 1, "--");
    lv_obj_align(stats, LV_ALIGN_TOP_RIGHT, -20, 192);

    /* ── Route map (hidden until forecast data arrives) ── */
    forecast_row = make_container(clock_root);
    lv_obj_set_size(forecast_row, screen_size(), screen_size());
    lv_obj_set_pos(forecast_row, 0, 0);
    lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

    /* Runs (one segment per station, colored by its temperature) */
    for (int i = 0; i < FORECAST_BARS; i++) {
        int row = m5_row_of(i);
        met_segs[i] = make_bg_rect(forecast_row, m5_seg_x[i], m5_line_y[row],
                                   m5_seg_w[i], 14, MET_TEAL);
        if (i == 0) {
            lv_obj_set_style_radius(met_segs[i], 7, 0);
        }
    }

    /* U-bends: right (A->B) and left (B->C), 60px centerline radius */
    met_arcs[0] = make_bend_arc(forecast_row, 533, 408, 134, true, MET_ORANGE);
    met_arcs[1] = make_bend_arc(forecast_row, 23, 528, 134, false, MET_TEAL);

    /* Terminus cap at the end of run C */
    met_terminus = make_bg_rect(forecast_row, 424, 633, 14, 44, MET_INK);

    /* Stations: dots on the runs, hour + temp labels beside them */
    for (int i = 0; i < FORECAST_BARS; i++) {
        int row = m5_row_of(i);
        int cy = m5_line_y[row] + 7;
        met_dots[i] = make_station_dot(forecast_row, m5_sx[i], cy,
                                       (i == 0) ? 22 : 18,
                                       (i == 0) ? 6 : 5,
                                       (i == 0) ? MET_RED : MET_TEAL);

        int band = m5_band_y[row];
        forecast_lbls[i] = make_label(forecast_row, &lv_font_overpass_27,
                                      MET_DIM, 0, "--");
        /* 64px: "10a" at overpass_27 is ~50px; 48 wrapped it */
        lv_obj_set_pos(forecast_lbls[i], m5_sx[i] - 88, band + 6);
        lv_obj_set_width(forecast_lbls[i], 64);
        lv_obj_set_style_text_align(forecast_lbls[i], LV_TEXT_ALIGN_RIGHT, 0);

        forecast_temp_lbls[i] = make_label(forecast_row,
                                           &lv_font_hanken_bold_44,
                                           MET_INK, 0, "--");
        lv_obj_set_pos(forecast_temp_lbls[i], m5_sx[i] - 16, band);
    }

    /* YOU ARE HERE pill above the first station */
    met_pill = make_label(forecast_row, &lv_font_overpass_27, 0xFFFFFF, 2,
                          "YOU ARE HERE");
    lv_obj_set_style_bg_color(met_pill, lv_color_hex(MET_RED), 0);
    lv_obj_set_style_bg_opa(met_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(met_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_ver(met_pill, 6, 0);
    lv_obj_set_style_pad_hor(met_pill, 12, 0);
    lv_obj_set_pos(met_pill, 30, 296);

    /* PEAK tag — repositioned to the hottest station */
    met_peak_lbl = make_label(forecast_row, &lv_font_overpass_27, MET_RED, 2,
                              "PEAK");
    /* 100px: "PEAK" at overpass_27 + letter_space 2 is ~70px; 70 wrapped */
    lv_obj_set_width(met_peak_lbl, 100);
    lv_obj_set_style_text_align(met_peak_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(met_peak_lbl, 0, 0);

    /* Sunrise diamond marker + label, shown when the forecast crosses 6a */
    met_sun_dia = make_bg_rect(forecast_row, 0, 0, 15, 15, MET_GOLD);
    lv_obj_set_style_transform_pivot_x(met_sun_dia, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(met_sun_dia, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(met_sun_dia, 450, 0);
    lv_obj_add_flag(met_sun_dia, LV_OBJ_FLAG_HIDDEN);

    met_sun_lbl = make_label(forecast_row, &lv_font_overpass_27, MET_DIM, 1,
                             "SUNRISE");
    lv_obj_set_width(met_sun_lbl, 120);
    lv_obj_set_style_text_align(met_sun_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(met_sun_lbl, LV_OBJ_FLAG_HIDDEN);
    reg_dim_lbl(met_sun_lbl);
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * One Night Network legend swatch chip (colored by apply_theme).
 */
static void make_met_chip(lv_obj_t *parent, int w, int h, int idx) {
    lv_obj_t *c = make_bg_rect(parent, 0, 0, w, h, MET_TEAL);
    lv_obj_set_style_radius(c, 3, 0);
    met_chips[idx] = c;
}

/**
 * Layout 6 — Night Network. Two-line map: the vertical Temperature line
 * crosses the horizontal Conditions line; corner block names the live
 * weekday; legend panel with line keys and temperature-band swatches.
 */
static void build_layout_network(void) {
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(MET_BG), 0);

    /* ── Corner name block ── */
    met_panel = make_container(clock_root);
    lv_obj_set_size(met_panel, 360, LV_SIZE_CONTENT);
    lv_obj_set_pos(met_panel, 24, 20);
    lv_obj_set_style_bg_color(met_panel, lv_color_hex(MET_BOX), 0);
    lv_obj_set_style_bg_opa(met_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(met_panel, 2, 0);
    lv_obj_set_style_border_color(met_panel, lv_color_hex(MET_EDGE), 0);
    lv_obj_set_style_border_opa(met_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(met_panel, 10, 0);
    lv_obj_set_flex_flow(met_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(met_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(met_panel, 20, 0);
    lv_obj_set_style_pad_ver(met_panel, 16, 0);
    lv_obj_set_style_pad_row(met_panel, 4, 0);

    lv_obj_t *trow = make_container(met_panel);
    lv_obj_set_size(trow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(trow, 10, 0);
    lbl_time = make_label(trow, &lv_font_hanken_black_96, MET_INK, -2, "");
    lbl_ampm = make_label(trow, &lv_font_overpass_27, MET_GOLD, 1, "");
    lv_obj_set_style_pad_bottom(lbl_ampm, 4, 0);

    met_name = make_label(met_panel, &lv_font_hanken_black_40, MET_INK, 2,
                          "");
    lv_obj_set_style_pad_top(met_name, 12, 0);   /* gap under the time */

    lbl_date = make_label(met_panel, &lv_font_overpass_27, MET_DIM, 2, "---");

    lbl_cond = make_label(met_panel, &lv_font_overpass_27, MET_GOLD, 1, "--");
    lv_obj_set_width(lbl_cond, LV_PCT(100));
    lv_label_set_long_mode(lbl_cond, LV_LABEL_LONG_DOT);

    /* ── Network map (hidden until forecast data arrives) ── */
    forecast_row = make_container(clock_root);
    lv_obj_set_size(forecast_row, screen_size(), screen_size());
    lv_obj_set_pos(forecast_row, 0, 0);
    lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

    /* Conditions line (horizontal, blue) with end ticks. It crosses the
     * temperature line between stations so it never strikes through the
     * fixed station labels; the peak double-ring moves along the
     * temperature line to the actual hottest hour. */
    met_cline[0] = make_bg_rect(forecast_row, 60, 365, 630, 14, MET_BLUE);
    met_cline[1] = make_bg_rect(forecast_row, 53, 350, 14, 44, MET_BLUE);
    met_cline[2] = make_bg_rect(forecast_row, 683, 350, 14, 44, MET_BLUE);

    /* Conditions stations: dot + value + caption, below the line */
    static const int16_t c_cx[MET_CSTATIONS] = { 85, 160, 235, 310, 385 };
    static const char *c_caps[MET_CSTATIONS] = {
        "HIGH", "LOW", "HUM", "DEW", "WIND"
    };
    lv_obj_t *c_vals[MET_CSTATIONS];
    for (int k = 0; k < MET_CSTATIONS; k++) {
        met_cdots[k] = make_station_dot(forecast_row, c_cx[k], 372, 10, 4,
                                        MET_BLUE);
        lv_obj_t *cell = make_container(forecast_row);
        lv_obj_set_size(cell, 90, LV_SIZE_CONTENT);
        lv_obj_set_pos(cell, c_cx[k] - 45, 393);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        c_vals[k] = make_label(cell, &lv_font_overpass_27, MET_INK, 0, "--");
        lv_obj_t *cap = make_label(cell, &lv_font_overpass_27, MET_DIM, 1,
                                   c_caps[k]);
        reg_dim_lbl(cap);
    }
    met_hi_val    = c_vals[0];
    met_lo_val    = c_vals[1];
    lbl_humid_val = c_vals[2];
    lbl_dew_val   = c_vals[3];
    lbl_wind_val  = c_vals[4];

    /* Temperature line (vertical) with end ticks */
    met_tick_a = make_bg_rect(forecast_row, 606, 21, 44, 14, MET_TEAL);
    met_tick_b = make_bg_rect(forecast_row, 606, 685, 44, 14, MET_TEAL);
    for (int i = 0; i < FORECAST_BARS; i++) {
        met_segs[i] = make_bg_rect(forecast_row, 621, 35 + 65 * i, 14, 65,
                                   MET_TEAL);
    }

    /* Temperature stations: dot on the line, hour + temp to its left */
    for (int i = 0; i < FORECAST_BARS; i++) {
        int sy = 67 + 65 * i;
        met_dots[i] = make_station_dot(forecast_row, 628, sy, 10, 4, MET_TEAL);

        forecast_lbls[i] = make_label(forecast_row, &lv_font_overpass_27,
                                      MET_DIM, 0, "--");
        lv_obj_set_pos(forecast_lbls[i], 468, sy - 19);
        lv_obj_set_width(forecast_lbls[i], 56);
        lv_obj_set_style_text_align(forecast_lbls[i], LV_TEXT_ALIGN_RIGHT, 0);

        forecast_temp_lbls[i] = make_label(forecast_row,
                                           &lv_font_hanken_bold_44,
                                           MET_INK, 0, "--");
        lv_obj_set_pos(forecast_temp_lbls[i], 534, sy - 15);
        lv_obj_set_width(forecast_temp_lbls[i], 86);
        lv_obj_set_style_text_align(forecast_temp_lbls[i],
                                    LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* Peak interchange: double ring + PEAK tag, moved to the hottest hour */
    met_peak_ring = make_container(forecast_row);
    lv_obj_set_size(met_peak_ring, 36, 36);
    lv_obj_set_pos(met_peak_ring, 610, 49);
    lv_obj_set_style_radius(met_peak_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(met_peak_ring, 5, 0);
    lv_obj_set_style_border_color(met_peak_ring, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(met_peak_ring, LV_OPA_COVER, 0);

    met_peak_core = make_bg_rect(forecast_row, 622, 61, 12, 12, 0xFFFFFF);
    lv_obj_set_style_radius(met_peak_core, LV_RADIUS_CIRCLE, 0);

    met_peak_lbl = make_label(forecast_row, &lv_font_overpass_16, MET_RED, 1,
                              "PEAK");
    lv_obj_set_pos(met_peak_lbl, 654, 57);

    /* ── Legend panel ── */
    met_legend = make_container(forecast_row);
    lv_obj_set_size(met_legend, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_pos(met_legend, 24, 506);
    lv_obj_set_style_bg_color(met_legend, lv_color_hex(MET_BOX), 0);
    lv_obj_set_style_bg_opa(met_legend, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(met_legend, 2, 0);
    lv_obj_set_style_border_color(met_legend, lv_color_hex(MET_EDGE), 0);
    lv_obj_set_style_border_opa(met_legend, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(met_legend, 10, 0);
    lv_obj_set_flex_flow(met_legend, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(met_legend, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(met_legend, 16, 0);
    lv_obj_set_style_pad_ver(met_legend, 8, 0);
    lv_obj_set_style_pad_row(met_legend, 2, 0);

    /* Line keys */
    lv_obj_t *k1 = make_container(met_legend);
    lv_obj_set_size(k1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(k1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(k1, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(k1, 4, 0);
    for (int b = 0; b < 4; b++) {
        make_met_chip(k1, 22, 10, b);
    }
    lv_obj_t *k1t = make_label(k1, &lv_font_overpass_27, MET_INK, 1,
                               "TEMPERATURE");
    lv_obj_set_style_pad_left(k1t, 10, 0);
    reg_ink_lbl(k1t);

    lv_obj_t *k2 = make_container(met_legend);
    lv_obj_set_size(k2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(k2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(k2, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(k2, 4, 0);
    make_met_chip(k2, 88, 10, 4);
    lv_obj_t *k2t = make_label(k2, &lv_font_overpass_27, MET_INK, 1,
                               "CONDITIONS");
    lv_obj_set_style_pad_left(k2t, 10, 0);
    reg_ink_lbl(k2t);

    /* Temperature band swatches, two per row */
    for (int r = 0; r < 2; r++) {
        lv_obj_t *br = make_container(met_legend);
        lv_obj_set_size(br, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(br, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(br, 8, 0);
        for (int c = 0; c < 2; c++) {
            int band = r * 2 + c;
            make_met_chip(br, 14, 14, 5 + band);
            met_band_lbl[band] = make_label(br, &lv_font_overpass_27,
                                            MET_DIM, 0, "--");
            if (c == 0) {
                lv_obj_set_width(met_band_lbl[band], 190);
            }
        }
    }
}
#endif /* !CONFIG_NINA_FAMILY_ROUND */

/* ── Forecast styling for the new layouts ────────────────────────────── */

/**
 * Style the forecast cells for layouts 1-3 from live weather data:
 * Console bar heights + amber-hot coloring and the RANGE caption,
 * Broadside numeral font/color by temperature tercile, Evensong numeral
 * color by min/max. Called from both the update and theme paths so a
 * theme switch restyles immediately. @p wd must be valid.
 */
static void restyle_forecast(const weather_data_t *wd, bool red_night) {
    clk_palette_t p;
    resolve_palette(&p, red_night);

    float t_min = wd->hourly_temps[0];
    float t_max = wd->hourly_temps[0];
    for (int i = 1; i < FORECAST_BARS; i++) {
        if (wd->hourly_temps[i] < t_min) t_min = wd->hourly_temps[i];
        if (wd->hourly_temps[i] > t_max) t_max = wd->hourly_temps[i];
    }
    float t_range = t_max - t_min;
    if (t_range < 1.0f) t_range = 1.0f;

    if (s_layout == 1 && console_range) {
        char rbuf[48];
        snprintf(rbuf, sizeof(rbuf), "RANGE %.0f-%.0f", t_min, t_max);
        lv_label_set_text(console_range, rbuf);
    }

    for (int i = 0; i < FORECAST_BARS; i++) {
        float tv = wd->hourly_temps[i];
        float frac = (tv - t_min) / t_range;
        bool hottest = (tv >= t_max - 0.01f);

        char nbuf[16];
        snprintf(nbuf, sizeof(nbuf), "%.0f", tv);

        if (s_layout == 1) {
            uint32_t col = hottest ? p.accent : p.dim;
            if (forecast_bars[i]) {
                int h = CON_BAR_MIN +
                        (int)(frac * (CON_BAR_MAX - CON_BAR_MIN) + 0.5f);
                lv_obj_set_height(forecast_bars[i], h);
                lv_obj_set_style_bg_color(forecast_bars[i], lv_color_hex(col), 0);
            }
            if (forecast_temp_lbls[i]) {
                lv_label_set_text(forecast_temp_lbls[i], nbuf);
                lv_obj_set_style_text_color(forecast_temp_lbls[i],
                                            lv_color_hex(col), 0);
            }
        } else if (s_layout == 2) {
            if (forecast_temp_lbls[i]) {
                const lv_font_t *font = &lv_font_saira_light_46;
                uint32_t col;
                if (frac >= 0.667f) {
                    /* saira_black_66 carries digits only — negative temps
                     * fall back to the full-ASCII light face */
                    if (tv >= 0.0f) font = &lv_font_saira_black_66;
                    col = hottest ? p.accent : p.primary;
                } else if (frac >= 0.333f) {
                    col = p.tertiary;
                } else {
                    col = p.rule;   /* dim-step gray */
                }
                lv_label_set_text(forecast_temp_lbls[i], nbuf);
                lv_obj_set_style_text_font(forecast_temp_lbls[i], font, 0);
                lv_obj_set_style_text_color(forecast_temp_lbls[i],
                                            lv_color_hex(col), 0);
            }
        } else if (s_layout == 3) {
            if (forecast_temp_lbls[i]) {
                uint32_t col = p.primary;
                if (hottest) col = p.accent;
                else if (tv <= t_min + 0.01f) col = p.rule;
                lv_label_set_text(forecast_temp_lbls[i], nbuf);
                lv_obj_set_style_text_color(forecast_temp_lbls[i],
                                            lv_color_hex(col), 0);
            }
        } else if (s_layout == 4) {
            /* Blueprint elevation bars: tallest in ink, rest dim */
            if (forecast_bars[i]) {
                int h = 12 + (int)(frac * 108.0f + 0.5f);
                lv_obj_set_height(forecast_bars[i], h);
                lv_obj_set_y(forecast_bars[i], 157 - h);
                lv_obj_set_style_bg_color(forecast_bars[i],
                    lv_color_hex(hottest ? p.primary : p.dim), 0);
            }
        } else if (s_layout == 5 || s_layout == 6) {
            /* Metro maps: quartile band colors segment, dot ring, temp */
            int band = (int)(frac * 4.0f);
            uint32_t col = met_band_color(band, red_night);
            if (met_segs[i]) {
                lv_obj_set_style_bg_color(met_segs[i], lv_color_hex(col), 0);
            }
            if (met_dots[i] && i != 0) {
                /* Station 0 keeps its red "you are here" ring on Transit */
                lv_obj_set_style_border_color(met_dots[i],
                                              lv_color_hex(col), 0);
            }
            if (met_dots[0] && i == 0 && s_layout == 6) {
                lv_obj_set_style_border_color(met_dots[0],
                                              lv_color_hex(col), 0);
            }
            if (forecast_temp_lbls[i]) {
                char dbuf[48];
                snprintf(dbuf, sizeof(dbuf), "%.0f\xc2\xb0", tv);
                lv_label_set_text(forecast_temp_lbls[i], dbuf);
                lv_obj_set_style_text_color(forecast_temp_lbls[i],
                                            lv_color_hex(col), 0);
            }
        }
    }

    /* ── Per-layout elements that follow the peak / sunrise ── */

    int i_pk = 0;
    for (int i = 1; i < FORECAST_BARS; i++) {
        if (wd->hourly_temps[i] > wd->hourly_temps[i_pk]) i_pk = i;
    }

    if (s_layout == 4 && blu_peak_tick && blu_max_lbl) {
        /* Dimension tick + MAX callout hugging the tallest bar. Same frac
         * math as the bars (h = 12..120, top at row y 201-h), so a flat
         * forecast (all bars 12px) keeps the callout on the bar tops
         * instead of floating at the full-height position. */
        int bx = 16 + 59 * i_pk;          /* bar x in forecast_row coords */
        float frac_pk = (wd->hourly_temps[i_pk] - t_min) / t_range;
        int h_pk = 12 + (int)(frac_pk * 108.0f + 0.5f);
        int ty = (201 - h_pk) - 8;        /* tick 8px above the bar top */
        char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "MAX %.0f\xc2\xb0",
                 wd->hourly_temps[i_pk]);
        lv_label_set_text(blu_max_lbl, mbuf);
        lv_obj_set_pos(blu_peak_tick, bx, ty);
        if (i_pk < 5) {
            lv_obj_set_pos(blu_max_lbl, bx + 34, ty - 32);
            lv_obj_set_style_text_align(blu_max_lbl, LV_TEXT_ALIGN_LEFT, 0);
        } else {
            lv_obj_set_pos(blu_max_lbl, bx - 148, ty - 32);
            lv_obj_set_style_text_align(blu_max_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    if (s_layout == 5) {
        if (met_peak_lbl) {
            int row = m5_row_of(i_pk);
            /* -50 = half the 100px label width, centered on the station */
            lv_obj_set_pos(met_peak_lbl, m5_sx[i_pk] - 50, m5_tag_y[row]);
        }
        /* U-bend colors: arrival station's band */
        if (met_arcs[0]) {
            float f4 = (wd->hourly_temps[4] - t_min) / t_range;
            lv_obj_set_style_arc_color(met_arcs[0],
                lv_color_hex(met_band_color((int)(f4 * 4.0f), red_night)),
                LV_PART_MAIN);
        }
        if (met_arcs[1]) {
            float f8 = (wd->hourly_temps[8] - t_min) / t_range;
            lv_obj_set_style_arc_color(met_arcs[1],
                lv_color_hex(met_band_color((int)(f8 * 4.0f), red_night)),
                LV_PART_MAIN);
        }
        /* Sunrise diamond between the stations that cross ~6a, if any */
        if (met_sun_dia && met_sun_lbl) {
            int sr = -1;
            for (int i = 1; i < FORECAST_BARS; i++) {
                if (wd->hourly_hours[i - 1] < 6 && wd->hourly_hours[i] >= 6 &&
                    wd->hourly_hours[i] <= 9 &&
                    m5_row_of(i - 1) == m5_row_of(i)) {
                    sr = i;
                    break;
                }
            }
            if (sr > 0) {
                int row = m5_row_of(sr);
                int mx = (m5_sx[sr - 1] + m5_sx[sr]) / 2;
                lv_obj_set_pos(met_sun_dia, mx - 8, m5_line_y[row]);
                lv_obj_set_pos(met_sun_lbl, mx - 60, m5_tag_y[row]);
                lv_obj_remove_flag(met_sun_dia, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(met_sun_lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(met_sun_dia, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(met_sun_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_layout == 6) {
        int yp = 67 + 65 * i_pk;
        if (met_peak_ring) lv_obj_set_pos(met_peak_ring, 610, yp - 18);
        if (met_peak_core) lv_obj_set_pos(met_peak_core, 622, yp - 6);
        if (met_peak_lbl)  lv_obj_set_pos(met_peak_lbl, 654, yp - 10);
        /* Temperature-line end ticks follow their adjacent stations */
        if (met_tick_a) {
            float f0 = (wd->hourly_temps[0] - t_min) / t_range;
            lv_obj_set_style_bg_color(met_tick_a,
                lv_color_hex(met_band_color((int)(f0 * 4.0f), red_night)), 0);
        }
        if (met_tick_b) {
            float f9 = (wd->hourly_temps[FORECAST_BARS - 1] - t_min) / t_range;
            lv_obj_set_style_bg_color(met_tick_b,
                lv_color_hex(met_band_color((int)(f9 * 4.0f), red_night)), 0);
        }
        /* Legend band thresholds from the fetch range quartiles */
        if (met_band_lbl[0]) {
            float q1 = t_min + 0.25f * t_range;
            float q2 = t_min + 0.50f * t_range;
            float q3 = t_min + 0.75f * t_range;
            char bbuf[64];
            snprintf(bbuf, sizeof(bbuf), "UNDER %.0f\xc2\xb0", q1);
            lv_label_set_text(met_band_lbl[0], bbuf);
            snprintf(bbuf, sizeof(bbuf), "%.0f-%.0f\xc2\xb0", q1, q2);
            lv_label_set_text(met_band_lbl[1], bbuf);
            snprintf(bbuf, sizeof(bbuf), "%.0f-%.0f\xc2\xb0", q2, q3);
            lv_label_set_text(met_band_lbl[2], bbuf);
            snprintf(bbuf, sizeof(bbuf), "%.0f\xc2\xb0+", q3);
            lv_label_set_text(met_band_lbl[3], bbuf);
        }
    }
}

/* ── Content build / rebuild plumbing ────────────────────────────────── */

/**
 * Null every file-scope widget pointer (the objects themselves are
 * deleted by the caller). Timer is handled separately.
 */
static void reset_widget_ptrs(void) {
    lbl_day = lbl_date = lbl_year = lbl_mday = NULL;
    lbl_temp = lbl_deg = lbl_cond = lbl_hilo = NULL;
    lbl_time = lbl_ampm = NULL;
    lbl_humid_val = lbl_dew_val = lbl_wind_val = lbl_uv_val = NULL;
    forecast_row = NULL;
    for (int i = 0; i < FORECAST_BARS; i++) {
        forecast_bars[i] = NULL;
        forecast_lbls[i] = NULL;
        forecast_temp_lbls[i] = NULL;
    }
    for (int i = 0; i < CLK_RULE_COUNT; i++) {
        rules[i] = NULL;
    }
    rule_count = 0;
    for (int i = 0; i < CLK_VDIV_COUNT; i++) {
        vdividers[i] = NULL;
    }
    vdivider_count = 0;
    console_atmo = NULL;
    console_fc_cap = console_range = NULL;
    lbl_cond_big = NULL;
    for (int i = 0; i < 4; i++) {
        console_corners[i] = NULL;
        console_caps[i] = NULL;
        console_rulers[i] = NULL;
    }
    ev_itis = ev_w1 = ev_w2 = ev_w3 = NULL;
    for (int i = 0; i < 6; i++) {
        ev_keys[i] = NULL;
        ev_leaders[i] = NULL;
    }

    /* Blueprint / Transit / Network */
    x_dim_lbl_cnt = x_ter_lbl_cnt = x_ink_lbl_cnt = 0;
    x_dim_bg_cnt = x_ter_bg_cnt = 0;
    blu_frame_o = blu_frame_i = blu_plot = NULL;
    blu_dimlbl = blu_max_lbl = blu_peak_tick = NULL;
    blu_arrow[0] = blu_arrow[1] = NULL;
    met_name = met_ring5 = met_bar = met_pill = met_peak_lbl = NULL;
    met_peak_ring = met_peak_core = met_sun_dia = met_sun_lbl = NULL;
    met_terminus = met_panel = met_legend = NULL;
    met_tick_a = met_tick_b = NULL;
    met_hi_val = met_lo_val = NULL;
    met_arcs[0] = met_arcs[1] = NULL;
    for (int i = 0; i < FORECAST_BARS; i++) {
        met_segs[i] = NULL;
        met_dots[i] = NULL;
    }
    for (int i = 0; i < 3; i++) {
        met_cline[i] = NULL;
    }
    for (int i = 0; i < MET_CSTATIONS; i++) {
        met_cdots[i] = NULL;
    }
    for (int i = 0; i < 9; i++) {
        met_chips[i] = NULL;
    }
    for (int i = 0; i < 4; i++) {
        met_band_lbl[i] = NULL;
    }

    /* Round-only handles and the round restyle hook */
    for (int i = 0; i < FORECAST_BARS; i++) {
        clk_fc_arc[i] = NULL;
        clk_blu_ray[i] = NULL;
    }
    for (int i = 0; i < 4; i++) {
        clk_con_tick[i] = NULL;
    }
    clk_minute_arc = clk_now_tick = NULL;
    clk_arc_cond = clk_arc_stats = NULL;
    clk_blu_peak_tick = clk_blu_max_lbl = NULL;
    clock_round_restyle = NULL;
    clock_round_tick_ms = 0;
    /* The rebuilt arclabels start at "--"; clearing the shadows forces the
     * next update to write the real strings instead of matching a shadow left
     * over from the deleted widgets. */
    clk_arc_cond_prev[0] = '\0';
    clk_arc_stats_prev[0] = '\0';
}

/**
 * Build the content of the configured layout onto clock_root.
 * clock_root must exist. Does not touch the minute timer.
 */
static void build_content(void) {
    /* Reset divider registries so re-creation cannot overflow the arrays */
    rule_count = 0;
    vdivider_count = 0;

    uint8_t layout = app_config_get()->clock_layout;
    s_layout = (layout <= 6) ? layout : 0;

#if CONFIG_NINA_FAMILY_ROUND
    /* Round: Broadside (2) and Evensong (3) are inset cases and keep the
     * shared builders below; every other face has a round composition in
     * ui/nina_clock_round.c. "Transit Line" (5) has no round composition;
     * it is rewritten to Classic (0) here, before the dispatch, so every
     * page path (strings, palette, restyle) sees Classic (spec addendum
     * section 7). The stored config value is NOT rewritten and the web UI
     * keeps offering it. */
    if (s_layout == 5) {
        s_layout = 0;
    }
    if (s_layout == 2) {
        build_layout_broadside();
    } else if (s_layout == 3) {
        build_layout_evensong();
    } else {
        clock_round_build(s_layout);
    }
#else
    if (s_layout == 1) {
        build_layout_console();
    } else if (s_layout == 2) {
        build_layout_broadside();
    } else if (s_layout == 3) {
        build_layout_evensong();
    } else if (s_layout == 4) {
        build_layout_blueprint();
    } else if (s_layout == 5) {
        build_layout_transit();
    } else if (s_layout == 6) {
        build_layout_network();
    } else {
        build_layout_classic();
    }
#endif
}

/* ── Page creation ───────────────────────────────────────────────────── */

lv_obj_t *clock_page_create(lv_obj_t *parent) {
    /* Root — full screen, own background, overrides parent padding */
    clock_root = lv_obj_create(parent);
    lv_obj_set_size(clock_root, screen_size(), screen_size());
    /* Full bleed. Centring rather than negating a literal pad: on square a 720
     * root centred in main_cont's 688 content box lands at (-16,-16), exactly
     * what lv_obj_set_pos(-OUTER_PADDING, -OUTER_PADDING) produced, and it
     * stays centred whatever pad main_cont carries. */
    lv_obj_center(clock_root);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(CLK_BG), 0);
    lv_obj_set_style_bg_opa(clock_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(clock_root, 0, 0);
    lv_obj_set_style_radius(clock_root, 0, 0);
    lv_obj_remove_flag(clock_root, LV_OBJ_FLAG_SCROLLABLE);

    build_content();

    /* Create-once minute timer: clock_page_request_update() reads
     * clock_timer lock-free from other tasks, so the timer must never
     * be deleted/recreated after this point (layout rebuilds keep it). */
    if (clock_timer == NULL) {
        create_clock_timer();
    }

    /* ── Idle connection indicator (floating overlay at top center) ──
     * Created exactly once per boot: the indicator registry has no
     * per-entry removal, so layout rebuilds keep this dot alive. */
    idle_dot = nina_idle_indicator_create(clock_root, LV_ALIGN_TOP_MID, true);

    ESP_LOGI(TAG, "Clock page created (layout=%u)", (unsigned)s_layout);
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
    if (lbl_time) {
        lv_label_set_text(lbl_time, time_buf);
    }

    /* AM/PM */
    if (lbl_ampm) {
        if (is_24h) {
            lv_obj_add_flag(lbl_ampm, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(lbl_ampm, LV_OBJ_FLAG_HIDDEN);
            if (s_layout == 0) {
                lv_label_set_text(lbl_ampm, tm_now.tm_hour < 12 ? "A M" : "P M");
            } else {
                lv_label_set_text(lbl_ampm, tm_now.tm_hour < 12 ? "AM" : "PM");
            }
        }
    }

    /* Evensong word clock (always 12-hour words regardless of the
     * digital format) */
    if (s_layout == 3 && ev_w1) {
        lv_label_set_text(ev_w1, ev_hour_words[tm_now.tm_hour % 12]);
        if (ev_w2) {
            char wbuf[24];
            minute_words(tm_now.tm_min, wbuf, sizeof(wbuf));
            lv_label_set_text(ev_w2, wbuf);
        }
        if (ev_w3) {
            lv_label_set_text(ev_w3, part_of_day_words(tm_now.tm_hour));
        }
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

    const char *day_s = (tm_now.tm_wday >= 0 && tm_now.tm_wday < 7)
                            ? days[tm_now.tm_wday] : "---";
    const char *mon_s = (tm_now.tm_mon >= 0 && tm_now.tm_mon < 12)
                            ? months[tm_now.tm_mon] : "---";

    if (lbl_day) {
        lv_label_set_text(lbl_day, day_s);
    }

    if (lbl_date) {
        char date_buf[64];
        if (s_layout == 1) {
            /* Console header: THU AUG 21 2026 (3-letter weekday — the full
             * weekday overflows the 608px content width at overpass_27) */
            snprintf(date_buf, sizeof(date_buf), "%.3s %.3s %d %d",
                     day_s, mon_s, tm_now.tm_mday, tm_now.tm_year + 1900);
        } else if (s_layout == 2) {
            /* Broadside dateline: month only (day/mday/year separate) */
            snprintf(date_buf, sizeof(date_buf), "%s", mon_s);
        } else if (s_layout == 3) {
            snprintf(date_buf, sizeof(date_buf), "%s - %s %d - %d",
                     day_s, mon_s, tm_now.tm_mday, tm_now.tm_year + 1900);
        } else if (s_layout == 4) {
            /* Blueprint top line: FRI 21 AUG 2026 */
            snprintf(date_buf, sizeof(date_buf), "%.3s %d %.3s %d",
                     day_s, tm_now.tm_mday, mon_s, tm_now.tm_year + 1900);
        } else if (s_layout == 5 || s_layout == 6) {
            snprintf(date_buf, sizeof(date_buf), "%s %d %d",
                     mon_s, tm_now.tm_mday, tm_now.tm_year + 1900);
        } else {
            snprintf(date_buf, sizeof(date_buf), "%s %d", mon_s, tm_now.tm_mday);
        }
        lv_label_set_text(lbl_date, date_buf);
    }

    /* Metro layouts: live weekday header (A-Z font) */
    if (met_name && (s_layout == 5 || s_layout == 6)) {
        lv_label_set_text(met_name, day_s);
    }

    /* Blueprint dimension text under the hero digits */
    if (s_layout == 4 && blu_dimlbl) {
        char dim_buf[40];
        if (is_24h) {
            snprintf(dim_buf, sizeof(dim_buf), "LOCAL TIME %s", time_buf);
        } else {
            snprintf(dim_buf, sizeof(dim_buf), "LOCAL TIME %s %s", time_buf,
                     tm_now.tm_hour < 12 ? "AM" : "PM");
        }
        lv_label_set_text(blu_dimlbl, dim_buf);
    }

    if (lbl_mday) {
        char mday_buf[16];
        snprintf(mday_buf, sizeof(mday_buf), "%d", tm_now.tm_mday);
        lv_label_set_text(lbl_mday, mday_buf);
    }

    if (lbl_year) {
        char year_buf[16];
        snprintf(year_buf, sizeof(year_buf), "%d", tm_now.tm_year + 1900);
        lv_label_set_text(lbl_year, year_buf);
    }

    /* Console unit-bearing captions */
    if (console_caps[0]) {
        lv_label_set_text(console_caps[0],
                          is_metric ? "TEMP \xc2\xb0" "C" : "TEMP \xc2\xb0" "F");
    }
    if (console_fc_cap) {
        lv_label_set_text(console_fc_cap,
                          is_metric ? "TEMP FORECAST \xc2\xb0" "C"
                                    : "TEMP FORECAST \xc2\xb0" "F");
    }

    /* ── Weather data ── */
    weather_data_t wd;
    weather_client_get_data(&wd);

    if (!wd.valid) {
        /* No weather data — show placeholders, hide forecast */
        if (lbl_temp) {
            /* Broadside temp font has no hyphen; Evensong table and the
             * Blueprint callout column want a visible placeholder */
            lv_label_set_text(lbl_temp,
                              (s_layout == 3 || s_layout == 4) ? "--" : "");
        }
        if (lbl_deg)  lv_obj_add_flag(lbl_deg, LV_OBJ_FLAG_HIDDEN);
        if (lbl_cond) {
            /* "--" is outside the thin_84 range on Broadside */
            lv_label_set_text(lbl_cond, (s_layout == 2) ? "" : "--");
        }
        if (lbl_cond_big) lv_label_set_text(lbl_cond_big, "--");
        if (console_range) lv_label_set_text(console_range, "RANGE --");
        set_hilo_text(&wd, red_night);
        if (lbl_humid_val) lv_label_set_text(lbl_humid_val, "--");
        if (lbl_dew_val)   lv_label_set_text(lbl_dew_val, "--");
        if (lbl_wind_val)  lv_label_set_text(lbl_wind_val, "--");
        if (lbl_uv_val)    lv_label_set_text(lbl_uv_val, "--");
        if (forecast_row) lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);
#if LV_USE_ARCLABEL
        if (clk_arc_cond)  lv_arclabel_set_text(clk_arc_cond, "--");
        if (clk_arc_stats) lv_arclabel_set_text(clk_arc_stats, "--");
#endif
        /* Clear the shadows too: a provider that recovers with the same
         * reading it had before the outage must still repaint over "--". */
        clk_arc_cond_prev[0] = '\0';
        clk_arc_stats_prev[0] = '\0';
        if (clock_round_restyle) {
            clock_round_restyle(&wd, &tm_now, red_night, is_metric);
        }
        return;
    }

    /* Temperature (current). Classic keeps its separate degree label;
     * the new layouts carry the degree glyph in the value itself
     * (Evensong folds H/L into the same timetable value). */
    char temp_buf[64];
    if (s_layout == 3) {
        snprintf(temp_buf, sizeof(temp_buf), "%.0f\xc2\xb0  H %.0f / L %.0f",
                 wd.temp_current, wd.temp_high, wd.temp_low);
    } else if (s_layout == 4) {
        /* Blueprint first callout carries the unit: 69°F */
        snprintf(temp_buf, sizeof(temp_buf), "%.0f\xc2\xb0%c",
                 wd.temp_current, is_metric ? 'C' : 'F');
    } else if (s_layout == 1 || s_layout == 2) {
        snprintf(temp_buf, sizeof(temp_buf), "%.0f\xc2\xb0", wd.temp_current);
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "%.0f", wd.temp_current);
    }
    if (lbl_temp) {
        lv_label_set_text(lbl_temp, temp_buf);
        if (s_layout == 2) {
            /* saira_black_110 has no minus glyph — negative temps fall back
             * to the full-ASCII light face (same rule as restyle_forecast) */
            lv_obj_set_style_text_font(lbl_temp,
                                       (temp_buf[0] == '-')
                                           ? &lv_font_saira_light_46
                                           : &lv_font_saira_black_110, 0);
        }
    }
    if (lbl_deg) {
        lv_obj_remove_flag(lbl_deg, LV_OBJ_FLAG_HIDDEN);
    }

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
    if (s_layout == 2) {
        sanitize_thin84(cond_upper);   /* thin_84: A-Z, 0-9, space only */
    }
    if (lbl_cond) {
        if (s_layout == 5 || s_layout == 6) {
            /* Metro accent line: temp first so LONG_DOT clips the tail
             * of long conditions, never the temperature. No middot in
             * these fonts — plain hyphen separator. */
            char acc_buf[160];
            snprintf(acc_buf, sizeof(acc_buf), "%.0f\xc2\xb0 - %s",
                     wd.temp_current, cond_upper);
            lv_label_set_text(lbl_cond, acc_buf);
            /* Round keeps the 27 px floor: ellipsise rather than shrink. */
            fit_cond_one_line(lbl_cond, &lv_font_overpass_27,
                              SCREEN_ROUND ? &lv_font_overpass_27
                                           : &lv_font_overpass_16);
        } else {
            lv_label_set_text(lbl_cond, cond_upper);
            if (s_layout == 4) {
                fit_cond_one_line(lbl_cond, &lv_font_overpass_27,
                                  SCREEN_ROUND ? &lv_font_overpass_27
                                               : &lv_font_overpass_16);
            }
        }
    }
    if (lbl_cond_big) {
        /* Console SKY cell: first word of the condition */
        char word[16];
        int w_i = 0;
        for (; w_i < (int)sizeof(word) - 1 && cond_upper[w_i] &&
               cond_upper[w_i] != ' '; w_i++) {
            word[w_i] = cond_upper[w_i];
        }
        word[w_i] = '\0';
        lv_label_set_text(lbl_cond_big, word[0] ? word : "--");
        fit_cond_one_line(lbl_cond_big, &lv_font_saira_light_46,
                          &lv_font_overpass_27);
    }

    /* Hi / Lo */
    set_hilo_text(&wd, red_night);

    /* Stats. Blueprint carries its caption inside each value ("HUM 91%");
     * Transit folds all readings into two combined right-column rows. */
    char uv_s[16];
    if (wd.uv_index < 0.0f) {
        snprintf(uv_s, sizeof(uv_s), "--");
    } else {
        snprintf(uv_s, sizeof(uv_s), "%.0f", wd.uv_index);
    }

    if (s_layout == 4) {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "HUM %.0f%%", wd.humidity);
        if (lbl_humid_val) lv_label_set_text(lbl_humid_val, sbuf);
        snprintf(sbuf, sizeof(sbuf), "DEW %.0f\xc2\xb0", wd.dew_point);
        if (lbl_dew_val) lv_label_set_text(lbl_dew_val, sbuf);
        snprintf(sbuf, sizeof(sbuf), "WIND %s %.0f", wd.wind_dir,
                 wd.wind_speed);
        if (lbl_wind_val) lv_label_set_text(lbl_wind_val, sbuf);
        snprintf(sbuf, sizeof(sbuf), "UV %s", uv_s);
        if (lbl_uv_val) lv_label_set_text(lbl_uv_val, sbuf);
    } else if (s_layout == 5) {
        char sbuf[96];
        snprintf(sbuf, sizeof(sbuf), "H %.0f L %.0f  HUM %.0f%%",
                 wd.temp_high, wd.temp_low, wd.humidity);
        if (lbl_humid_val) lv_label_set_text(lbl_humid_val, sbuf);
        snprintf(sbuf, sizeof(sbuf), "DEW %.0f\xc2\xb0  WIND %s %.0f  UV %s",
                 wd.dew_point, wd.wind_dir, wd.wind_speed, uv_s);
        if (lbl_dew_val) lv_label_set_text(lbl_dew_val, sbuf);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f%%", wd.humidity);
        if (lbl_humid_val) lv_label_set_text(lbl_humid_val, buf);

        snprintf(buf, sizeof(buf), "%.0f%s", wd.dew_point, "\xc2\xb0");
        if (lbl_dew_val) lv_label_set_text(lbl_dew_val, buf);

        snprintf(buf, sizeof(buf), "%s %.0f", wd.wind_dir, wd.wind_speed);
        if (lbl_wind_val) lv_label_set_text(lbl_wind_val, buf);

        if (lbl_uv_val) lv_label_set_text(lbl_uv_val, uv_s);
    }

    /* Round Classic: the condition row and the stats row live on the rim as
     * arclabels inside the minute ring. Both handles are NULL on every other
     * face and on the square family. An arclabel re-lays out every glyph on
     * a text change, so write only when the string actually moved. */
    if (clk_arc_cond) {
        char cbuf[192];
        snprintf(cbuf, sizeof(cbuf), "%s / H %.0f / L %.0f",
                 cond_upper, wd.temp_high, wd.temp_low);
        if (strcmp(cbuf, clk_arc_cond_prev) != 0) {
#if LV_USE_ARCLABEL
            lv_arclabel_set_text(clk_arc_cond, cbuf);
#endif
            strncpy(clk_arc_cond_prev, cbuf, sizeof(clk_arc_cond_prev) - 1);
            clk_arc_cond_prev[sizeof(clk_arc_cond_prev) - 1] = '\0';
        }
    }
    if (clk_arc_stats) {
        char sbuf[160];
        snprintf(sbuf, sizeof(sbuf), "%.0f%% / %.0f\xc2\xb0 / %s %.0f / UV %s",
                 wd.humidity, wd.dew_point, wd.wind_dir, wd.wind_speed, uv_s);
        if (strcmp(sbuf, clk_arc_stats_prev) != 0) {
#if LV_USE_ARCLABEL
            lv_arclabel_set_text(clk_arc_stats, sbuf);
#endif
            strncpy(clk_arc_stats_prev, sbuf, sizeof(clk_arc_stats_prev) - 1);
            clk_arc_stats_prev[sizeof(clk_arc_stats_prev) - 1] = '\0';
        }
    }

    /* ── Forecast ── */
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
        float frac = (wd.hourly_temps[i] - t_min) / t_range;
        float temp_f = to_fahrenheit(wd.hourly_temps[i], is_metric);

        if (s_layout == 0 && forecast_bars[i]) {
            /* Bar height: proportional between BAR_MIN_H and BAR_MAX_H */
            int h = BAR_MIN_H + (int)(frac * (BAR_MAX_H - BAR_MIN_H) + 0.5f);
            if (h < BAR_MIN_H) h = BAR_MIN_H;
            if (h > BAR_MAX_H) h = BAR_MAX_H;
            lv_obj_set_height(forecast_bars[i], h);

            /* Bar color based on temperature in Fahrenheit */
            lv_obj_set_style_bg_color(forecast_bars[i],
                lv_color_hex(bar_color_for_temp(temp_f, red_night)), 0);
        }

        /* Hour label */
        if (forecast_lbls[i]) {
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
    }

    if (s_layout != 0) {
        restyle_forecast(&wd, red_night);
    }

    if (forecast_row) {
        lv_obj_remove_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* Round faces: shape geometry and the colours of the widgets only the
     * round builder creates. NULL on square. Runs after restyle_forecast()
     * so a round face can overwrite a square placement it inherited. */
    if (clock_round_restyle) {
        clock_round_restyle(&wd, &tm_now, red_night, is_metric);
    }
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
 * text bright/secondary red, dividers dark red, and forecast bars/numerals
 * red shades scaled by temperature. In every non-red theme the active
 * layout's fixed palette is restored byte-for-byte, so those themes look
 * identical to before. Covers all seven layouts (Console frame/brackets/
 * rulers/chip, Broadside recolor H/L and forecast numerals, Evensong
 * word block and dot leaders, Blueprint frames/dimension chrome, and the
 * Transit/Network metro chrome including the temperature-band colors).
 */
void clock_page_apply_theme(void) {
    if (!clock_root) return;

    bool red_night = theme_is_red_night(current_theme);

    clk_palette_t p;
    resolve_palette(&p, red_night);

    /* Background */
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(p.bg), 0);

    /* Date labels. Console prints the date in primary ink; the Broadside
     * dateline alternates emphasis (day/mday primary, month/year gray). */
    set_label_color(lbl_day,  (s_layout == 2) ? p.primary : p.tertiary);
    set_label_color(lbl_date, (s_layout == 1) ? p.primary : p.tertiary);
    set_label_color(lbl_mday, p.primary);
    set_label_color(lbl_year, p.tertiary);

    /* Weather header */
    set_label_color(lbl_temp, p.primary);
    set_label_color(lbl_deg,  p.primary);
    if (s_layout == 1) {
        set_label_color(lbl_cond, p.dim);         /* console grid sub line */
    } else if (s_layout == 5 || s_layout == 6) {
        set_label_color(lbl_cond, p.condition);   /* gold accent line */
    } else if (s_layout >= 2) {
        set_label_color(lbl_cond, p.primary);
    } else {
        set_label_color(lbl_cond, p.condition);
    }
    set_label_color(lbl_cond_big, p.primary);
    set_label_color(lbl_hilo, p.dim);

    /* Time */
    set_label_color(lbl_time, (s_layout == 1) ? p.accent : p.primary);
    if (s_layout == 1 || s_layout == 2 || s_layout == 5 || s_layout == 6) {
        set_label_color(lbl_ampm, p.accent);
    } else if (s_layout == 3) {
        set_label_color(lbl_ampm, p.primary);
    } else if (s_layout == 4) {
        set_label_color(lbl_ampm, p.tertiary);    /* blueprint chip ink */
    } else {
        set_label_color(lbl_ampm, p.dim);
    }

    /* Stats values (secondary). Their caption labels are siblings created
     * inline; recolor those too via the parent containers. */
    set_label_color(lbl_humid_val, p.secondary);
    set_label_color(lbl_dew_val,   p.secondary);
    set_label_color(lbl_wind_val,  p.secondary);
    set_label_color(lbl_uv_val,    p.secondary);

    /* Stat caption labels: the sibling children of each value's parent. */
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
                set_label_color(child, p.dim);
            }
        }
    }

    /* Horizontal rules */
    for (int i = 0; i < rule_count; i++) {
        if (rules[i]) {
            lv_obj_set_style_bg_color(rules[i], lv_color_hex(p.rule), 0);
        }
    }

    /* Vertical dividers */
    for (int i = 0; i < vdivider_count; i++) {
        if (vdividers[i]) {
            lv_obj_set_style_bg_color(vdividers[i], lv_color_hex(p.rule), 0);
        }
    }

    /* Console 92 chrome */
    if (s_layout == 1) {
        for (int i = 0; i < 4; i++) {
            if (console_corners[i]) {
                lv_obj_set_style_border_color(console_corners[i],
                                              lv_color_hex(p.dim), 0);
            }
            if (console_rulers[i]) {
                lv_obj_set_style_border_color(console_rulers[i],
                                              lv_color_hex(p.rule), 0);
                uint32_t tick_cnt = lv_obj_get_child_count(console_rulers[i]);
                for (uint32_t t = 0; t < tick_cnt; t++) {
                    lv_obj_t *tick = lv_obj_get_child(console_rulers[i], t);
                    if (tick) {
                        lv_obj_set_style_bg_color(tick, lv_color_hex(p.rule), 0);
                    }
                }
            }
            set_label_color(console_caps[i], p.dim);
        }
        /* Grid cell left borders (parents of the captions) */
        for (int i = 1; i < 4; i++) {
            if (console_caps[i]) {
                lv_obj_t *cell = lv_obj_get_parent(console_caps[i]);
                if (cell) {
                    lv_obj_set_style_border_color(cell, lv_color_hex(p.rule), 0);
                }
            }
        }
        set_label_color(console_fc_cap, p.dim);
        set_label_color(console_range, p.dim);
        if (console_atmo) {
            lv_obj_set_style_bg_color(console_atmo, lv_color_hex(p.bg), 0);
            lv_obj_set_style_text_color(console_atmo, lv_color_hex(p.dim), 0);
        }
        if (lbl_ampm) {   /* PM chip border */
            lv_obj_set_style_border_color(lbl_ampm, lv_color_hex(p.rule), 0);
        }
    }

    /* Evensong chrome */
    set_label_color(ev_itis, p.accent);
    set_label_color(ev_w1, p.primary);
    set_label_color(ev_w2, p.primary);
    set_label_color(ev_w3, p.accent);
    for (int i = 0; i < 6; i++) {
        set_label_color(ev_keys[i], p.dim);
        set_label_color(ev_leaders[i], p.rule);
    }

    /* Generic chrome collectors (Blueprint / Transit / Network) */
    for (int i = 0; i < x_dim_lbl_cnt; i++) {
        set_label_color(x_dim_lbls[i], p.dim);
    }
    for (int i = 0; i < x_ter_lbl_cnt; i++) {
        set_label_color(x_ter_lbls[i], p.tertiary);
    }
    for (int i = 0; i < x_ink_lbl_cnt; i++) {
        set_label_color(x_ink_lbls[i], p.primary);
    }
    for (int i = 0; i < x_dim_bg_cnt; i++) {
        if (x_dim_bg[i]) {
            lv_obj_set_style_bg_color(x_dim_bg[i], lv_color_hex(p.dim), 0);
        }
    }
    for (int i = 0; i < x_ter_bg_cnt; i++) {
        if (x_ter_bg[i]) {
            lv_obj_set_style_bg_color(x_ter_bg[i], lv_color_hex(p.tertiary), 0);
        }
    }

    /* Blueprint chrome */
    if (s_layout == 4) {
        if (blu_frame_o) {
            lv_obj_set_style_border_color(blu_frame_o,
                                          lv_color_hex(p.tertiary), 0);
        }
        if (blu_frame_i) {
            lv_obj_set_style_border_color(blu_frame_i, lv_color_hex(p.dim), 0);
        }
        if (blu_plot) {
            lv_obj_set_style_border_color(blu_plot,
                                          lv_color_hex(p.tertiary), 0);
        }
        for (int a = 0; a < 2; a++) {
            if (blu_arrow[a]) {
                lv_obj_set_style_line_color(blu_arrow[a],
                                            lv_color_hex(p.tertiary), 0);
            }
        }
        set_label_color(blu_max_lbl, p.primary);
        set_label_color(lbl_hilo, p.primary);   /* callout values read ink */
        if (blu_peak_tick) {
            lv_obj_set_style_bg_color(blu_peak_tick,
                                      lv_color_hex(p.tertiary), 0);
        }
        if (lbl_ampm) {   /* AM chip border */
            lv_obj_set_style_border_color(lbl_ampm,
                                          lv_color_hex(p.tertiary), 0);
        }
    }

    /* Transit / Network chrome */
    if (s_layout == 5 || s_layout == 6) {
        set_label_color(met_name, p.primary);
        if (met_ring5) {
            lv_obj_set_style_border_color(met_ring5,
                lv_color_hex(met_red_col(red_night)), 0);
        }
        if (met_bar) {
            lv_obj_set_style_bg_color(met_bar,
                lv_color_hex(met_navy_col(red_night)), 0);
        }
        if (met_pill) {
            lv_obj_set_style_bg_color(met_pill,
                lv_color_hex(met_red_col(red_night)), 0);
            lv_obj_set_style_text_color(met_pill,
                lv_color_hex(red_night ? p.bg : 0xFFFFFF), 0);
        }
        set_label_color(met_peak_lbl, met_red_col(red_night));
        if (met_sun_dia) {
            lv_obj_set_style_bg_color(met_sun_dia, lv_color_hex(p.accent), 0);
        }
        if (met_terminus) {
            lv_obj_set_style_bg_color(met_terminus,
                                      lv_color_hex(p.primary), 0);
        }
        /* Colours only: the round Night Network builder relies on bg_opa 0
         * and border_width 0, LVGL's defaults after lv_obj_remove_style_all(),
         * to keep met_panel borderless there (C1); do not add an opacity or
         * width write here. */
        lv_obj_t *boxes[2] = { met_panel, met_legend };
        for (int b = 0; b < 2; b++) {
            if (boxes[b]) {
                lv_obj_set_style_bg_color(boxes[b],
                    lv_color_hex(met_box_col(red_night)), 0);
                lv_obj_set_style_border_color(boxes[b],
                    lv_color_hex(p.rule), 0);
            }
        }
        if (met_peak_ring) {
            lv_obj_set_style_border_color(met_peak_ring,
                                          lv_color_hex(p.primary), 0);
        }
        if (met_peak_core) {
            lv_obj_set_style_bg_color(met_peak_core,
                                      lv_color_hex(p.primary), 0);
        }
        for (int i = 0; i < 3; i++) {
            if (met_cline[i]) {
                lv_obj_set_style_bg_color(met_cline[i],
                    lv_color_hex(met_blue_col(red_night)), 0);
            }
        }
        for (int i = 0; i < MET_CSTATIONS; i++) {
            if (met_cdots[i]) {
                lv_obj_set_style_border_color(met_cdots[i],
                    lv_color_hex(met_blue_col(red_night)), 0);
                lv_obj_set_style_bg_color(met_cdots[i],
                    lv_color_hex(met_fill_col(red_night)), 0);
            }
        }
        for (int i = 0; i < FORECAST_BARS; i++) {
            if (met_dots[i]) {
                lv_obj_set_style_bg_color(met_dots[i],
                    lv_color_hex(met_fill_col(red_night)), 0);
            }
        }
        if (s_layout == 5 && met_dots[0]) {
            /* "You are here" ring stays red (restyle skips it) */
            lv_obj_set_style_border_color(met_dots[0],
                lv_color_hex(met_red_col(red_night)), 0);
        }
        for (int i = 0; i < 9; i++) {
            if (met_chips[i]) {
                uint32_t cc = (met_chip_band[i] < 0)
                                  ? met_blue_col(red_night)
                                  : met_band_color(met_chip_band[i],
                                                   red_night);
                lv_obj_set_style_bg_color(met_chips[i], lv_color_hex(cc), 0);
            }
        }
        for (int i = 0; i < 4; i++) {
            set_label_color(met_band_lbl[i], p.dim);
        }
        set_label_color(met_hi_val, p.secondary);
        set_label_color(met_lo_val, p.secondary);
        if (s_layout == 5) {
            /* Transit readings column reads dim, not ink */
            set_label_color(lbl_humid_val, p.dim);
            set_label_color(lbl_dew_val, p.dim);
        }
    }

    /* Forecast hour labels */
    for (int i = 0; i < FORECAST_BARS; i++) {
        set_label_color(forecast_lbls[i], p.bar_label);
    }

    /* Forecast bars/numerals: restyle from the last-known weather sample so
     * the red map applies immediately on theme switch. When no weather is
     * present they stay hidden, so the placeholder color is harmless. */
    weather_data_t wd;
    weather_client_get_data(&wd);
    if (wd.valid) {
        if (s_layout == 0) {
            for (int i = 0; i < FORECAST_BARS; i++) {
                float temp_f = to_fahrenheit(wd.hourly_temps[i], last_is_metric);
                uint32_t col = bar_color_for_temp(temp_f, red_night);
                if (forecast_bars[i]) {
                    lv_obj_set_style_bg_color(forecast_bars[i], lv_color_hex(col), 0);
                }
            }
        } else {
            restyle_forecast(&wd, red_night);
        }
    } else {
        /* No data: give every bar the neutral cool/cold base. */
        uint32_t base = red_night ? 0x500000 : CLK_BAR_COOL;
        for (int i = 0; i < FORECAST_BARS; i++) {
            if (forecast_bars[i]) {
                lv_obj_set_style_bg_color(forecast_bars[i], lv_color_hex(base), 0);
            }
        }
    }

    /* Broadside H/L uses inline recolor tags — re-render with the palette
     * resolved for this theme. */
    if (s_layout == 2) {
        set_hilo_text(&wd, red_night);
    }

    /* Round faces repaint their own shapes through the same hook the update
     * path uses, so this function needs no round branch. NULL on square. */
    if (clock_round_restyle) {
        time_t theme_now = time(NULL);
        struct tm theme_tm;
        localtime_r(&theme_now, &theme_tm);
        clock_round_restyle(&wd, &theme_tm, red_night, last_is_metric);
    }
    if (clk_arc_cond) {
        lv_obj_set_style_text_color(clk_arc_cond, lv_color_hex(p.condition), 0);
    }
    if (clk_arc_stats) {
        lv_obj_set_style_text_color(clk_arc_stats, lv_color_hex(p.secondary), 0);
    }
}

void clock_page_request_update(void) {
    if (clock_timer) {
        lv_timer_ready(clock_timer);  /* Fire on next LVGL tick */
    }
}

void clock_page_refresh_config(void) {
    if (!clock_root) return;

    bool visible = s_page_visible;

    /* The minute timer is create-once and survives rebuilds:
     * clock_page_request_update() reads clock_timer lock-free from the
     * weather poll task, so deleting it here would be a use-after-free.
     * The callback only touches NULL-guarded widget statics and runs in
     * the LVGL task, which is blocked while we hold the display lock.
     * If hidden it stays paused; if visible, on_show below realigns it. */

    /* Delete the content but keep clock_root alive (the dashboard's page
     * pointer stays valid) and keep the idle indicator dot (the indicator
     * registry has no per-entry removal; recreating would leak a dangling
     * registry slot). */
    for (int32_t i = (int32_t)lv_obj_get_child_count(clock_root) - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(clock_root, i);
        if (child != idle_dot) {
            lv_obj_delete(child);
        }
    }
    reset_widget_ptrs();

    build_content();

    /* Keep the floating indicator drawn above the rebuilt content */
    if (idle_dot) {
        lv_obj_move_foreground(idle_dot);
    }

    clock_page_apply_theme();

    if (visible) {
        clock_page_on_show();
    }

    ESP_LOGI(TAG, "Clock page rebuilt (layout=%u)", (unsigned)s_layout);
}

void clock_page_on_hide(void) {
    s_page_visible = false;
    if (clock_timer) {
        lv_timer_pause(clock_timer);
    }
}

void clock_page_on_show(void) {
    s_page_visible = true;
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
