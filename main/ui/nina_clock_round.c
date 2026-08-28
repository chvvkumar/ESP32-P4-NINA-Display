/**
 * @file nina_clock_round.c
 * @brief Round compositions for the clock page faces.
 *
 * Compiled only on CONFIG_NINA_FAMILY_ROUND (main/CMakeLists.txt,
 * nina_round_srcs). One build function per face plus one restyle hook per
 * face; nina_clock.c keeps the timer, the data, the strings and every text
 * write, and calls the installed hook from clock_page_update() and
 * clock_page_apply_theme().
 *
 * Geometry: every mockup y is centre relative, every ring radius is
 * ui_rim_radius() minus an absolute offset, and strokes and fonts are the
 * same pixels at 720 and at 800. LVGL draws an arc inward from its radius,
 * so round_arc() takes the OUTER radius of the stroke.
 *
 * Designs: C:\tmp\p4-multiboard\boards\radial_batch3.md board 1 (Classic),
 * inscribed_batch3.md boards 2, 3 and 5 (Console 92, Blueprint, Night
 * Network), radial_batch3.md board 3 item 5 (Blueprint radial bars).
 */

#include "nina_clock_internal.h"
#include "nina_dashboard_internal.h"   /* current_theme */
#include "clock_dial.h"
#include "ui_round.h"
#include "ui_arclabel.h"
#include "display_defs.h"

#include <math.h>
#include <stdio.h>

/* ── Fonts ───────────────────────────────────────────────────────────── */

LV_FONT_DECLARE(lv_font_playfair_200)      /* round Classic hero, digits + colon */
extern const lv_font_t lv_font_playfair_90;
extern const lv_font_t lv_font_overpass_27;
LV_FONT_DECLARE(lv_font_saira_thin_240)    /* round Console 92 hero */
LV_FONT_DECLARE(lv_font_stencil_190)       /* round Blueprint hero */
LV_FONT_DECLARE(lv_font_hanken_black_96)   /* round Night Network hero */
LV_FONT_DECLARE(lv_font_hanken_black_40)   /* round Night Network weekday */
/* Full ASCII fallback: the Playfair cuts carry no minus sign. */
LV_FONT_DECLARE(lv_font_saira_light_46)

/* ── Shared round helpers ────────────────────────────────────────────── */

#define DEG2RAD_F 0.017453292f

/** lv_arc measures from three o'clock clockwise; the dial from twelve. */
static int32_t dial_to_lv(int deg)
{
    int d = deg % 360;
    if (d < 0) d += 360;
    return (int32_t)((d + 270) % 360);
}

/**
 * A bare stroke drawn with an lv_arc, centred on the parent.
 * @p r_out is the OUTER radius of the stroke and @p w its width, so the
 * stroke covers [r_out - w, r_out]. Only LV_PART_MAIN is styled; the caller
 * styles LV_PART_INDICATOR if it wants a value arc as well.
 */
static lv_obj_t *round_arc(lv_obj_t *parent, int r_out, int w, uint32_t color)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, 2 * r_out, 2 * r_out);
    lv_obj_center(a);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    return a;
}

/**
 * A radial line whose two points the caller fills in panel coordinates.
 * The object sits at (0, 0) so lv_line's point coordinates are panel
 * absolute. lv_line does not copy its points: the caller's array must be
 * static and must outlive the object.
 */
static lv_obj_t *round_ray(lv_obj_t *parent, int w, uint32_t color)
{
    lv_obj_t *ln = lv_line_create(parent);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(ln, 0, 0);
    lv_obj_set_style_line_width(ln, w, 0);
    lv_obj_set_style_line_color(ln, lv_color_hex(color), 0);
    lv_obj_set_style_line_opa(ln, LV_OPA_COVER, 0);
    return ln;
}

/** Panel point at radius @p r on dial angle @p deg. */
static void round_pt(float deg, float r, lv_point_precise_t *out)
{
    float rad = (deg - 90.0f) * DEG2RAD_F;
    float c = (float)screen_center();
    out->x = (lv_value_precise_t)(c + cosf(rad) * r);
    out->y = (lv_value_precise_t)(c + sinf(rad) * r);
}

/* ══ Face 0, Classic ═════════════════════════════════════════════════ */

/* lv_line keeps a pointer to its points, so the store is file static. */
static lv_point_precise_t s_now_tick_pts[2];

static void classic_restyle(const weather_data_t *wd, const struct tm *tm_now,
                            bool red_night, bool is_metric)
{
    const int rs = ui_rim_radius();

    if (clk_minute_arc) {
        lv_arc_set_value(clk_minute_arc, tm_now->tm_min);
        lv_obj_set_style_arc_color(clk_minute_arc,
            lv_color_hex(red_night ? current_theme->bento_border : CLK_RULE),
            LV_PART_MAIN);
        lv_obj_set_style_arc_color(clk_minute_arc,
            lv_color_hex(red_night ? current_theme->text_color : CLK_ACCENT),
            LV_PART_INDICATOR);
    }
    if (clk_now_tick) {
        float ang = (float)((tm_now->tm_hour % 12) * CLOCK_DIAL_DEG_PER_H);
        round_pt(ang, (float)(rs - 26), &s_now_tick_pts[0]);
        round_pt(ang, (float)(rs + 2), &s_now_tick_pts[1]);
        lv_line_set_points(clk_now_tick, s_now_tick_pts, 2);
        lv_obj_set_style_line_color(clk_now_tick,
            lv_color_hex(red_night ? current_theme->text_color : CLK_PRIMARY), 0);
    }
    /* Playfair carries no minus sign (range 0x30-0x3A,0xB0), so a negative
     * reading would render as bare digits with the sign dropped. Swap the
     * number and its degree to the full-ASCII face, the same rule Broadside
     * uses for its own digits-only cut. */
    if (lbl_temp) {
        const lv_font_t *tf = (wd->valid && wd->temp_current < 0.0f)
                                  ? &lv_font_saira_light_46
                                  : &lv_font_playfair_90;
        lv_obj_set_style_text_font(lbl_temp, tf, 0);
        if (lbl_deg) lv_obj_set_style_text_font(lbl_deg, tf, 0);
    }

    if (!clk_fc_arc[0]) return;

    if (!wd->valid) {
        for (int i = 0; i < FORECAST_BARS; i++) {
            lv_obj_add_flag(clk_fc_arc[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    int16_t start_deg[FORECAST_BARS];
    int16_t span_deg[FORECAST_BARS];
    int n = clock_dial_blocks(wd->hourly_hours, FORECAST_BARS,
                              start_deg, span_deg);

    float t_min = wd->hourly_temps[0];
    float t_max = wd->hourly_temps[0];
    for (int i = 1; i < FORECAST_BARS; i++) {
        if (wd->hourly_temps[i] < t_min) t_min = wd->hourly_temps[i];
        if (wd->hourly_temps[i] > t_max) t_max = wd->hourly_temps[i];
    }
    float t_range = t_max - t_min;
    if (t_range < 1.0f) t_range = 1.0f;

    for (int i = 0; i < FORECAST_BARS; i++) {
        if (i >= n) {
            lv_obj_add_flag(clk_fc_arc[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float frac = (wd->hourly_temps[i] - t_min) / t_range;
        int w = 8 + (int)(frac * 14.0f + 0.5f);          /* 8..22 px */
        float tf = is_metric ? wd->hourly_temps[i] * 9.0f / 5.0f + 32.0f
                             : wd->hourly_temps[i];
        lv_obj_set_style_arc_width(clk_fc_arc[i], w, LV_PART_MAIN);
        lv_obj_set_style_arc_color(clk_fc_arc[i],
            lv_color_hex(bar_color_for_temp(tf, red_night)), LV_PART_MAIN);
        /* 2 degrees of air at each end so neighbouring blocks read apart */
        lv_arc_set_bg_angles(clk_fc_arc[i],
                             dial_to_lv(start_deg[i] + 2),
                             dial_to_lv(start_deg[i] + span_deg[i] - 2));
        lv_obj_remove_flag(clk_fc_arc[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * Classic: ten forecast blocks on a 12-hour dial at the rim, a minute ring
 * inside them with a now tick at the current hour, the shipped centre column
 * (date, hero time, AM/PM, temperature) on the widest chords, and the
 * condition and stats rows as arclabels inside the minute ring.
 * radial_batch3.md board 1 plus the 12-hour dial and arclabel rows from the
 * pick (addendum section 3).
 */
static void build_round_classic(void)
{
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(CLK_BG), 0);

    const int rs = ui_rim_radius();

    /* Rim: ten dial blocks. Outer edge on rs - 2 for every block, so a
     * thicker (warmer) block grows inward and the rim edge stays true. The
     * angles and the widths come from classic_restyle(). */
    for (int i = 0; i < FORECAST_BARS; i++) {
        lv_obj_t *a = round_arc(clock_root, rs - 2, 22, CLK_BAR_COOL);
        lv_arc_set_bg_angles(a, 0, 1);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        clk_fc_arc[i] = a;
    }

    /* Minute ring: track plus a value arc filling clockwise from twelve. */
    clk_minute_arc = round_arc(clock_root, rs - 33, 6, CLK_RULE);
    lv_obj_set_style_arc_width(clk_minute_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(clk_minute_arc, lv_color_hex(CLK_ACCENT),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(clk_minute_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_arc_set_mode(clk_minute_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(clk_minute_arc, 0, 360);
    lv_arc_set_rotation(clk_minute_arc, 270);
    lv_arc_set_range(clk_minute_arc, 0, 60);
    lv_arc_set_value(clk_minute_arc, 0);

    /* Now tick: 3 px across the block ring at the current hour. */
    clk_now_tick = round_ray(clock_root, 3, CLK_PRIMARY);
    round_pt(0.0f, (float)(rs - 26), &s_now_tick_pts[0]);
    round_pt(0.0f, (float)(rs + 2), &s_now_tick_pts[1]);
    lv_line_set_points(clk_now_tick, s_now_tick_pts, 2);

    /* Centre column: date row, hero time, AM/PM, temperature. */
    lv_obj_t *col = make_container(clock_root);
    lv_obj_set_size(col, 2 * ui_chord_half(120), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *drow = make_container(col);
    lv_obj_set_size(drow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(drow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(drow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(drow, 10, 0);
    lbl_day  = make_label(drow, &lv_font_overpass_27, CLK_TERTIARY, 2, "---");
    lbl_date = make_label(drow, &lv_font_overpass_27, CLK_TERTIARY, 2, "---");

    lbl_time = make_label(col, &lv_font_playfair_200, CLK_PRIMARY, -4, "");
    lv_obj_set_style_text_line_space(lbl_time, 0, 0);

    lbl_ampm = make_label(col, &lv_font_overpass_27, CLK_DIM, 4, "");
    lv_obj_set_style_pad_top(lbl_ampm, 4, 0);

    lv_obj_t *trow = make_container(col);
    lv_obj_set_size(trow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(trow, 0, 0);
    lv_obj_set_style_pad_top(trow, 8, 0);
    lbl_temp = make_label(trow, &lv_font_playfair_90, CLK_PRIMARY, 0, "");
    /* Playfair 90 carries 0xB0, so the degree stays at 90 px and no text on
     * this face is below the 27 px floor. */
    lbl_deg = make_label(trow, &lv_font_playfair_90, CLK_PRIMARY, 0, "\xc2\xb0");
    lv_obj_add_flag(lbl_deg, LV_OBJ_FLAG_HIDDEN);

    /* Two rim rows inside the minute ring. Text comes from
     * clock_page_update(); this only creates and colours them. */
    clk_arc_cond = ui_arclabel_bottom(clock_root, &lv_font_overpass_27, rs - 60);
    lv_obj_set_style_text_color(clk_arc_cond, lv_color_hex(CLK_CONDITION), 0);
    lv_arclabel_set_text(clk_arc_cond, "--");

    clk_arc_stats = ui_arclabel_bottom(clock_root, &lv_font_overpass_27, rs - 92);
    lv_obj_set_style_text_color(clk_arc_stats, lv_color_hex(CLK_SECONDARY), 0);
    lv_arclabel_set_text(clk_arc_stats, "--");

    clock_round_restyle = classic_restyle;
}

/* ══ Face 1, Console 92 ══════════════════════════════════════════════ */

/* The four rim stubs are lv_line: their points must outlive the call. */
static lv_point_precise_t s_con_stub_pts[4][2];
static lv_obj_t *s_con_stub[4];

static void console_restyle(const weather_data_t *wd, const struct tm *tm_now,
                            bool red_night, bool is_metric)
{
    (void)wd;
    (void)tm_now;
    (void)is_metric;

    uint32_t dim = red_night ? current_theme->label_color : CON_DIM;
    for (int i = 0; i < 4; i++) {
        if (clk_con_tick[i]) {
            lv_obj_set_style_arc_color(clk_con_tick[i], lv_color_hex(dim),
                                       LV_PART_MAIN);
        }
        if (s_con_stub[i]) {
            lv_obj_set_style_line_color(s_con_stub[i], lv_color_hex(dim), 0);
        }
    }
}

/**
 * Console 92: the four corner brackets walk radially out to the rim and
 * become tick arcs with a radial stub, the hero drops to a chord wide enough
 * for the 240 px condensed digits, the ATMOSPHERICS rule becomes a chord with
 * its knock-out intact, the four readout cells keep their dividers and tick
 * rulers at the equator, and the forecast strip tightens to a 34 px pitch.
 * inscribed_batch3.md board 2.
 */
static void build_round_console(void)
{
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(CON_BG), 0);

    const int rs = ui_rim_radius();
    const int cy = screen_center();

    /* Rim ticks at the four diagonals: a 34 degree arc plus a radial stub. */
    static const int16_t tick_deg[4] = { 45, 135, 225, 315 };
    for (int i = 0; i < 4; i++) {
        s_con_stub[i] = NULL;
        lv_obj_t *a = round_arc(clock_root, rs - 5, 2, CON_DIM);
        lv_arc_set_bg_angles(a, dial_to_lv(tick_deg[i] - 17),
                                dial_to_lv(tick_deg[i] + 17));
        clk_con_tick[i] = a;

        round_pt((float)tick_deg[i], (float)(rs - 6), &s_con_stub_pts[i][0]);
        round_pt((float)tick_deg[i], (float)(rs - 24), &s_con_stub_pts[i][1]);
        s_con_stub[i] = round_ray(clock_root, 2, CON_DIM);
        lv_line_set_points(s_con_stub[i], s_con_stub_pts[i], 2);
    }

    /* Hero time + PM chip + date. */
    lbl_time = make_label(clock_root, &lv_font_saira_thin_240, CON_AMBER, 0, "");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, cy - 210);

    lbl_ampm = make_label(clock_root, &lv_font_overpass_27, CON_AMBER, 3, "");
    /* saira_thin_240's ink fills its 169 px line box, so the hero ends at
     * cy - 41; the chip sits clear of it, not 3 px into it as the square does. */
    lv_obj_align(lbl_ampm, LV_ALIGN_TOP_MID, 141, cy - 36);
    lv_obj_set_style_border_width(lbl_ampm, 1, 0);
    lv_obj_set_style_border_color(lbl_ampm, lv_color_hex(CON_LINE), 0);
    lv_obj_set_style_border_opa(lbl_ampm, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(lbl_ampm, 6, 0);
    lv_obj_set_style_pad_bottom(lbl_ampm, 5, 0);
    lv_obj_set_style_pad_left(lbl_ampm, 10, 0);
    lv_obj_set_style_pad_right(lbl_ampm, 8, 0);

    lbl_date = make_label(clock_root, &lv_font_overpass_27, CON_CREAM, 2, "---");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_MID, 0, cy + 6);

    /* Engraved rule, now a chord, with the knocked-out caption over it. */
    lv_obj_t *atmo_rule = make_rule(clock_root);
    lv_obj_set_size(atmo_rule, ui_chord_at_y(cy + 68), 1);
    lv_obj_align(atmo_rule, LV_ALIGN_TOP_MID, 0, cy + 68);
    lv_obj_set_style_bg_color(atmo_rule, lv_color_hex(CON_LINE), 0);

    /* The knock-out bed behind the caption is the page colour over a hairline,
     * not a panel framing text: ledger ruling F(9) allows it under C1, and
     * inscribed board 2 draws it. */
    console_atmo = make_label(clock_root, &lv_font_overpass_27, CON_DIM, 3,
                              "ATMOSPHERICS");
    lv_obj_set_style_bg_color(console_atmo, lv_color_hex(CON_BG), 0);
    lv_obj_set_style_bg_opa(console_atmo, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(console_atmo, 14, 0);
    lv_obj_align(console_atmo, LV_ALIGN_TOP_MID, 0, cy + 50);

    /* Four readout cells on the equator. The sub rows the square face fills
     * (H/L, DEW, UV) are left empty: their labels stay NULL and the page
     * skips them. */
    lv_obj_t *grid = make_container(clock_root);
    /* Cell content at 27 px captions: 4 pad + 39 cap + 8 + 44 val + 8 + 6
     * ruler = 109 px. 110 keeps the box off the forecast row below it. */
    lv_obj_set_size(grid, 554, 110);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, cy + 92);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    /* "HUMID", not "HUMIDITY": overpass_27 is monospace at 16.6 px advance and
     * make_console_cell adds 2 px letter space, so eight glyphs need 147 px in
     * a 554 / 4 = 138 px cell and LVGL would clip it. The radial board uses
     * the same short word. The grid cannot grow: the chord at its bottom
     * (dy 202) is 551 px. */
    static const char *caps[4] = { "TEMP", "SKY", "HUMID", "WIND" };
    lv_obj_t *vals[4];
    for (int i = 0; i < 4; i++) {
        make_console_cell(grid, i, caps[i], &lv_font_saira_light_46, &vals[i]);
        /* 27 px floor: the shipped cell caption is overpass_16. */
        lv_obj_set_style_text_font(console_caps[i], &lv_font_overpass_27, 0);
        lv_obj_set_width(console_rulers[i], 84);
    }
    lbl_temp      = vals[0];
    lbl_cond_big  = vals[1];
    lbl_humid_val = vals[2];
    lbl_wind_val  = vals[3];
    lv_obj_set_width(lbl_cond_big, 130);
    lv_label_set_long_mode(lbl_cond_big, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl_cond_big, LV_TEXT_ALIGN_CENTER, 0);

    /* Forecast strip on a chord baseline: ten 22 px bars at a 34 px pitch.
     * The hour and temperature labels are created and left HIDDEN so the
     * page's write path needs no branch. */
    forecast_row = make_container(clock_root);
    lv_obj_set_size(forecast_row, 328, CON_BAR_MAX + 24);
    lv_obj_align(forecast_row, LV_ALIGN_TOP_MID, 0, cy + 208);
    lv_obj_set_flex_flow(forecast_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(forecast_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < FORECAST_BARS; i++) {
        lv_obj_t *bcol = make_container(forecast_row);
        lv_obj_set_size(bcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bcol, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bcol, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(bcol, 4, 0);

        forecast_temp_lbls[i] = make_label(bcol, &lv_font_overpass_27,
                                           CON_DIM, 0, "--");
        lv_obj_add_flag(forecast_temp_lbls[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *bar = lv_obj_create(bcol);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 22, CON_BAR_MIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CON_DIM), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        forecast_bars[i] = bar;

        forecast_lbls[i] = make_label(bcol, &lv_font_overpass_27,
                                      CON_DIM, 0, "--");
        lv_obj_add_flag(forecast_lbls[i], LV_OBJ_FLAG_HIDDEN);
    }

    clock_round_restyle = console_restyle;
}

/* ══ Face 4, Blueprint ═══════════════════════════════════════════════ */

/* Ten radial forecast bars plus their fixed outward ticks. lv_line keeps a
 * pointer to its points, so both stores are file static. */
static lv_point_precise_t s_blu_ray_pts[FORECAST_BARS][2];
static lv_point_precise_t s_blu_tick_pts[FORECAST_BARS][2];
static lv_obj_t *s_blu_tick[FORECAST_BARS];

/** Dial angle of forecast bar @p i: the 135 to 225 degree bottom sector. */
static float blu_ray_deg(int i)
{
    return 135.0f + (float)i * 9.0f + 4.5f;
}

static void blueprint_restyle(const weather_data_t *wd, const struct tm *tm_now,
                              bool red_night, bool is_metric)
{
    (void)tm_now;
    (void)is_metric;

    const int ri = ui_rim_radius() - 26;
    uint32_t ink  = red_night ? current_theme->text_color  : BLU_HI;
    uint32_t dim  = red_night ? current_theme->label_color : BLU_DIM;
    uint32_t line = red_night ? current_theme->label_color : BLU_LINE;

    for (int i = 0; i < FORECAST_BARS; i++) {
        if (s_blu_tick[i]) {
            lv_obj_set_style_line_color(s_blu_tick[i], lv_color_hex(line), 0);
        }
    }
    if (!clk_blu_ray[0]) return;

    if (!wd->valid) {
        for (int i = 0; i < FORECAST_BARS; i++) {
            lv_obj_add_flag(clk_blu_ray[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (clk_blu_peak_tick) {
            lv_obj_add_flag(clk_blu_peak_tick, LV_OBJ_FLAG_HIDDEN);
        }
        if (clk_blu_max_lbl) {
            lv_obj_add_flag(clk_blu_max_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    float t_min = wd->hourly_temps[0];
    float t_max = wd->hourly_temps[0];
    int i_pk = 0;
    for (int i = 1; i < FORECAST_BARS; i++) {
        if (wd->hourly_temps[i] < t_min) t_min = wd->hourly_temps[i];
        if (wd->hourly_temps[i] > t_max) t_max = wd->hourly_temps[i];
        if (wd->hourly_temps[i] > wd->hourly_temps[i_pk]) i_pk = i;
    }
    float t_range = t_max - t_min;
    if (t_range < 1.0f) t_range = 1.0f;

    for (int i = 0; i < FORECAST_BARS; i++) {
        float len = 22.0f + 46.0f * ((wd->hourly_temps[i] - t_min) / t_range);
        round_pt(blu_ray_deg(i), (float)ri, &s_blu_ray_pts[i][0]);
        round_pt(blu_ray_deg(i), (float)ri - len, &s_blu_ray_pts[i][1]);
        lv_line_set_points(clk_blu_ray[i], s_blu_ray_pts[i], 2);
        lv_obj_set_style_line_color(clk_blu_ray[i],
            lv_color_hex(i == i_pk ? ink : dim), 0);
        lv_obj_remove_flag(clk_blu_ray[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* MAX callout, hanging inward off the tallest bar's tip. This label is
     * a round-only widget, so the hook owns its text as well as its place. */
    const int cy = screen_center();
    float len_pk = 22.0f + 46.0f *
                   ((wd->hourly_temps[i_pk] - t_min) / t_range);
    lv_point_precise_t tip;
    round_pt(blu_ray_deg(i_pk), (float)ri - len_pk - 10.0f, &tip);
    int px = (int)tip.x;
    int py = (int)tip.y;

    if (clk_blu_peak_tick) {
        lv_obj_set_pos(clk_blu_peak_tick, px - 13, py);
        lv_obj_set_style_bg_color(clk_blu_peak_tick, lv_color_hex(ink), 0);
        lv_obj_remove_flag(clk_blu_peak_tick, LV_OBJ_FLAG_HIDDEN);
    }
    if (clk_blu_max_lbl) {
        char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "MAX %.0f\xc2\xb0",
                 wd->hourly_temps[i_pk]);
        lv_label_set_text(clk_blu_max_lbl, mbuf);
        /* An edge bar (0 or 9) points sideways, so its tip is high enough that
         * the label would land inside the 2 x 2 callout block, whose second
         * row ends at about cy + 174. Clamp the label down to just under it;
         * the tick stays on the tip. The clamped band (cy + 184 to cy + 223)
         * is above the bar tips (r 248) and inside the rim. */
        int ly = py - 46;
        if (ly < cy + 184) ly = cy + 184;
        lv_obj_set_pos(clk_blu_max_lbl, px - 70, ly);
        lv_obj_set_style_text_color(clk_blu_max_lbl, lv_color_hex(ink), 0);
        lv_obj_remove_flag(clk_blu_max_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * Blueprint: the two concentric sheet frames become two concentric circles,
 * the registration crosshairs walk to the four cardinals, the header and the
 * dimension chain keep their widgets on the inner circle's chords, the seven
 * leader-dot callouts collapse to a 2 x 2 block, and the elevation chart
 * becomes ten radial bars hanging inward from the inner frame over the 135 to
 * 225 degree sector. inscribed_batch3.md board 3 plus radial_batch3.md
 * board 3 item 5.
 */
static void build_round_blueprint(void)
{
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(BLU_BG), 0);

    const int rs = ui_rim_radius();
    const int ri = rs - 26;
    const int cy = screen_center();

    /* Two sheet frames, now circles: 684 and 632 px at 720. */
    blu_frame_o = make_container(clock_root);
    lv_obj_set_size(blu_frame_o, 2 * rs, 2 * rs);
    lv_obj_center(blu_frame_o);
    lv_obj_set_style_radius(blu_frame_o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(blu_frame_o, 2, 0);
    lv_obj_set_style_border_color(blu_frame_o, lv_color_hex(BLU_LINE), 0);
    lv_obj_set_style_border_opa(blu_frame_o, LV_OPA_COVER, 0);

    blu_frame_i = make_container(clock_root);
    lv_obj_set_size(blu_frame_i, 2 * ri, 2 * ri);
    lv_obj_center(blu_frame_i);
    lv_obj_set_style_radius(blu_frame_i, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(blu_frame_i, 1, 0);
    lv_obj_set_style_border_color(blu_frame_i, lv_color_hex(BLU_DIM), 0);
    lv_obj_set_style_border_opa(blu_frame_i, LV_OPA_COVER, 0);

    /* Registration crosshairs: sheet corners to the four cardinals. */
    static const int8_t card_dx[4] = {  0,  1,  0, -1 };
    static const int8_t card_dy[4] = { -1,  0,  1,  0 };
    for (int i = 0; i < 4; i++) {
        int px = cy + (int)card_dx[i] * rs;
        int py = cy + (int)card_dy[i] * rs;
        reg_ter_bg(make_bg_rect(clock_root, px, py - 8, 1, 17, BLU_LINE));
        reg_ter_bg(make_bg_rect(clock_root, px - 8, py, 17, 1, BLU_LINE));
    }

    /* Header on the inner circle's chord at dy -206. The vertical budget is
     * set by the real font, not by the mock's browser stand-in: stencil_190
     * has a 157 px line box whose ink fills it, so a hero centred at -96 spans
     * dy -174.5 .. -17.5, and overpass_27's cap ink at dy -206 spans
     * -206 .. -187. That leaves 12 px between the header and the digits, and
     * the chain group below moves down 12 px to keep its own clearance.
     * Inner-frame half chord at dy -206: sqrt(316^2 - 206^2) = 239. */
    int hx = cy - (int)sqrtf((float)(ri * ri - 206 * 206)) + 10;
    lbl_date = make_label(clock_root, &lv_font_overpass_27, BLU_LINE, 3, "---");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_LEFT, hx, cy - 206);
    lv_obj_t *rev = make_label(clock_root, &lv_font_overpass_27, BLU_DIM, 3,
                               "REV C");
    lv_obj_align(rev, LV_ALIGN_TOP_RIGHT, -hx, cy - 206);
    reg_dim_lbl(rev);

    /* Hero stencil time, centred. */
    lbl_time = make_label(clock_root, &lv_font_stencil_190, BLU_HI, 2, "");
    lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, -96);

    lbl_ampm = make_label(clock_root, &lv_font_overpass_27, BLU_LINE, 2, "");
    /* Clear of the hero ink, which is about 373 px wide (half 187). */
    lv_obj_align(lbl_ampm, LV_ALIGN_TOP_MID, 224, cy - 170);
    lv_obj_set_style_border_width(lbl_ampm, 1, 0);
    lv_obj_set_style_border_color(lbl_ampm, lv_color_hex(BLU_LINE), 0);
    lv_obj_set_style_border_opa(lbl_ampm, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(lbl_ampm, 4, 0);
    lv_obj_set_style_pad_hor(lbl_ampm, 8, 0);

    /* Dimension chain: the same widgets at the same offsets relative to the
     * chain line, re-centred on the panel and dropped 12 px so the extension
     * rects clear the stencil ink at dy -17.5. */
    reg_dim_bg(make_bg_rect(clock_root, cy - 197, cy - 12, 1, 26, BLU_DIM));
    reg_dim_bg(make_bg_rect(clock_root, cy + 196, cy - 12, 1, 26, BLU_DIM));
    reg_ter_bg(make_bg_rect(clock_root, cy - 186, cy + 22, 372, 1, BLU_LINE));

    for (int a = 0; a < 2; a++) {
        blu_arrow[a] = lv_line_create(clock_root);
        lv_obj_clear_flag(blu_arrow[a], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(blu_arrow[a], 2, 0);
        lv_obj_set_style_line_color(blu_arrow[a], lv_color_hex(BLU_LINE), 0);
    }
    lv_line_set_points(blu_arrow[0], blu_arr_l_pts, 3);
    lv_obj_set_pos(blu_arrow[0], cy - 196, cy + 18);
    lv_line_set_points(blu_arrow[1], blu_arr_r_pts, 3);
    lv_obj_set_pos(blu_arrow[1], cy + 186, cy + 18);

    blu_dimlbl = make_label(clock_root, &lv_font_overpass_27, BLU_LINE, 1,
                            "---");
    lv_obj_set_pos(blu_dimlbl, cy - 197, cy + 32);
    lv_obj_set_width(blu_dimlbl, 394);
    lv_obj_set_style_text_align(blu_dimlbl, LV_TEXT_ALIGN_CENTER, 0);
    reg_ter_lbl(blu_dimlbl);

    /* Four callouts in a 2 x 2 block. Hi/lo, dew and UV are dropped: their
     * labels stay NULL and the page skips them. */
    lbl_temp      = make_blu_callout(cy - 210, cy + 82, &lv_font_saira_light_46);
    lbl_cond      = make_blu_callout(cy + 40,  cy + 82, &lv_font_overpass_27);
    lv_obj_set_width(lbl_cond, 172);
    lv_label_set_long_mode(lbl_cond, LV_LABEL_LONG_DOT);
    lbl_humid_val = make_blu_callout(cy - 210, cy + 130, &lv_font_overpass_27);
    lbl_wind_val  = make_blu_callout(cy + 40,  cy + 130, &lv_font_overpass_27);

    /* Elevation chart: ten 18 px radial bars hanging inward from the inner
     * frame, each with a fixed 6 px outward tick. Lengths and colours come
     * from blueprint_restyle(). */
    for (int i = 0; i < FORECAST_BARS; i++) {
        clk_blu_ray[i] = round_ray(clock_root, 18, BLU_DIM);
        round_pt(blu_ray_deg(i), (float)ri, &s_blu_ray_pts[i][0]);
        round_pt(blu_ray_deg(i), (float)ri, &s_blu_ray_pts[i][1]);
        lv_line_set_points(clk_blu_ray[i], s_blu_ray_pts[i], 2);
        lv_obj_add_flag(clk_blu_ray[i], LV_OBJ_FLAG_HIDDEN);

        round_pt(blu_ray_deg(i), (float)ri, &s_blu_tick_pts[i][0]);
        round_pt(blu_ray_deg(i), (float)(ri + 6), &s_blu_tick_pts[i][1]);
        s_blu_tick[i] = round_ray(clock_root, 1, BLU_LINE);
        lv_line_set_points(s_blu_tick[i], s_blu_tick_pts[i], 2);
    }

    clk_blu_peak_tick = make_bg_rect(clock_root, 0, 0, 26, 1, BLU_HI);
    lv_obj_add_flag(clk_blu_peak_tick, LV_OBJ_FLAG_HIDDEN);

    clk_blu_max_lbl = make_label(clock_root, &lv_font_overpass_27, BLU_HI, 0,
                                 "MAX --");
    lv_obj_set_width(clk_blu_max_lbl, 140);
    lv_obj_set_style_text_align(clk_blu_max_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(clk_blu_max_lbl, LV_OBJ_FLAG_HIDDEN);

    clock_round_restyle = blueprint_restyle;
}

/* ══ Face 6, Night Network ═══════════════════════════════════════════ */

/* The round temperature line carries six stations, not ten. */
#define NET_ROUND_STATIONS 6

static void network_restyle(const weather_data_t *wd, const struct tm *tm_now,
                            bool red_night, bool is_metric)
{
    (void)tm_now;
    (void)is_metric;

    if (!met_peak_ring || !wd->valid) return;

    /* The peak is over the SIX hours this line draws, not the ten
     * restyle_forecast() scanned. */
    int i_pk = 0;
    float t_min = wd->hourly_temps[0];
    float t_max = wd->hourly_temps[0];
    for (int i = 1; i < NET_ROUND_STATIONS; i++) {
        if (wd->hourly_temps[i] > wd->hourly_temps[i_pk]) i_pk = i;
        if (wd->hourly_temps[i] < t_min) t_min = wd->hourly_temps[i];
        if (wd->hourly_temps[i] > t_max) t_max = wd->hourly_temps[i];
    }
    float t_range = t_max - t_min;
    if (t_range < 1.0f) t_range = 1.0f;

    /* restyle_forecast() has already placed the pair at the square face's
     * coordinates over all ten hours; put it back on this face's line. */
    const int cy = screen_center();
    const int tx = cy + 133;
    int sy = cy - 30 + i_pk * 58;
    lv_obj_set_pos(met_peak_ring, tx - 11, sy - 18);
    if (met_peak_core) {
        lv_obj_set_pos(met_peak_core, tx + 1, sy - 6);
    }
    /* Same reason for the bottom end tick: restyle_forecast() colours it from
     * slot 9, and this line ends at slot 5. */
    if (met_tick_b) {
        float f5 = (wd->hourly_temps[NET_ROUND_STATIONS - 1] - t_min) / t_range;
        int band = (int)(f5 * 4.0f);
        if (band > 3) band = 3;
        if (band < 0) band = 0;
        static const uint32_t ramp[4]    = { MET_TEAL, MET_GOLD,
                                             MET_ORANGE, MET_RED };
        static const uint32_t ramp_rn[4] = { 0x500000, 0x800000,
                                             0xC00000, 0xFF0000 };
        lv_obj_set_style_bg_color(met_tick_b,
            lv_color_hex(red_night ? ramp_rn[band] : ramp[band]), 0);
    }
}

/**
 * Night Network: the corner name block walks to the top cardinal and loses
 * its fill and border (guideline C1), the conditions line stays a chord with
 * both end ticks, the temperature line moves in from x 621 to a vertical run
 * at x 493 with six stations, each conditions station carries one caption
 * plus value row instead of a stacked pair, and the legend is not built.
 * inscribed_batch3.md board 5 with the addendum's C1 and no-legend rulings.
 */
static void build_round_network(void)
{
    lv_obj_set_style_pad_all(clock_root, 0, 0);
    lv_obj_set_style_pad_row(clock_root, 0, 0);
    lv_obj_set_layout(clock_root, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(clock_root, lv_color_hex(MET_BG), 0);

    const int cy = screen_center();

    /* Name block on the top cardinal. C1: no fill, no border. Height is
     * LV_SIZE_CONTENT so the real Hanken metrics cannot clip the column. */
    met_panel = make_container(clock_root);
    lv_obj_set_size(met_panel, 420, LV_SIZE_CONTENT);
    lv_obj_align(met_panel, LV_ALIGN_TOP_MID, 0, cy - 264);
    lv_obj_set_flex_flow(met_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(met_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(met_panel, 4, 0);

    lv_obj_t *trow = make_container(met_panel);
    lv_obj_set_size(trow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(trow, 10, 0);
    lbl_time = make_label(trow, &lv_font_hanken_black_96, MET_INK, -2, "");
    lbl_ampm = make_label(trow, &lv_font_overpass_27, MET_GOLD, 1, "");
    lv_obj_set_style_pad_bottom(lbl_ampm, 4, 0);

    met_name = make_label(met_panel, &lv_font_hanken_black_40, MET_INK, 2, "");
    lv_obj_set_style_pad_top(met_name, 12, 0);

    lbl_cond = make_label(met_panel, &lv_font_overpass_27, MET_GOLD, 1, "--");
    lv_obj_set_width(lbl_cond, 400);
    lv_label_set_long_mode(lbl_cond, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl_cond, LV_TEXT_ALIGN_CENTER, 0);

    /* Network map, hidden until forecast data arrives. */
    forecast_row = make_container(clock_root);
    lv_obj_set_size(forecast_row, screen_size(), screen_size());
    lv_obj_set_pos(forecast_row, 0, 0);
    lv_obj_add_flag(forecast_row, LV_OBJ_FLAG_HIDDEN);

    /* Conditions line: a 640 px chord with both end ticks, 29 px above the
     * mock's y so the crossing lands at the midpoint between temperature
     * stations 0 (cy - 30) and 1 (cy + 28) instead of straight through
     * station 1. The board draws it at 380..394 with a station at 388, which
     * contradicts its own caption; the square face crosses at 365..379
     * between stations at 327 and 392. Moving the temperature line instead
     * would push met_tick_b out to r 357, past the rim. The chord at dy -9 is
     * 684 px, so the 640 px bar and its ticks still fit. */
    met_cline[0] = make_bg_rect(forecast_row, cy - 320, cy - 9, 640, 14,
                                MET_BLUE);
    met_cline[1] = make_bg_rect(forecast_row, cy - 327, cy - 24, 14, 44,
                                MET_BLUE);
    met_cline[2] = make_bg_rect(forecast_row, cy + 313, cy - 24, 14, 44,
                                MET_BLUE);

    /* Five conditions stations, each one caption + value row. The legend is
     * gone, so the stations centre on the panel instead of leaving its
     * bottom-left corner free. */
    static const int16_t c_dx[MET_CSTATIONS] = { -240, -120, 0, 120, 240 };
    static const char *c_caps[MET_CSTATIONS] = { "HI", "LO", "RH", "DEW", "W" };
    lv_obj_t *c_vals[MET_CSTATIONS];
    for (int k = 0; k < MET_CSTATIONS; k++) {
        met_cdots[k] = make_station_dot(forecast_row, cy + c_dx[k], cy - 2,
                                        10, 4, MET_BLUE);
        lv_obj_t *row = make_container(forecast_row);
        lv_obj_set_size(row, 120, LV_SIZE_CONTENT);
        lv_obj_set_pos(row, cy + c_dx[k] - 60, cy + 19);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        reg_dim_lbl(make_label(row, &lv_font_overpass_27, MET_DIM, 1,
                               c_caps[k]));
        c_vals[k] = make_label(row, &lv_font_overpass_27, MET_INK, 0, "--");
    }
    met_hi_val    = c_vals[0];
    met_lo_val    = c_vals[1];
    lbl_humid_val = c_vals[2];
    lbl_dew_val   = c_vals[3];
    lbl_wind_val  = c_vals[4];

    /* Temperature line: a vertical run right of centre, moved in from the
     * square face's x 621 so the circle does not clip it to a stub. */
    const int tx = cy + 133;
    met_tick_a = make_bg_rect(forecast_row, tx - 15, cy - 67, 44, 14, MET_TEAL);
    met_tick_b = make_bg_rect(forecast_row, tx - 15, cy + 283, 44, 14,
                              MET_TEAL);
    for (int i = 0; i < NET_ROUND_STATIONS; i++) {
        met_segs[i] = make_bg_rect(forecast_row, tx, cy - 60 + i * 58, 14, 58,
                                   MET_TEAL);
    }
    for (int i = 0; i < NET_ROUND_STATIONS; i++) {
        int sy = cy - 30 + i * 58;
        met_dots[i] = make_station_dot(forecast_row, tx + 7, sy, 10, 4,
                                       MET_TEAL);

        forecast_lbls[i] = make_label(forecast_row, &lv_font_overpass_27,
                                      MET_DIM, 0, "--");
        lv_obj_set_pos(forecast_lbls[i], tx - 120, sy - 19);
        lv_obj_add_flag(forecast_lbls[i], LV_OBJ_FLAG_HIDDEN);

        forecast_temp_lbls[i] = make_label(forecast_row, &lv_font_overpass_27,
                                           MET_INK, 0, "--");
        lv_obj_set_pos(forecast_temp_lbls[i], tx - 60, sy - 19);
        lv_obj_add_flag(forecast_temp_lbls[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Peak interchange, moved to the hottest of the six by the hook. */
    met_peak_ring = make_container(forecast_row);
    lv_obj_set_size(met_peak_ring, 36, 36);
    lv_obj_set_pos(met_peak_ring, tx - 11, cy - 48);
    lv_obj_set_style_radius(met_peak_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(met_peak_ring, 5, 0);
    lv_obj_set_style_border_color(met_peak_ring, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(met_peak_ring, LV_OPA_COVER, 0);

    met_peak_core = make_bg_rect(forecast_row, tx + 1, cy - 36, 12, 12,
                                 0xFFFFFF);
    lv_obj_set_style_radius(met_peak_core, LV_RADIUS_CIRCLE, 0);

    clock_round_restyle = network_restyle;
}

/* ══ Dispatch ════════════════════════════════════════════════════════ */

void clock_round_build(uint8_t layout)
{
    switch (layout) {
    case 1:
        build_round_console();
        break;
    case 4:
        build_round_blueprint();
        break;
    case 6:
        build_round_network();
        break;
    /* Layout 5 (Transit Line) is removed on round and falls to Classic;
     * layouts 2 and 3 never reach here (build_content keeps the inset
     * Broadside and Evensong builders). */
    default:
        build_round_classic();
        break;
    }
}
