#pragma once

/**
 * @file nina_clock_internal.h
 * @brief Seam between the clock page (nina_clock.c) and its round builders
 *        (nina_clock_round.c).
 *
 * The page owns the minute timer, the config reads, the weather snapshot,
 * every string and every text write. A builder creates and places widgets and
 * writes the handles below; it may leave any handle NULL and every page path
 * null-checks. The only colours a builder sets are those of the widgets only
 * it creates (the clk_* handles), and it sets them from the clock_round_restyle
 * hook, which the page calls from clock_page_update() and
 * clock_page_apply_theme().
 *
 * Same shape as main/ui/nina_graph_internal.h: the page's widget pointers are
 * plain globals, declared here, defined in nina_clock.c.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "sdkconfig.h"   /* CONFIG_NINA_FAMILY_ROUND, for the prototype guard */
#include "lvgl.h"
#include "weather_client.h"

/* Shared sizes */

#define FORECAST_BARS   10
#define MET_CSTATIONS   5

/* Console 92 forecast bar height range (mock: 8..48px in a 56px strip) */
#define CON_BAR_MIN     8
#define CON_BAR_MAX     56

/* Classic editorial color palette (fixed, not theme-dependent) */

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
#define CLK_ACCENT      0xB86A3A  /* Warm accent (mock --accent): AM/PM, dial, highs */

/* Console 92 palette (mock ins-b1) */

#define CON_BG          0x0D0B07
#define CON_LINE        0x372F1E
#define CON_AMBER       0xF5A83C
#define CON_DIM         0x9B7F4E
#define CON_CREAM       0xEFE6D3

/* Blueprint palette (mock blu-b1) */

#define BLU_BG          0x0E2A52  /* deep cyanotype blue */
#define BLU_LINE        0x8FB8DD  /* pale cyan hairlines */
#define BLU_HI          0xEEF6FF  /* white/ivory ink */
#define BLU_DIM         0x56799F  /* dim cyan */

/* Transit / Night Network palette (mocks met-b1 / met-b2) */

#define MET_BG          0x0E1420
#define MET_INK         0xF2F4F6
#define MET_DIM         0x8A94A6
#define MET_EDGE        0x2A3646
#define MET_NAVY        0x24427D
#define MET_TEAL        0x37B3AD
#define MET_GOLD        0xEEBC4F
#define MET_ORANGE      0xE8823C
#define MET_RED         0xE04A3F
#define MET_BLUE        0x5AA7E0
#define MET_BOX         0x151D2B  /* panel/legend fill */

/* Page widgets the builders write (defined in nina_clock.c) */

extern lv_obj_t *clock_root;

extern lv_obj_t *lbl_day;
extern lv_obj_t *lbl_date;
extern lv_obj_t *lbl_time;
extern lv_obj_t *lbl_ampm;
extern lv_obj_t *lbl_temp;
extern lv_obj_t *lbl_deg;
extern lv_obj_t *lbl_cond;
extern lv_obj_t *lbl_cond_big;
extern lv_obj_t *lbl_hilo;
extern lv_obj_t *lbl_humid_val;
extern lv_obj_t *lbl_dew_val;
extern lv_obj_t *lbl_wind_val;
extern lv_obj_t *lbl_uv_val;

extern lv_obj_t *forecast_row;
extern lv_obj_t *forecast_bars[FORECAST_BARS];
extern lv_obj_t *forecast_lbls[FORECAST_BARS];
extern lv_obj_t *forecast_temp_lbls[FORECAST_BARS];

extern lv_obj_t *console_atmo;
extern lv_obj_t *console_caps[4];
extern lv_obj_t *console_rulers[4];

extern lv_obj_t *blu_frame_o;
extern lv_obj_t *blu_frame_i;
extern lv_obj_t *blu_dimlbl;
extern lv_obj_t *blu_arrow[2];
extern const lv_point_precise_t blu_arr_l_pts[3];
extern const lv_point_precise_t blu_arr_r_pts[3];

extern lv_obj_t *met_name;
extern lv_obj_t *met_panel;
extern lv_obj_t *met_segs[FORECAST_BARS];
extern lv_obj_t *met_dots[FORECAST_BARS];
extern lv_obj_t *met_cline[3];
extern lv_obj_t *met_cdots[MET_CSTATIONS];
extern lv_obj_t *met_tick_a;
extern lv_obj_t *met_tick_b;
extern lv_obj_t *met_hi_val;
extern lv_obj_t *met_lo_val;
extern lv_obj_t *met_peak_ring;
extern lv_obj_t *met_peak_core;

/* Round-only shape handles. NULL on square and on the round faces that do not
 * draw them. */

extern lv_obj_t *clk_fc_arc[FORECAST_BARS];   /* Classic 12-hour dial blocks */
extern lv_obj_t *clk_minute_arc;              /* Classic minute ring */
extern lv_obj_t *clk_now_tick;                /* Classic now tick */
extern lv_obj_t *clk_arc_cond;                /* Classic condition rim arclabel */
extern lv_obj_t *clk_arc_stats;               /* Classic stats rim arclabel */
extern lv_obj_t *clk_con_tick[4];             /* Console 92 rim tick arcs */
extern lv_obj_t *clk_blu_ray[FORECAST_BARS];  /* Blueprint radial forecast bars */
extern lv_obj_t *clk_blu_peak_tick;           /* Blueprint radial peak tick */
extern lv_obj_t *clk_blu_max_lbl;             /* Blueprint radial MAX callout */

/**
 * Per-update hook installed by a round builder, NULL otherwise. The page
 * calls it at the end of clock_page_update() (with or without valid weather)
 * and at the end of clock_page_apply_theme(), so a round face gets exactly
 * one place for the geometry and the colours the square page has no handle
 * for. @p wd may be invalid; the hook must check wd->valid.
 */
typedef void (*clock_round_restyle_cb_t)(const weather_data_t *wd,
                                         const struct tm *tm_now,
                                         bool red_night, bool is_metric);
extern clock_round_restyle_cb_t clock_round_restyle;

/**
 * Last string written to each round Classic rim arclabel. An arclabel re-lays
 * out every glyph on a text change, so the page writes only when the string
 * moved; these shadows are what "moved" is measured against, and
 * reset_widget_ptrs() and the no-weather path clear them so a rebuild or an
 * outage recovery always repaints. Declared here only so those two sites can
 * reach them; nothing outside nina_clock.c touches them.
 */
extern char clk_arc_cond_prev[192];
extern char clk_arc_stats_prev[160];

/* Widget helpers shared with the round builders */

lv_obj_t *make_container(lv_obj_t *parent);
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                     uint32_t color_hex, int letter_space, const char *text);
lv_obj_t *make_rule(lv_obj_t *parent);
lv_obj_t *make_bg_rect(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t color);
lv_obj_t *make_station_dot(lv_obj_t *parent, int cx, int cy,
                           int inner, int bw, uint32_t border);
lv_obj_t *make_console_cell(lv_obj_t *grid, int idx, const char *cap,
                            const lv_font_t *val_font, lv_obj_t **out_val);
lv_obj_t *make_blu_callout(int x, int y, const lv_font_t *font);
uint32_t  bar_color_for_temp(float temp_f, bool red_night);
float     to_fahrenheit(float temp, bool is_metric);

void reg_dim_lbl(lv_obj_t *o);
void reg_ter_lbl(lv_obj_t *o);
void reg_ink_lbl(lv_obj_t *o);
void reg_dim_bg(lv_obj_t *o);
void reg_ter_bg(lv_obj_t *o);

#if CONFIG_NINA_FAMILY_ROUND
/**
 * Build the round composition for @p layout onto clock_root. Layouts 2
 * (Broadside) and 3 (Evensong) never reach here: they are inset cases and
 * build_content() keeps them on the shared square builders. Layout 5
 * (Transit Line) is removed on round and falls to Classic.
 * Caller holds the LVGL display lock.
 */
void clock_round_build(uint8_t layout);
#endif
