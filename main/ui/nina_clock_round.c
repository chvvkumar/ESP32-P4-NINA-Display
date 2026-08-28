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

/* ══ Dispatch ════════════════════════════════════════════════════════ */

void clock_round_build(uint8_t layout)
{
    switch (layout) {
    case 1:
        build_round_console();
        break;
    /* Layout 5 (Transit Line) is removed on round and falls to Classic;
     * layouts 4 and 6 join this switch in sub-plan tasks E3 and E4. */
    default:
        build_round_classic();
        break;
    }
}
