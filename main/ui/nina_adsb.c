/**
 * @file nina_adsb.c
 * @brief ADS-B page — Sky Dome / Radar Scope / Board over one client snapshot.
 *
 * SHAPE OF THE FILE
 * -----------------
 * Two halves that never mix:
 *
 *   1. nina_adsb_update() / recompute() run in the UI task, in `float`, and turn
 *      the adsb_client snapshot plus the NINA mount pointings into a small
 *      static array of INTEGER screen coordinates (s_mark / s_ring / s_fov /
 *      s_lead). They also drive every lv_label on the page.
 *   2. disc_draw_cb() is an LV_EVENT_DRAW_MAIN_END callback that reads those
 *      ints and nothing else — no trig, no snapshot, no config read. The P4 FPU
 *      is single precision and the draw callback runs on the flush path, so
 *      keeping the maths out of it is not a style preference (precedent:
 *      ui_styles.c widget_draw_cb).
 *
 * Text is lv_labels, pre-created once and repositioned/retexted per update:
 * cardinals, ring labels, the three tag boxes, the header/status scrims, the
 * Board rows and the Board detail card. Nothing is created per poll.
 *
 * BOARD MODE is flight awareness, not an observing aid (retarget 2026-08-18):
 * it applies no elevation gate, and carries no elevation, azimuth or relative
 * bearing anywhere. Contacts come straight off the client's nearest-first
 * ranking — rank 0 fills the lead block plus the detail card, ranks 1-5 fill
 * the rows. Sky and Scope keep the elevation gate and the mount pointing.
 *
 * LAYOUT RULE (spec 2026-08-18 section 6): the outer ring is pushed to the
 * screen edge (r = 356 at 360,360) and the cardinals and ring labels sit INSIDE
 * the rim. The header and status strips are translucent scrims OVER the disc,
 * they do not reserve space.
 *
 * INTERACTION: press/move/release on the root. Under ADSB_TAP_SLOP_PX of total
 * travel it is a tap and cycles the mode; beyond it on Sky/Scope it rotates
 * flights_up_azimuth live (the grabbed azimuth follows the finger), snapping to
 * 5 deg on release. Both writes go through a PSRAM config snapshot +
 * app_config_save_deferred(), which already debounces the ~350 ms NVS write by
 * ~2 s, so a burst of taps or a whole drag costs one flash write.
 */

#include "nina_adsb.h"

#include "nina_dashboard_internal.h"   /* SCREEN_SIZE, OUTER_PADDING, current_theme */
#include "nina_empty_state.h"
#include "adsb_client.h"
#include "adsb_geom.h"
#include "app_config.h"
#include "display_defs.h"
#include "nina_client.h"
#include "page_conn.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "adsb_ui";

LV_FONT_DECLARE(lv_font_montserrat_64);
LV_FONT_DECLARE(lv_font_overpass_16);
LV_FONT_DECLARE(lv_font_overpass_27);

/* ── Geometry constants ───────────────────────────────────────────────── */

#define DISC_CX        360
#define DISC_CY        360
#define DISC_R         356     /* outer ring, spec: hard to the screen edge */
#define CARD_R         (DISC_R - 24)   /* cardinal letters INSIDE the rim     */
#define NTICK_OUT      (DISC_R - 3)
#define NTICK_IN       (DISC_R - 20)

#define STRIP_H        44
#define HDR_H          44

#define ADSB_TAG_COUNT   3
#define ADSB_BOARD_ROWS  5
#define ADSB_TAP_SLOP_PX 12
#define ADSB_SNAP_DEG    5.0f

/* Tag box: two lines (Montserrat 20 over 18) plus 2 px of breathing room. */
#define TAG_W  150
#define TAG_H  48

/* Board grid. Column x are relative to the row panel, which is 680 wide.
 *
 * VERTICAL BUDGET (720 tall, header scrim 0-44, status scrim 676-720):
 *    48  eyebrow          Montserrat 16
 *    68  callsign         Montserrat 64  (line height ~82)
 *   152  route / operator Montserrat 28
 *   190  dist / alt / hdg Montserrat 22
 *   228  column headings  Montserrat 16
 *   252  rows 1-5         44 tall, 50 pitch -> last row ends 496
 *   506  detail card      164 tall -> ends 670, 6 px clear of the strip
 */
#define BOARD_X       20
#define BOARD_W       680
#define BOARD_ROW_H   44
#define BOARD_ROW_DY  50
#define BOARD_ROW_Y0  252
#define COL_CALL_X    14
#define COL_ROUTE_X   182
#define COL_ALT_X     400
#define COL_HDG_X     486
#define COL_DIST_X    560

/* Lead-contact detail card. Nine key/value rows do not fit one column in the
 * 164 px that is left, so the grid is 5 rows left / 4 rows right. */
#define CARD_Y        506
#define CARD_H        164
#define CARD_ROW_Y0   38
#define CARD_ROW_DY   24
#define CARD_K1_X     16
#define CARD_V1_X     160
#define CARD_K2_X     356
#define CARD_V2_X     500
#define CARD_ROWS_L   5
#define CARD_FIELDS   9

#define STALE_DIM_S      15.0f   /* dim past this position age  */
#define STALE_DROP_S     60.0f   /* not drawn at all past this  */

#define MODE_SKY    0
#define MODE_SCOPE  1
#define MODE_BOARD  2

/* Mark flags */
#define MK_FILLED  0x01   /* filled diamond (Sky, el >= 20)        */
#define MK_SQUARE  0x02   /* military (dbFlags & 1)                */
#define MK_EMERG   0x04   /* emergency squawk: red halo            */
#define MK_TRI     0x08   /* Scope: heading-rotated triangle       */

/* ── Fixed palette (never themed: it is the tar1090 domain convention) ── */

static const uint32_t ADSB_RAMP[6] = {
    0xFF5C5C,   /* ground .. 2,000 ft */
    0xFF9F3A,   /* 2,000 .. 10,000    */
    0xFFE45C,   /* 10,000 .. 20,000   */
    0x6EE7A8,   /* 20,000 .. 30,000   */
    0x5CC8FF,   /* above 30,000       */
    0x9AA3AD,   /* unknown            */
};
#define COL_EMERG  0xEF4444

/* Frame furniture, from the mockup deck (flights-options.html options 5-7).
 * These are DELIBERATELY not theme colours: current_theme->bento_border lands
 * near black on the dark themes and the range rings vanished on the panel. They
 * still go through themed() so the colour-brightness slider applies. */
#define COL_RING_OUT    0x4A5665   /* outer rim, 2 px, fully opaque */
#define COL_RING_IN     0x36404C   /* inner range/elevation rings   */
#define COL_RING_LBL    0x7E8B99   /* the "10" / "30 deg" numbers   */
#define COL_THREAT      0xE6B450   /* the one accent: lead contact  */
#define COL_LEAD_BRD    0x4A3A12
#define COL_LEAD_BG     0x120D04
#define COL_ROW_BG      0x0A0A0A
#define COL_ROW_BRD     0x222222
#define COL_MUTED       0x8B95A1   /* gs / el / secondary numerics  */
#define COL_MUTED_DIM   0x6B7280   /* range / bearing, column heads */
#define COL_SUB         0x7A8592   /* glance sub-line               */

/* One colour per NINA instance for the FOV rings. nina_summary.c has no
 * per-instance palette to borrow (its cards are themed), so the page owns
 * three fixed hues; they only ever appear next to each other. */
static const uint32_t RIG_COL[MAX_NINA_INSTANCES] = { 0x3B82F6, 0x22C55E, 0xF59E0B };

/* ── Widget tree ──────────────────────────────────────────────────────── */

static lv_obj_t *s_root;
/* Everything that represents DATA hangs off s_content, so one opacity call
 * dims the whole page for the STALE tier while the scrims stay legible. */
static lv_obj_t *s_content;
static lv_obj_t *s_disc;                        /* draw-callback host (Sky/Scope) */
static lv_obj_t *s_lbl_card[4];                 /* N E S W                        */
static lv_obj_t *s_lbl_ring[3];
static lv_obj_t *s_hdr;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_mount;
static lv_obj_t *s_strip;
static lv_obj_t *s_lbl_strip;
static lv_obj_t *s_tag_box[ADSB_TAG_COUNT];     /* scrim + border, the declutter unit */
static lv_obj_t *s_tag_l1[ADSB_TAG_COUNT];
static lv_obj_t *s_tag_l2[ADSB_TAG_COUNT];
static lv_obj_t *s_board;
static lv_obj_t *s_lbl_gkk;                     /* amber eyebrow over the callsign */
static lv_obj_t *s_lbl_glance;                  /* lead callsign, Montserrat 64   */
static lv_obj_t *s_lbl_gsub;                    /* route / operator line          */
static lv_obj_t *s_lbl_gsub2;                   /* dist / alt / hdg / speed line  */
static lv_obj_t *s_hdr_col[5];                  /* board column headings */
static lv_obj_t *s_row_panel[ADSB_BOARD_ROWS];
static lv_obj_t *s_row_call[ADSB_BOARD_ROWS];
static lv_obj_t *s_row_route[ADSB_BOARD_ROWS];
static lv_obj_t *s_row_alt[ADSB_BOARD_ROWS];
static lv_obj_t *s_row_hdg[ADSB_BOARD_ROWS];
static lv_obj_t *s_row_dist[ADSB_BOARD_ROWS];
static lv_obj_t *s_card;                        /* lead-contact detail card */
static lv_obj_t *s_card_title;
static lv_obj_t *s_card_mil;                    /* "MILITARY" chip */
static lv_obj_t *s_card_key[CARD_FIELDS];
static lv_obj_t *s_card_val[CARD_FIELDS];
static lv_obj_t *s_backdrop;                    /* full-cover host for the empty state */
static lv_obj_t *s_empty;

/* ── Page state ───────────────────────────────────────────────────────── */

/* The values the page DRAWS with. Seeded from config at create, re-seeded by
 * nina_adsb_config_changed(), moved by the gestures. Keeping them here (rather
 * than reading config every frame) is what lets a drag rotate live without a
 * config write per touch-move event. */
static uint8_t s_mode;
static float   s_up_deg;

static adsb_data_t *s_snap;      /* PSRAM: ~10 KB, far too big for any stack */
static bool         s_have;      /* s_snap holds a real snapshot            */

/* Gesture scratch */
static bool  s_pressing;
static int   s_press_x, s_press_y;
static int   s_travel;
static float s_press_ang;
static float s_press_up;

/* Precomputed draw geometry (ints only — see file header) */
typedef struct {
    int16_t  x, y;
    int16_t  tx[3], ty[3];
    uint32_t color;
    uint8_t  opa;
    uint8_t  flags;
} adsb_mark_t;

static adsb_mark_t s_mark[ADSB_MAX_AC];
static int         s_mark_n;

static int16_t s_ring_r[3];
static int     s_ring_n;

typedef struct { int16_t x, y, r; uint32_t color; } adsb_fov_t;
static adsb_fov_t s_fov[MAX_NINA_INSTANCES];
static int        s_fov_n;

typedef struct { int16_t x1, y1, x2, y2; } adsb_lead_t;
static adsb_lead_t s_lead[ADSB_TAG_COUNT];
static int         s_lead_n;

/* Position trails, projected in recompute() and drawn as polylines.
 *
 * Stored as points (not segments): 64 contacts x 48 samples of x/y is 12.5 KB,
 * where the same thing as 3000 four-corner segments would be four times that.
 * TRAIL_BREAK marks a gap -- a sample that falls off the disc (below the Sky
 * gate, outside the Scope range) breaks the line instead of drawing a chord
 * across the face. PSRAM: the UI task has no business with 12 KB of .bss. */
#define TRAIL_BREAK  ((int16_t)-32768)

typedef struct {
    int16_t  x[ADSB_MAX_AC * ADSB_TRAIL_MAX];
    int16_t  y[ADSB_MAX_AC * ADSB_TRAIL_MAX];
    struct {
        uint16_t first;     /* index into x/y */
        uint8_t  n;         /* points in this run, >= 2 */
        uint32_t color;     /* the contact's altitude-ramp colour */
    } run[ADSB_MAX_AC];
} adsb_trailbuf_t;

static adsb_trailbuf_t *s_tb;
static int              s_trun_n;

static int16_t s_ntick[4];    /* x1,y1,x2,y2 of the true-north tick */
/* Rim point of each cardinal (N E S W) at DISC_R. The crosshairs are the N-S
 * and E-W chords through these, so the axes turn with the letters instead of
 * staying screen-aligned. */
static int16_t s_axis_x[4], s_axis_y[4];
static bool    s_show_rx;     /* Scope: receiver marker at the centre */

/* Theme-derived draw colours, refreshed by apply_colors() so the draw callback
 * never touches current_theme or the config mutex. */
static uint32_t s_col_line;      /* outer rim */
static uint32_t s_col_ring_in;   /* inner rings */
static uint32_t s_col_dim;
static uint32_t s_col_ink;

/* ── Forward declarations ─────────────────────────────────────────────── */

static void recompute(void);
static void apply_mode(void);
static void apply_colors(void);
static void persist_nav_fields(void);

/* ── Small helpers ────────────────────────────────────────────────────── */

static int cfg_brightness(void)
{
    return app_config_get()->color_brightness;
}

static uint32_t themed(uint32_t raw)
{
    return app_config_apply_brightness(raw, cfg_brightness());
}

static uint32_t col_bg(void)
{
    return themed(current_theme ? current_theme->bg_main : 0x050505);
}

static uint32_t col_text(void)
{
    return themed(current_theme ? current_theme->text_color : 0xE5E7EB);
}

static uint32_t col_label(void)
{
    return themed(current_theme ? current_theme->label_color : 0x6B7280);
}

/** Altitude ramp bucket. Unknown altitude (no position report yet) is grey. */
static uint32_t alt_color(const adsb_ac_t *a)
{
    if (a->on_ground)        return ADSB_RAMP[0];
    if (a->alt_ft <= 0.0f)   return ADSB_RAMP[5];
    if (a->alt_ft <  2000.0f) return ADSB_RAMP[0];
    if (a->alt_ft < 10000.0f) return ADSB_RAMP[1];
    if (a->alt_ft < 20000.0f) return ADSB_RAMP[2];
    if (a->alt_ft < 30000.0f) return ADSB_RAMP[3];
    return ADSB_RAMP[4];
}

/** Vertical-rate cue. ASCII only (LVGL Montserrat has no arrows). */
static char vrate_char(float fpm)
{
    if (fpm >  300.0f) return '^';
    if (fpm < -300.0f) return 'v';
    return '-';
}

/** Altitude in hundreds of feet, clamped to three printed digits. */
static int alt_hundreds(const adsb_ac_t *a)
{
    int h = (int)(a->alt_ft / 100.0f + 0.5f);
    if (h < 0)   h = 0;
    if (h > 999) h = 999;
    return h;
}

/** Callsign, falling back to the ICAO hex when the contact broadcasts none. */
static const char *call_of(const adsb_ac_t *a)
{
    return (a->flight[0] != '\0') ? a->flight : a->hex;
}

/** What is flying: the type description ("BOEING 737-800"), else the ICAO
 *  type code. The registered owner (readsb ownOp) is often a leasing trustee,
 *  so it lives in the detail card only. Empty when the receiver knows nothing
 *  but the callsign. */
static const char *ident_of(const adsb_ac_t *a)
{
    if (a->desc[0]) return a->desc;
    if (a->type[0]) return a->type;
    return "";
}

/** Board row route cell: the IATA pair, "..." while the lookup is in flight,
 *  otherwise whatever identity we do have. */
static const char *row_route_of(const adsb_ac_t *a)
{
    if (a->route[0])      return a->route;
    if (a->route_pending) return "...";
    const char *id = ident_of(a);
    return id[0] ? id : "--";
}

/** "LAX-STL" -> "LAX - STL" for the lead line; the rows print it raw. */
static void route_spaced(const char *src, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 4 < n; i++) {
        if (src[i] == '-') {
            out[o++] = ' ';
            out[o++] = '-';
            out[o++] = ' ';
        } else {
            out[o++] = src[i];
        }
    }
    out[o] = '\0';
}

/** One decimal under 10 nm, whole numbers above it: the extra digit only
 *  carries meaning when the contact is close. */
static void fmt_dist(char *out, size_t n, float nm)
{
    if (nm < 10.0f) {
        snprintf(out, n, "%.1f nm", (double)nm);
    } else {
        snprintf(out, n, "%d nm", (int)(nm + 0.5f));
    }
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

/** Fix a label's width and ellipsize past it. readsb `ownOp` / `desc` run to
 *  23 characters, which walks straight into the next column otherwise.
 *
 *  The height matters as much as the width: LV_LABEL_LONG_DOT only writes dots
 *  once the laid-out text is TALLER than the object, and a label left at
 *  LV_SIZE_CONTENT grows to fit, so it silently WRAPS onto the next row instead
 *  (that is what put "LLC" over the Aircraft row on the detail card). Pinning
 *  the height to one line of the label's own font is what makes DOT bite. */
static void clip_label(lv_obj_t *l, int w)
{
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    if (f) {
        lv_obj_set_height(l, lv_font_get_line_height(f));
    }
}

static void show_obj(lv_obj_t *o, bool visible)
{
    if (!o) return;
    if (visible) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Draw callback: reads precomputed ints only ───────────────────────── */

/* @p cx / @p cy are the ring centre: the disc rings sit at DISC_CX/DISC_CY,
 * but the per-rig FOV circle and the emergency halo are centred on their own
 * mark, so the centre cannot be hardcoded here. */
static void draw_ring(lv_layer_t *layer, int cx, int cy, int r, uint32_t color,
                      int width, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color       = lv_color_hex(color);
    d.width       = width;
    d.radius      = (uint16_t)r;
    d.center.x    = cx;
    d.center.y    = cy;
    d.start_angle = 0;
    d.end_angle   = 360;
    d.opa         = opa;
    lv_draw_arc(layer, &d);
}

static void draw_seg(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                     uint32_t color, int width, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    d.opa   = opa;
    d.p1    = (lv_point_precise_t){ x1, y1 };
    d.p2    = (lv_point_precise_t){ x2, y2 };
    lv_draw_line(layer, &d);
}

static void draw_tri(lv_layer_t *layer, const int16_t *xs, const int16_t *ys,
                     uint32_t color, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.opa   = opa;
    d.p[0]  = (lv_point_precise_t){ xs[0], ys[0] };
    d.p[1]  = (lv_point_precise_t){ xs[1], ys[1] };
    d.p[2]  = (lv_point_precise_t){ xs[2], ys[2] };
    lv_draw_triangle(layer, &d);
}

/** Diamond, TCAS style: filled above the 20 deg tier, hollow below it. */
static void draw_diamond(lv_layer_t *layer, const adsb_mark_t *m)
{
    const int h = 9;
    int x = m->x, y = m->y;
    if (m->flags & MK_FILLED) {
        int16_t xs[3], ys[3];
        xs[0] = (int16_t)x;     ys[0] = (int16_t)(y - h);
        xs[1] = (int16_t)(x + h); ys[1] = (int16_t)y;
        xs[2] = (int16_t)(x - h); ys[2] = (int16_t)y;
        draw_tri(layer, xs, ys, m->color, m->opa);
        ys[0] = (int16_t)(y + h);
        draw_tri(layer, xs, ys, m->color, m->opa);
    } else {
        draw_seg(layer, x, y - h, x + h, y, m->color, 2, m->opa);
        draw_seg(layer, x + h, y, x, y + h, m->color, 2, m->opa);
        draw_seg(layer, x, y + h, x - h, y, m->color, 2, m->opa);
        draw_seg(layer, x - h, y, x, y - h, m->color, 2, m->opa);
    }
}

/** Military: a square instead of a diamond, same size class. */
static void draw_square(lv_layer_t *layer, const adsb_mark_t *m)
{
    const int h = 7;
    int x = m->x, y = m->y;
    if (m->flags & MK_FILLED) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(m->color);
        d.bg_opa   = m->opa;
        d.radius   = 0;
        lv_area_t a = { x - h, y - h, x + h, y + h };
        lv_draw_rect(layer, &d, &a);
    } else {
        draw_seg(layer, x - h, y - h, x + h, y - h, m->color, 2, m->opa);
        draw_seg(layer, x + h, y - h, x + h, y + h, m->color, 2, m->opa);
        draw_seg(layer, x + h, y + h, x - h, y + h, m->color, 2, m->opa);
        draw_seg(layer, x - h, y + h, x - h, y - h, m->color, 2, m->opa);
    }
}

static void disc_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) return;

    /* Rings, outermost first so the inner hairlines land on top. Opaque and
     * 2 px: at LV_OPA_50 over a 1 px arc these were invisible on the panel. */
    draw_ring(layer, DISC_CX, DISC_CY, DISC_R, s_col_line, 2, LV_OPA_COVER);
    for (int i = 0; i < s_ring_n; i++) {
        if (s_ring_r[i] > 6) {
            draw_ring(layer, DISC_CX, DISC_CY, s_ring_r[i], s_col_ring_in, 2, LV_OPA_COVER);
        }
    }

    /* Cross hairs through the centre, plus the true-north tick. Both chords are
     * rotated with the compass (endpoints from place_compass) so the N-S line
     * runs through the N and S letters. */
    draw_seg(layer, s_axis_x[0], s_axis_y[0], s_axis_x[2], s_axis_y[2],
             s_col_ring_in, 1, LV_OPA_40);
    draw_seg(layer, s_axis_x[1], s_axis_y[1], s_axis_x[3], s_axis_y[3],
             s_col_ring_in, 1, LV_OPA_40);
    draw_seg(layer, s_ntick[0], s_ntick[1], s_ntick[2], s_ntick[3],
             s_col_ink, 3, LV_OPA_COVER);

    /* Receiver marker (Radar Scope: the centre IS the receiver). */
    if (s_show_rx) {
        draw_ring(layer, DISC_CX, DISC_CY, 13, s_col_ink, 1, LV_OPA_40);
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(s_col_ink);
        d.bg_opa   = LV_OPA_COVER;
        d.radius   = LV_RADIUS_CIRCLE;
        lv_area_t a = { DISC_CX - 4, DISC_CY - 4, DISC_CX + 4, DISC_CY + 4 };
        lv_draw_rect(layer, &d, &a);
    }

    /* Telescope fields of view: circle + a small mount cross at the centre. */
    for (int i = 0; i < s_fov_n; i++) {
        const adsb_fov_t *f = &s_fov[i];
        draw_ring(layer, f->x, f->y, f->r, f->color, 2, LV_OPA_80);
        draw_seg(layer, f->x, f->y - f->r - 12, f->x, f->y - f->r - 2, f->color, 2, LV_OPA_80);
        draw_seg(layer, f->x, f->y + f->r + 2, f->x, f->y + f->r + 12, f->color, 2, LV_OPA_80);
        draw_seg(layer, f->x - f->r - 12, f->y, f->x - f->r - 2, f->y, f->color, 2, LV_OPA_80);
        draw_seg(layer, f->x + f->r + 2, f->y, f->x + f->r + 12, f->y, f->color, 2, LV_OPA_80);
    }

    /* Position trails, under everything else a contact owns. The opacity ramps
     * from 15% at the oldest sample to 70% at the glyph, so the direction of
     * travel reads without a single arrowhead. Integer maths only (file
     * header): 38..179 is 15%..70% of LV_OPA_COVER. */
    for (int i = 0; i < s_trun_n; i++) {
        int      first = s_tb->run[i].first;
        int      n     = s_tb->run[i].n;
        uint32_t c     = s_tb->run[i].color;
        for (int k = 1; k < n; k++) {
            int16_t x1 = s_tb->x[first + k - 1], y1 = s_tb->y[first + k - 1];
            int16_t x2 = s_tb->x[first + k],     y2 = s_tb->y[first + k];
            if (x1 == TRAIL_BREAK || x2 == TRAIL_BREAK) continue;
            draw_seg(layer, x1, y1, x2, y2, c, 2,
                     (lv_opa_t)(38 + (141 * k) / (n - 1)));
        }
    }

    /* Tag leader lines, drawn under the glyphs. */
    for (int i = 0; i < s_lead_n; i++) {
        draw_seg(layer, s_lead[i].x1, s_lead[i].y1, s_lead[i].x2, s_lead[i].y2,
                 s_col_dim, 1, LV_OPA_70);
    }

    /* Contacts. */
    for (int i = 0; i < s_mark_n; i++) {
        const adsb_mark_t *m = &s_mark[i];
        if (m->flags & MK_EMERG) {
            draw_ring(layer, m->x, m->y, 20, COL_EMERG, 3, LV_OPA_60);
        }
        if (m->flags & MK_TRI) {
            draw_tri(layer, m->tx, m->ty, m->color, m->opa);
        } else if (m->flags & MK_SQUARE) {
            draw_square(layer, m);
        } else {
            draw_diamond(layer, m);
        }
    }
}

/* ── Gesture ──────────────────────────────────────────────────────────── */

/** Screen angle of a point about the disc centre, degrees, 0 = up, CW. */
static float screen_angle(int x, int y)
{
    return adsb_wrap360(atan2f((float)(x - DISC_CX),
                               (float)(DISC_CY - y)) * ADSB_RAD2DEG);
}

static bool indev_point(lv_point_t *pt)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return false;
    lv_indev_get_point(indev, pt);
    return true;
}

static void press_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t pt;
    if (!indev_point(&pt)) return;
    s_pressing   = true;
    s_travel     = 0;
    s_press_x    = pt.x;
    s_press_y    = pt.y;
    s_press_ang  = screen_angle(pt.x, pt.y);
    s_press_up   = s_up_deg;
}

static void pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pressing) return;
    lv_point_t pt;
    if (!indev_point(&pt)) return;

    int dx = pt.x - s_press_x;
    int dy = pt.y - s_press_y;
    int d  = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (d > s_travel) {
        s_travel = d;
    }
    if (s_mode == MODE_BOARD || s_travel <= ADSB_TAP_SLOP_PX) {
        return;
    }
    /* The azimuth the finger grabbed must stay under the finger:
     * az = screen_angle + up  =>  up_new = up_start + (ang_start - ang_now). */
    float now = screen_angle(pt.x, pt.y);
    s_up_deg = adsb_wrap360(s_press_up + (s_press_ang - now));
    recompute();
}

static void released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pressing) return;
    s_pressing = false;

    if (s_travel <= ADSB_TAP_SLOP_PX) {
        s_mode = (uint8_t)((s_mode + 1) % 3);
        apply_mode();
        recompute();
        persist_nav_fields();
        return;
    }
    if (s_mode == MODE_BOARD) {
        return;
    }
    s_up_deg = adsb_wrap360(roundf(s_up_deg / ADSB_SNAP_DEG) * ADSB_SNAP_DEG);
    recompute();
    persist_nav_fields();
}

/**
 * Commit flights_mode / flights_up_azimuth.
 *
 * app_config_save_deferred() puts the values in RAM immediately and coalesces
 * the ~350 ms flash write ~2 s after the LAST call, which is exactly the
 * debounce the design asks for — so no lv_timer of our own. The snapshot goes
 * to PSRAM: app_config_t is far too big for a UI task stack.
 */
static void persist_nav_fields(void)
{
    app_config_t *c = heap_caps_malloc(sizeof(app_config_t), MALLOC_CAP_SPIRAM);
    if (!c) {
        ESP_LOGW(TAG, "no PSRAM for config snapshot; nav change not saved");
        return;
    }
    app_config_get_snapshot_into(c);
    c->flights_mode        = s_mode;
    c->flights_up_azimuth  = (uint16_t)adsb_wrap360(s_up_deg);
    app_config_save_deferred(c);
    heap_caps_free(c);
}

/* ── Mode layout ──────────────────────────────────────────────────────── */

static void apply_mode(void)
{
    bool disc_mode = (s_mode != MODE_BOARD);

    show_obj(s_disc, disc_mode);
    for (int i = 0; i < 4; i++) show_obj(s_lbl_card[i], disc_mode);
    for (int i = 0; i < 3; i++) show_obj(s_lbl_ring[i], disc_mode);
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        show_obj(s_tag_box[i], false);   /* recompute() re-shows the live ones */
    }
    show_obj(s_board, !disc_mode);
    /* The mount pointing is an observing aid; the Board is flight awareness
     * and carries no elevation or azimuth at all. */
    show_obj(s_lbl_mount, disc_mode);
}

/* ── Coordinate pipeline ──────────────────────────────────────────────── */

/* Tag boxes already committed this cycle — the declutter pass tests against
 * these and nothing else. Reset alongside s_lead_n in recompute(). */
static lv_area_t s_tag_area[ADSB_TAG_COUNT];
static int       s_tag_area_n;

static bool boxes_hit(const lv_area_t *a, const lv_area_t *b)
{
    return !(a->x2 < b->x1 || b->x2 < a->x1 || a->y2 < b->y1 || b->y2 < a->y1);
}

/** Keep a TAG_W x TAG_H block inside the disc, clear of both scrims. */
static void clamp_tag(int *ax, int *ay)
{
    if (*ax < 6)                          *ax = 6;
    if (*ax > SCREEN_SIZE - TAG_W - 6)    *ax = SCREEN_SIZE - TAG_W - 6;
    if (*ay < HDR_H + 4)                          *ay = HDR_H + 4;
    if (*ay > SCREEN_SIZE - STRIP_H - TAG_H - 4)  *ay = SCREEN_SIZE - STRIP_H - TAG_H - 4;
}

/** Innermost drawn ring, the "crowded middle" threshold for the leader length.
 *  place_rings() runs before any tag is placed, so s_ring_r is current. */
static int inner_ring_r(void)
{
    return (s_ring_n > 0 && s_ring_r[0] > 20) ? s_ring_r[0] : (DISC_R / 3);
}

/** Cost of putting a tag box here: lower is better. A hidden glyph costs 1, a
 *  hidden tag box costs 4 (two lines of text lost, not one triangle) and a
 *  scrim overlap 2. @p skip is the tagged contact's own glyph, which the leader
 *  line is supposed to reach. Glyph half-extent is 14 px (12 px triangle nose
 *  plus a margin). */
static int tag_score(const lv_area_t *box, int skip)
{
    int s = 0;
    for (int i = 0; i < s_mark_n; i++) {
        if (i == skip) continue;
        int gx = s_mark[i].x, gy = s_mark[i].y;
        if (gx + 14 < box->x1 || gx - 14 > box->x2) continue;
        if (gy + 14 < box->y1 || gy - 14 > box->y2) continue;
        s++;
    }
    for (int i = 0; i < s_tag_area_n; i++) {
        if (boxes_hit(box, &s_tag_area[i])) s += 4;
    }
    if (box->y1 < HDR_H || box->y2 > SCREEN_SIZE - STRIP_H) s += 2;
    return s;
}

/**
 * Place one tag box and hand back its leader line.
 *
 * DECLUTTER: eight candidates — the four quadrants around the contact
 *   0 right-down, 1 right-up, 2 left-up, 3 left-down
 * starting from the one pointing AWAY from the disc centre, then that outward
 * quadrant walked one and two whole boxes up and down. Every candidate is
 * SCORED (tag_score) and the cheapest wins; candidate 0 is the outward one and
 * the strict comparison keeps it on a tie. First-fit was the old rule and it
 * happily parked a tag on top of three other contacts as long as no other TAG
 * was there yet.
 *
 * A contact inside the innermost ring gets a 70 px leader instead of 20: the
 * nearest traffic clusters at the centre, which is exactly where the boxes did
 * the most damage.
 *
 * Cost is 3 tags x 8 candidates x <=64 marks, integer compares only.
 */
static void place_tag(int slot, int x, int y, int mark_idx,
                      const char *l1, const char *l2)
{
    static const int8_t qx[4] = {  1,  1, -1, -1 };
    static const int8_t qy[4] = {  1, -1, -1,  1 };
    static const int8_t vstep[4] = { 1, -1, 2, -2 };
    int start = (x >= DISC_CX) ? ((y >= DISC_CY) ? 0 : 1)
                               : ((y >= DISC_CY) ? 3 : 2);

    int dx0 = x - DISC_CX, dy0 = y - DISC_CY;
    int inner = inner_ring_r();
    bool crowded = (dx0 * dx0 + dy0 * dy0) < (inner * inner);
    int gap_x = crowded ? 70 : 20;
    int gap_y = crowded ? 24 : 8;

    int best_x = x, best_y = y, best_score = -1;
    lv_area_t best = { x, y, x + TAG_W, y + TAG_H };

    for (int c = 0; c < 8; c++) {
        int q  = (start + (c < 4 ? c : 0)) & 3;
        int ax = (qx[q] > 0) ? (x + gap_x) : (x - gap_x - TAG_W);
        int ay = (qy[q] > 0) ? (y + gap_y) : (y - gap_y - TAG_H);
        if (c >= 4) {
            ay += vstep[c - 4] * (TAG_H + 6);
        }
        clamp_tag(&ax, &ay);
        lv_area_t box = { ax, ay, ax + TAG_W, ay + TAG_H };
        int sc = tag_score(&box, mark_idx);
        if (best_score < 0 || sc < best_score) {
            best_score = sc;
            best_x = ax;
            best_y = ay;
            best   = box;
            if (sc == 0) break;      /* clean air: nothing can beat it */
        }
    }

    lv_label_set_text(s_tag_l1[slot], l1);
    lv_label_set_text(s_tag_l2[slot], l2);
    lv_obj_set_pos(s_tag_box[slot], best_x, best_y);
    show_obj(s_tag_box[slot], true);

    if (s_tag_area_n < ADSB_TAG_COUNT) {
        s_tag_area[s_tag_area_n++] = best;
    }
    if (s_lead_n < ADSB_TAG_COUNT) {
        /* Nearest point of the box to the glyph — with a long radial leader the
         * closest edge is as often horizontal as vertical. */
        int ex = (x < best.x1) ? best.x1 : ((x > best.x2) ? best.x2 : x);
        int ey = (y < best.y1) ? best.y1 : ((y > best.y2) ? best.y2 : y);
        s_lead[s_lead_n].x1 = (int16_t)x;
        s_lead[s_lead_n].y1 = (int16_t)y;
        s_lead[s_lead_n].x2 = (int16_t)ex;
        s_lead[s_lead_n].y2 = (int16_t)ey;
        s_lead_n++;
    }
}

/** Cardinals and the true-north tick, both functions of the rotation only. */
static void place_compass(void)
{
    static const char *names[4] = { "N", "E", "S", "W" };
    for (int i = 0; i < 4; i++) {
        float t = (i * 90.0f - s_up_deg) * ADSB_DEG2RAD;
        float si = sinf(t), co = cosf(t);
        int x = DISC_CX + (int)(CARD_R * si);
        int y = DISC_CY - (int)(CARD_R * co);
        /* Same bearing at the rim: the two crosshair chords. */
        s_axis_x[i] = (int16_t)(DISC_CX + (int)(DISC_R * si));
        s_axis_y[i] = (int16_t)(DISC_CY - (int)(DISC_R * co));
        lv_label_set_text(s_lbl_card[i], names[i]);
        /* Keep the letter clear of the header and status scrims: a letter
         * that the rotation carries to the very top or bottom slides
         * inward instead of vanishing under the strip text. */
        int ly = y - 14;
        if (ly < HDR_H + 2)                       ly = HDR_H + 2;
        if (ly > SCREEN_SIZE - STRIP_H - 30)      ly = SCREEN_SIZE - STRIP_H - 30;
        lv_obj_set_pos(s_lbl_card[i], x - 10, ly);
    }
    float tn = (-s_up_deg) * ADSB_DEG2RAD;
    s_ntick[0] = (int16_t)(DISC_CX + (int)(NTICK_OUT * sinf(tn)));
    s_ntick[1] = (int16_t)(DISC_CY - (int)(NTICK_OUT * cosf(tn)));
    s_ntick[2] = (int16_t)(DISC_CX + (int)(NTICK_IN * sinf(tn)));
    s_ntick[3] = (int16_t)(DISC_CY - (int)(NTICK_IN * cosf(tn)));
}

/**
 * Put one ring label just INSIDE its ring, on the NNW diagonal.
 *
 * NNW is the quiet quadrant on both modes: the header scrim only reaches y=44
 * and the tag declutter prefers the outward quadrant of whatever contact it is
 * labelling, so the run of numbers up the top-left diagonal stays readable.
 */
static void place_ring_label(int slot, int r, const char *text)
{
    int d = (int)(0.707f * (float)(r - 22));
    lv_label_set_text(s_lbl_ring[slot], text);
    lv_obj_set_pos(s_lbl_ring[slot], DISC_CX - d - 16, DISC_CY - d - 12);
    show_obj(s_lbl_ring[slot], true);
}

/** Ring radii + their labels. Sky gets 60/30 plus the gate at the rim. */
static void place_rings(float gate, float range)
{
    s_ring_n = 0;
    char buf[16];

    if (s_mode == MODE_SKY) {
        static const float tiers[2] = { 60.0f, 30.0f };
        for (int i = 0; i < 2; i++) {
            if (tiers[i] <= gate) {
                show_obj(s_lbl_ring[i], false);
                continue;
            }
            float x, y;
            adsb_sky_project(0.0f, tiers[i], gate, 0.0f,
                             (float)DISC_CX, (float)DISC_CY, (float)DISC_R, &x, &y);
            int r = (int)(DISC_CY - y);
            s_ring_r[s_ring_n++] = (int16_t)r;
            snprintf(buf, sizeof(buf), "%d\xc2\xb0", (int)tiers[i]);
            place_ring_label(i, r, buf);
        }
        /* The rim IS the gate: label it rather than drawing a ring on top. */
        snprintf(buf, sizeof(buf), "%d\xc2\xb0", (int)gate);
        place_ring_label(2, DISC_R, buf);
    } else {
        const float frac[2] = { 0.2f, 0.5f };
        for (int i = 0; i < 2; i++) {
            int r = (int)(DISC_R * frac[i]);
            s_ring_r[s_ring_n++] = (int16_t)r;
            snprintf(buf, sizeof(buf), "%d", (int)(range * frac[i] + 0.5f));
            place_ring_label(i, r, buf);
        }
        snprintf(buf, sizeof(buf), "%d NM", (int)(range + 0.5f));
        place_ring_label(2, DISC_R, buf);
    }
}

/** Per-rig FOV circles (Sky only: a sky direction has no radius on the Scope). */
static void place_fov(const nina_pointing_t *pt, int n, float gate)
{
    s_fov_n = 0;
    if (s_mode != MODE_SKY) {
        return;
    }
    for (int i = 0; i < n && s_fov_n < MAX_NINA_INSTANCES; i++) {
        if (!pt[i].valid || pt[i].alt_deg < gate) continue;

        float cxp, cyp, exp_x, exp_y;
        adsb_sky_project(pt[i].az_deg, pt[i].alt_deg, gate, s_up_deg,
                         (float)DISC_CX, (float)DISC_CY, (float)DISC_R, &cxp, &cyp);
        float half = pt[i].fov_deg * 0.5f;
        float edge_el = pt[i].alt_deg + half;
        if (edge_el > 89.5f) edge_el = pt[i].alt_deg - half;
        adsb_sky_project(pt[i].az_deg, edge_el, gate, s_up_deg,
                         (float)DISC_CX, (float)DISC_CY, (float)DISC_R, &exp_x, &exp_y);

        float dx = exp_x - cxp, dy = exp_y - cyp;
        int r = (int)(sqrtf(dx * dx + dy * dy) + 0.5f);
        if (r < 6) r = 6;      /* a true 1.2 deg FOV is a few px: floor it, honestly */

        uint8_t inst = pt[i].instance;
        if (inst >= MAX_NINA_INSTANCES) inst = 0;
        s_fov[s_fov_n].x     = (int16_t)cxp;
        s_fov[s_fov_n].y     = (int16_t)cyp;
        s_fov[s_fov_n].r     = (int16_t)r;
        s_fov[s_fov_n].color = RIG_COL[inst];
        s_fov_n++;
    }
}

/**
 * Project one contact's position history into a polyline run.
 *
 * @p ci is the contact index, which is also its row in the snapshot's trail
 * table. Called from the mark loop, so it inherits that loop's already-decided
 * "this contact is drawn at all" test.
 *
 * Consecutive samples that land on the same pixel are dropped here rather than
 * in the draw callback: a slow contact near the centre of the Sky dome pushes
 * out sub-pixel steps for minutes, and each one would otherwise cost a
 * zero-length lv_draw_line every frame.
 */
static void build_trail(int ci, uint32_t color, float gate, float range)
{
    if (!s_tb || s_trun_n >= ADSB_MAX_AC) return;
    const adsb_ac_t *a = &s_snap->ac[ci];
    if (a->trail_n < 2) return;

    int  first = s_trun_n * ADSB_TRAIL_MAX;
    int  n = 0;
    int  px = 0, py = 0;
    bool have_prev = false;

    for (int k = 0; k < a->trail_n && k < ADSB_TRAIL_MAX && n < ADSB_TRAIL_MAX; k++) {
        const adsb_trail_pt_t *p = &s_snap->trail[ci][k];
        float fx = 0.0f, fy = 0.0f;
        bool  on_disc;

        if (s_mode == MODE_SKY) {
            on_disc = (p->el_deg >= gate);
            if (on_disc) {
                adsb_sky_project(p->bearing_deg, p->el_deg, gate, s_up_deg,
                                 (float)DISC_CX, (float)DISC_CY, (float)DISC_R, &fx, &fy);
            }
        } else {
            on_disc = (p->dist_nm <= range);
            if (on_disc) {
                adsb_scope_project(p->bearing_deg, p->dist_nm, range, s_up_deg,
                                   (float)DISC_CX, (float)DISC_CY, (float)DISC_R, &fx, &fy);
            }
        }

        if (!on_disc) {
            /* One break marker per gap, and none leading the run. */
            if (have_prev) {
                s_tb->x[first + n] = TRAIL_BREAK;
                s_tb->y[first + n] = TRAIL_BREAK;
                n++;
                have_prev = false;
            }
            continue;
        }

        int x = (int)(fx + 0.5f);
        int y = (int)(fy + 0.5f);
        if (have_prev && x == px && y == py) continue;
        s_tb->x[first + n] = (int16_t)x;
        s_tb->y[first + n] = (int16_t)y;
        n++;
        px = x;
        py = y;
        have_prev = true;
    }

    if (n < 2) return;
    s_tb->run[s_trun_n].first = (uint16_t)first;
    s_tb->run[s_trun_n].n     = (uint8_t)n;
    s_tb->run[s_trun_n].color = color;
    s_trun_n++;
}

/** The contact holding rank @p r, or NULL when fewer contacts are positioned.
 *  The client assigns ranks nearest-first over every positioned contact, so
 *  ranks are contiguous from 0 and the array order does not matter. */
static const adsb_ac_t *by_rank(int r)
{
    for (int i = 0; i < s_snap->count; i++) {
        if (s_snap->ac[i].rank == r) {
            return &s_snap->ac[i];
        }
    }
    return NULL;
}

/** Lead block lines 2 and 3: who it is, then where it is. */
static void fill_lead_lines(const adsb_ac_t *a)
{
    char buf[128];
    char route[24];
    const char *id = ident_of(a);

    if (a->route[0]) {
        route_spaced(a->route, route, sizeof(route));
    } else if (a->route_pending) {
        snprintf(route, sizeof(route), "route...");
    } else {
        route[0] = '\0';                 /* unknown and not coming: omit it */
    }

    if (route[0] && id[0]) {
        snprintf(buf, sizeof(buf), "%s  /  %s", route, id);
    } else if (route[0]) {
        snprintf(buf, sizeof(buf), "%s", route);
    } else {
        snprintf(buf, sizeof(buf), "%s", id);
    }
    lv_label_set_text(s_lbl_gsub, buf);

    char dist[16];
    fmt_dist(dist, sizeof(dist), a->dist_nm);
    snprintf(buf, sizeof(buf), "%s  /  %03d %c  /  hdg %03d  /  %d kt",
             dist, alt_hundreds(a), vrate_char(a->vrate_fpm),
             (int)((a->track_deg < 0.0f) ? 0.0f : a->track_deg + 0.5f),
             (int)(a->gs_kt + 0.5f));
    lv_label_set_text(s_lbl_gsub2, buf);
}

/** The nine-field detail card under the rows. Values only — the keys and every
 *  position are set once at create. */
static void fill_card(const adsb_ac_t *a)
{
    char v[CARD_FIELDS][40];
    int  i = 0;

    snprintf(v[i++], sizeof(v[0]), "%s", a->reg[0] ? a->reg : "--");
    snprintf(v[i++], sizeof(v[0]), "%s", a->op[0]  ? a->op  : "--");
    snprintf(v[i++], sizeof(v[0]), "%s",
             a->desc[0] ? a->desc : (a->type[0] ? a->type : "--"));

    if (a->squawk != 0) {
        snprintf(v[i++], sizeof(v[0]), "%04u", (unsigned)a->squawk);
    } else {
        snprintf(v[i++], sizeof(v[0]), "----");
    }

    /* Same +/-300 fpm dead band as the row cue, so the card never reads
     * "+150 fpm" next to a row that shows level flight. */
    if (a->vrate_fpm > 300.0f) {
        snprintf(v[i++], sizeof(v[0]), "+%d fpm", (int)(a->vrate_fpm + 0.5f));
    } else if (a->vrate_fpm < -300.0f) {
        snprintf(v[i++], sizeof(v[0]), "%d fpm", (int)(a->vrate_fpm - 0.5f));
    } else {
        snprintf(v[i++], sizeof(v[0]), "level");
    }

    snprintf(v[i++], sizeof(v[0]), "%d kt", (int)(a->gs_kt + 0.5f));

    if (a->on_ground) {
        snprintf(v[i++], sizeof(v[0]), "on ground");
    } else {
        snprintf(v[i++], sizeof(v[0]), "%d ft %s", (int)(a->alt_ft + 0.5f),
                 a->alt_is_geom ? "geom" : "baro");
    }

    snprintf(v[i++], sizeof(v[0]), "%d s ago", (int)(a->seen_pos_s + 0.5f));

    {
        char dist[16];
        fmt_dist(dist, sizeof(dist), a->dist_nm);
        snprintf(v[i++], sizeof(v[0]), "%s  %03d", dist,
                 (int)(adsb_wrap360(a->bearing_deg) + 0.5f));
    }

    for (int k = 0; k < CARD_FIELDS; k++) {
        lv_label_set_text(s_card_val[k], v[k]);
    }
    /* Squawk is field 3: the only value that ever goes red. */
    lv_obj_set_style_text_color(s_card_val[3],
                                lv_color_hex(a->emergency ? themed(COL_EMERG) : s_col_ink), 0);

    lv_label_set_text(s_card_title, call_of(a));
    show_obj(s_card_mil, (a->db_flags & 0x01) != 0);
    show_obj(s_card, true);
}

/**
 * Board mode: lead block, five ranked rows, lead detail card, empty state.
 *
 * No elevation gate here (spec 2026-08-18: the Board is flight awareness, not
 * an observing aid) and no elevation, azimuth or relative bearing anywhere on
 * it — the Sky and Scope modes own that view.
 */
static void fill_board(float range)
{
    char buf[64];

    const adsb_ac_t *lead = by_rank(0);

    if (!lead) {
        lv_label_set_text(s_lbl_gkk, "TRAFFIC");
        lv_obj_set_style_text_color(s_lbl_gkk, lv_color_hex(s_col_dim), 0);
        lv_label_set_text(s_lbl_glance, "CLEAR SKY");
        lv_obj_set_style_text_color(s_lbl_glance, lv_color_hex(s_col_ink), 0);
        snprintf(buf, sizeof(buf), "Nothing within %d nm", (int)(range + 0.5f));
        lv_label_set_text(s_lbl_gsub, buf);
        lv_label_set_text(s_lbl_gsub2, "");
        for (int i = 0; i < 5; i++) {
            show_obj(s_hdr_col[i], false);
        }
        for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
            show_obj(s_row_panel[i], false);
        }
        show_obj(s_card, false);
        return;
    }

    lv_label_set_text(s_lbl_gkk, lead->emergency ? "EMERGENCY" : "NEAREST");
    lv_obj_set_style_text_color(s_lbl_gkk,
                                lv_color_hex(themed(lead->emergency ? COL_EMERG : COL_THREAT)), 0);
    lv_label_set_text(s_lbl_glance, call_of(lead));
    lv_obj_set_style_text_color(s_lbl_glance, lv_color_hex(s_col_ink), 0);
    fill_lead_lines(lead);
    fill_card(lead);

    for (int i = 0; i < 5; i++) {
        show_obj(s_hdr_col[i], true);
    }

    /* Rows are ranks 1..5 — the lead already has the whole block above. */
    for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
        const adsb_ac_t *a = by_rank(i + 1);
        if (!a) {
            show_obj(s_row_panel[i], false);
            continue;
        }

        lv_label_set_text(s_row_call[i], call_of(a));
        lv_obj_set_style_text_color(s_row_call[i],
                                    lv_color_hex(a->emergency ? themed(COL_EMERG) : s_col_ink), 0);

        lv_label_set_text(s_row_route[i], row_route_of(a));

        /* Zero padded: LVGL has no tabular figures, so a variable-width
         * altitude column jitters on every poll. */
        snprintf(buf, sizeof(buf), "%03d%c", alt_hundreds(a), vrate_char(a->vrate_fpm));
        lv_label_set_text(s_row_alt[i], buf);
        lv_obj_set_style_text_color(s_row_alt[i], lv_color_hex(themed(alt_color(a))), 0);

        snprintf(buf, sizeof(buf), "%03d",
                 (int)((a->track_deg < 0.0f) ? 0.0f : a->track_deg + 0.5f));
        lv_label_set_text(s_row_hdg[i], buf);

        fmt_dist(buf, sizeof(buf), a->dist_nm);
        lv_label_set_text(s_row_dist[i], buf);

        lv_obj_set_style_opa(s_row_panel[i],
                             (a->seen_pos_s > STALE_DIM_S) ? LV_OPA_40 : LV_OPA_COVER, 0);
        show_obj(s_row_panel[i], true);
    }
}

/**
 * Turn the held snapshot into screen coordinates and label text.
 *
 * Split out from nina_adsb_update() so the drag path can repaint at touch rate
 * without re-taking the client mutex for a snapshot it already has.
 */
static void recompute(void)
{
    if (!s_root || !s_snap) return;

    const app_config_t *cfg = app_config_get();
    float gate  = (float)cfg->flights_min_el;
    float range = (float)cfg->flights_range_nm;
    if (range < 1.0f) range = 1.0f;

    char buf[128];

    nina_pointing_t pt[MAX_NINA_INSTANCES];
    int np = nina_client_get_pointings(pt, MAX_NINA_INSTANCES);
    if (np > 0) {
        snprintf(buf, sizeof(buf), "MOUNT AZ %03d EL %02d",
                 (int)(adsb_wrap360(pt[0].az_deg) + 0.5f), (int)(pt[0].alt_deg + 0.5f));
    } else {
        buf[0] = '\0';
    }
    lv_label_set_text(s_lbl_mount, buf);

    /* Connection tiers, shared with every other data page. */
    page_conn_t st = s_have
        ? page_conn_eval(s_snap->ever_ok, s_snap->fail_count == 0, s_snap->fail_count)
        : PAGE_CONN_CONNECTING;

    if (st == PAGE_CONN_CONNECTING || st == PAGE_CONN_DOWN) {
        nina_empty_state_set_title(s_empty, st == PAGE_CONN_CONNECTING
                                   ? "Connecting to ADS-B receiver..."
                                   : "Cannot reach ADS-B receiver");
        nina_empty_state_set_busy(s_empty, st == PAGE_CONN_CONNECTING);
        show_obj(s_backdrop, true);
        nina_empty_state_show(s_empty);
        lv_label_set_text(s_lbl_strip, st == PAGE_CONN_CONNECTING
                          ? "connecting" : "receiver unreachable");
        return;
    }
    nina_empty_state_hide(s_empty);
    show_obj(s_backdrop, false);
    lv_obj_set_style_opa(s_content, (st == PAGE_CONN_STALE) ? LV_OPA_60 : LV_OPA_COVER, 0);

    int above   = s_snap->count_above_gate;
    int tracked = s_snap->count_total;

    /* Contacts inside the scope range. Sky counts by elevation instead, so
     * only the two range-based modes read this. */
    int within = 0;
    for (int i = 0; i < s_snap->count; i++) {
        const adsb_ac_t *a = &s_snap->ac[i];
        if (a->has_pos && a->dist_nm <= range) within++;
    }

    /* The count sentence and nothing else. The mode is obvious from the screen
     * and the poll interval is a settings value, not a live reading; both were
     * only ever repeating what the user already knew.
     *
     * The Board no longer applies the elevation gate, so its strip counts by
     * range like the Scope; only Sky still talks about elevation. */
    if (s_mode == MODE_SKY) {
        snprintf(buf, sizeof(buf), "%d above %d\xc2\xb0 / %d tracked   %s",
                 above, (int)gate, tracked,
                 (st == PAGE_CONN_STALE) ? "Reconnecting..." : "");
    } else {
        snprintf(buf, sizeof(buf), "%d within %d nm / %d tracked   %s",
                 within, (int)(range + 0.5f), tracked,
                 (st == PAGE_CONN_STALE) ? "Reconnecting..." : "");
    }
    lv_label_set_text(s_lbl_strip, buf);

    if (s_mode == MODE_BOARD) {
        s_mark_n = 0;
        s_lead_n = 0;
        s_trun_n = 0;
        fill_board(range);
        return;
    }

    /* ── Disc modes ── */
    place_compass();
    place_rings(gate, range);
    place_fov(pt, np, gate);
    s_show_rx = (s_mode == MODE_SCOPE);

    s_mark_n     = 0;
    s_lead_n     = 0;
    s_trun_n     = 0;
    s_tag_area_n = 0;
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        show_obj(s_tag_box[i], false);
    }

    /* Tags are queued here and placed AFTER the mark loop: the declutter score
     * counts glyphs, so it needs every positioned contact, not just the ones
     * walked so far. l1 points into s_snap, which outlives this function. */
    struct { int slot, x, y, mark; const char *l1; char l2[32]; } pend[ADSB_TAG_COUNT];
    int pend_n = 0;

    int tags_done = 0;
    for (int i = 0; i < s_snap->count && s_mark_n < ADSB_MAX_AC; i++) {
        const adsb_ac_t *a = &s_snap->ac[i];
        if (!a->has_pos)                    continue;
        if (a->seen_pos_s > STALE_DROP_S)   continue;

        float fx, fy;
        if (s_mode == MODE_SKY) {
            if (a->el_deg < gate) continue;      /* below the gate: counted, not drawn */
            adsb_sky_project(a->az_deg, a->el_deg, gate, s_up_deg,
                             (float)DISC_CX, (float)DISC_CY, (float)DISC_R, &fx, &fy);
        } else {
            if (a->dist_nm > range) continue;
            adsb_scope_project(a->bearing_deg, a->dist_nm, range, s_up_deg,
                               (float)DISC_CX, (float)DISC_CY, (float)DISC_R, &fx, &fy);
        }
        int x = (int)(fx + 0.5f);
        int y = (int)(fy + 0.5f);
        if (x < 4 || x > SCREEN_SIZE - 4 || y < 4 || y > SCREEN_SIZE - 4) {
            continue;
        }

        int midx = s_mark_n;
        adsb_mark_t *m = &s_mark[s_mark_n];
        memset(m, 0, sizeof(*m));
        m->x     = (int16_t)x;
        m->y     = (int16_t)y;
        m->color = alt_color(a);
        m->opa   = (a->seen_pos_s > STALE_DIM_S) ? LV_OPA_50 : LV_OPA_COVER;
        if (a->db_flags & 0x01) m->flags |= MK_SQUARE;
        if (a->emergency)       m->flags |= MK_EMERG;

        /* Where it came from, in the glyph's own colour. */
        build_trail(i, m->color, gate, range);

        if (s_mode == MODE_SCOPE) {
            /* Heading-rotated triangle, nose along (track - up). */
            float hdg = (a->track_deg < 0.0f) ? 0.0f : a->track_deg;
            float t = (hdg - s_up_deg) * ADSB_DEG2RAD;
            float st_ = sinf(t), ct = cosf(t);
            static const float nose[3][2] = { { 0.0f, -12.0f }, { 7.0f, 8.0f }, { -7.0f, 8.0f } };
            for (int k = 0; k < 3; k++) {
                /* Clockwise screen rotation (y down): heading 90 = nose to the right. */
                float rx = nose[k][0] * ct - nose[k][1] * st_;
                float ry = nose[k][0] * st_ + nose[k][1] * ct;
                m->tx[k] = (int16_t)(x + (int)rx);
                m->ty[k] = (int16_t)(y + (int)ry);
            }
            if (!(m->flags & MK_SQUARE)) {
                m->flags |= MK_TRI;
            } else {
                m->flags |= MK_FILLED;
            }
        } else if (a->el_deg >= 20.0f) {
            m->flags |= MK_FILLED;
        }
        s_mark_n++;

        if (a->rank >= 0 && a->rank < ADSB_TAG_COUNT && tags_done < ADSB_TAG_COUNT) {
            char *l2 = pend[pend_n].l2;
            if (s_mode == MODE_SKY) {
                snprintf(l2, sizeof(pend[0].l2), "%03d%c  %02d\xc2\xb0",
                         alt_hundreds(a), vrate_char(a->vrate_fpm), (int)(a->el_deg + 0.5f));
            } else {
                /* Scope: altitude over range — heading is already in the
                 * rotated triangle, so repeating it in the tag is noise. */
                snprintf(l2, sizeof(pend[0].l2), "%03d  %4.1f",
                         alt_hundreds(a), (double)a->dist_nm);
            }
            pend[pend_n].slot = a->rank;
            pend[pend_n].x    = x;
            pend[pend_n].y    = y;
            pend[pend_n].mark = midx;
            pend[pend_n].l1   = call_of(a);
            pend_n++;
            tags_done++;
        }
    }

    /* Nearest first, so rank 0 gets the pick of the free air. */
    for (int r = 0; r < ADSB_TAG_COUNT; r++) {
        for (int i = 0; i < pend_n; i++) {
            if (pend[i].slot == r) {
                place_tag(r, pend[i].x, pend[i].y, pend[i].mark,
                          pend[i].l1, pend[i].l2);
                break;
            }
        }
    }

    lv_obj_invalidate(s_disc);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void nina_adsb_update(void)
{
    if (!s_root || !s_snap) return;
    if (adsb_client_snapshot(s_snap)) {
        s_have = true;
    }
    recompute();
}

void nina_adsb_config_changed(void)
{
    if (!s_root) return;
    const app_config_t *cfg = app_config_get();
    s_mode   = (cfg->flights_mode > MODE_BOARD) ? MODE_SKY : cfg->flights_mode;
    s_up_deg = adsb_wrap360((float)cfg->flights_up_azimuth);
    apply_mode();
    recompute();
}

uint8_t nina_adsb_get_mode(void)
{
    return s_mode;
}

/* ── Theme ────────────────────────────────────────────────────────────── */

static void apply_colors(void)
{
    /* Rings use the fixed scope greys, not bento_border: see COL_RING_OUT. */
    s_col_line    = themed(COL_RING_OUT);
    s_col_ring_in = themed(COL_RING_IN);
    s_col_dim     = col_label();
    s_col_ink     = col_text();

    lv_obj_set_style_bg_color(s_root, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_bg_color(s_hdr, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_bg_color(s_strip, lv_color_hex(col_bg()), 0);

    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(s_col_ink), 0);
    lv_obj_set_style_text_color(s_lbl_mount, lv_color_hex(s_col_dim), 0);
    lv_obj_set_style_text_color(s_lbl_strip, lv_color_hex(s_col_dim), 0);
    lv_obj_set_style_text_color(s_lbl_glance, lv_color_hex(s_col_ink), 0);
    lv_obj_set_style_text_color(s_lbl_gsub,   lv_color_hex(themed(COL_SUB)), 0);
    lv_obj_set_style_text_color(s_lbl_gsub2,  lv_color_hex(themed(COL_MUTED)), 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_text_color(s_lbl_card[i], lv_color_hex(s_col_ink), 0);
    }
    for (int i = 0; i < 5; i++) {
        lv_obj_set_style_text_color(s_hdr_col[i], lv_color_hex(themed(COL_MUTED_DIM)), 0);
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_text_color(s_lbl_ring[i], lv_color_hex(themed(COL_RING_LBL)), 0);
    }
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        lv_obj_set_style_bg_color(s_tag_box[i], lv_color_hex(col_bg()), 0);
        lv_obj_set_style_border_color(s_tag_box[i], lv_color_hex(s_col_ring_in), 0);
        lv_obj_set_style_text_color(s_tag_l1[i], lv_color_hex(s_col_ink), 0);
        lv_obj_set_style_text_color(s_tag_l2[i], lv_color_hex(themed(COL_MUTED)), 0);
    }
    for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
        lv_obj_set_style_bg_color(s_row_panel[i], lv_color_hex(themed(COL_ROW_BG)), 0);
        lv_obj_set_style_border_color(s_row_panel[i], lv_color_hex(themed(COL_ROW_BRD)), 0);
        lv_obj_set_style_text_color(s_row_call[i], lv_color_hex(s_col_ink), 0);
        lv_obj_set_style_text_color(s_row_route[i], lv_color_hex(themed(COL_MUTED)), 0);
        lv_obj_set_style_text_color(s_row_hdg[i], lv_color_hex(themed(COL_MUTED)), 0);
        lv_obj_set_style_text_color(s_row_dist[i], lv_color_hex(themed(COL_MUTED)), 0);
    }
    lv_obj_set_style_bg_color(s_card, lv_color_hex(themed(COL_ROW_BG)), 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(themed(COL_ROW_BRD)), 0);
    lv_obj_set_style_text_color(s_card_title, lv_color_hex(s_col_ink), 0);
    lv_obj_set_style_bg_color(s_card_mil, lv_color_hex(themed(COL_THREAT)), 0);
    lv_obj_set_style_text_color(s_card_mil, lv_color_hex(themed(COL_LEAD_BG)), 0);
    for (int i = 0; i < CARD_FIELDS; i++) {
        lv_obj_set_style_text_color(s_card_key[i], lv_color_hex(themed(COL_MUTED_DIM)), 0);
        lv_obj_set_style_text_color(s_card_val[i], lv_color_hex(s_col_ink), 0);
    }
    nina_empty_state_apply_theme(s_empty, current_theme, cfg_brightness());
}

static void adsb_apply_theme(void)
{
    if (!s_root) return;
    apply_colors();
    recompute();
}

/* ── Create ───────────────────────────────────────────────────────────── */

/** Translucent scrim strip over the disc (header / status). */
static lv_obj_t *mk_scrim(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCREEN_SIZE, h);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *adsb_page_create(lv_obj_t *parent)
{
    if (s_root) {
        return s_root;
    }

    if (!s_snap) {
        s_snap = heap_caps_calloc(1, sizeof(adsb_data_t), MALLOC_CAP_SPIRAM);
        if (!s_snap) {
            ESP_LOGE(TAG, "no PSRAM for the ADS-B snapshot buffer");
            return NULL;
        }
    }
    /* Not fatal: without it build_trail() no-ops and the page draws glyphs only. */
    if (!s_tb) {
        s_tb = heap_caps_calloc(1, sizeof(adsb_trailbuf_t), MALLOC_CAP_SPIRAM);
        if (!s_tb) ESP_LOGW(TAG, "no PSRAM for trails; drawing contacts only");
    }

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, SCREEN_SIZE, SCREEN_SIZE);
    /* Negate main_cont's OUTER_PADDING so the disc really reaches the edge
     * (same trick as nina_image_page / nina_spotify). */
    lv_obj_set_pos(s_root, -OUTER_PADDING, -OUTER_PADDING);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    /* The gesture must not scroll-chain up to main_cont and pan the page. */
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE |
                              LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                              LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    /* Data layer. Transparent and non-clickable so presses still land on the
     * root; exists purely so the STALE tier can dim everything at once. */
    s_content = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Draw host: full screen, transparent, one DRAW_MAIN_END callback. */
    s_disc = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_disc);
    lv_obj_set_size(s_disc, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(s_disc, 0, 0);
    lv_obj_clear_flag(s_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_disc, disc_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    for (int i = 0; i < 4; i++) {
        s_lbl_card[i] = mk_label(s_content, &lv_font_montserrat_22, 0xFFFFFF, "");
    }
    for (int i = 0; i < 3; i++) {
        s_lbl_ring[i] = mk_label(s_content, &lv_font_montserrat_18, COL_RING_LBL, "");
    }

    /* A tag is a bordered scrim box with its two lines inside, so the declutter
     * pass moves ONE object and the text never lands straight on a glyph. */
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        s_tag_box[i] = lv_obj_create(s_content);
        lv_obj_remove_style_all(s_tag_box[i]);
        lv_obj_set_size(s_tag_box[i], TAG_W, TAG_H);
        lv_obj_set_style_bg_opa(s_tag_box[i], LV_OPA_80, 0);
        lv_obj_set_style_border_width(s_tag_box[i], 1, 0);
        lv_obj_set_style_border_opa(s_tag_box[i], LV_OPA_60, 0);
        lv_obj_set_style_radius(s_tag_box[i], 3, 0);
        lv_obj_clear_flag(s_tag_box[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_tag_l1[i] = mk_label(s_tag_box[i], &lv_font_montserrat_20, 0xFFFFFF, "");
        lv_obj_set_pos(s_tag_l1[i], 8, 1);
        s_tag_l2[i] = mk_label(s_tag_box[i], &lv_font_montserrat_18, 0x808080, "");
        lv_obj_set_pos(s_tag_l2[i], 8, 25);
        show_obj(s_tag_box[i], false);
    }

    /* Board: lead block, five ranked rows, lead detail card. */
    s_board = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_board);
    lv_obj_set_size(s_board, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(s_board, 0, 0);
    lv_obj_clear_flag(s_board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_gkk = mk_label(s_board, &lv_font_montserrat_16, COL_THREAT, "");
    lv_obj_set_pos(s_lbl_gkk, BOARD_X + 10, 48);
    s_lbl_glance = mk_label(s_board, &lv_font_montserrat_64, 0xFFFFFF, "");
    lv_obj_set_pos(s_lbl_glance, BOARD_X + 8, 68);
    s_lbl_gsub = mk_label(s_board, &lv_font_montserrat_28, COL_SUB, "");
    lv_obj_set_pos(s_lbl_gsub, BOARD_X + 10, 152);
    clip_label(s_lbl_gsub, BOARD_W - 20);
    s_lbl_gsub2 = mk_label(s_board, &lv_font_montserrat_22, COL_MUTED, "");
    lv_obj_set_pos(s_lbl_gsub2, BOARD_X + 10, 190);

    /* Column headings, on the same x grid as the row fields below. */
    {
        static const char *heads[5] = { "CALLSIGN", "ROUTE", "ALT", "HDG", "DIST" };
        static const int   head_x[5] = { COL_CALL_X, COL_ROUTE_X, COL_ALT_X,
                                         COL_HDG_X, COL_DIST_X };
        for (int i = 0; i < 5; i++) {
            s_hdr_col[i] = mk_label(s_board, &lv_font_montserrat_16, COL_MUTED_DIM, heads[i]);
            lv_obj_set_pos(s_hdr_col[i], BOARD_X + head_x[i], BOARD_ROW_Y0 - 24);
        }
    }

    for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
        s_row_panel[i] = lv_obj_create(s_board);
        lv_obj_remove_style_all(s_row_panel[i]);
        lv_obj_set_size(s_row_panel[i], BOARD_W, BOARD_ROW_H);
        lv_obj_set_pos(s_row_panel[i], BOARD_X, BOARD_ROW_Y0 + i * BOARD_ROW_DY);
        lv_obj_set_style_bg_opa(s_row_panel[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_row_panel[i], 1, 0);
        lv_obj_set_style_radius(s_row_panel[i], 12, 0);
        lv_obj_clear_flag(s_row_panel[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        s_row_call[i] = mk_label(s_row_panel[i], &lv_font_montserrat_28, 0xFFFFFF, "");
        lv_obj_set_pos(s_row_call[i], COL_CALL_X, 6);
        clip_label(s_row_call[i], COL_ROUTE_X - COL_CALL_X - 6);
        s_row_route[i] = mk_label(s_row_panel[i], &lv_font_montserrat_22, COL_MUTED, "");
        lv_obj_set_pos(s_row_route[i], COL_ROUTE_X, 10);
        clip_label(s_row_route[i], COL_ALT_X - COL_ROUTE_X - 10);
        s_row_alt[i] = mk_label(s_row_panel[i], &lv_font_montserrat_28, 0xFFFFFF, "");
        lv_obj_set_pos(s_row_alt[i], COL_ALT_X, 6);
        s_row_hdg[i] = mk_label(s_row_panel[i], &lv_font_montserrat_22, COL_MUTED, "");
        lv_obj_set_pos(s_row_hdg[i], COL_HDG_X, 10);
        s_row_dist[i] = mk_label(s_row_panel[i], &lv_font_montserrat_22, COL_MUTED, "");
        lv_obj_set_pos(s_row_dist[i], COL_DIST_X, 10);
        show_obj(s_row_panel[i], false);
    }

    /* Lead-contact detail card: everything the receiver knows about rank 0
     * that the four lines above have no room for. Keys and positions are set
     * here once; fill_card() only ever writes the values. */
    {
        static const char *keys[CARD_FIELDS] = {
            "Registration", "Operator", "Aircraft", "Squawk", "Vertical rate",
            "Ground speed", "Altitude", "Last position", "Dist / bearing",
        };
        s_card = lv_obj_create(s_board);
        lv_obj_remove_style_all(s_card);
        lv_obj_set_size(s_card, BOARD_W, CARD_H);
        lv_obj_set_pos(s_card, BOARD_X, CARD_Y);
        lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_card, 1, 0);
        lv_obj_set_style_radius(s_card, 12, 0);
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        s_card_title = mk_label(s_card, &lv_font_montserrat_22, 0xFFFFFF, "");
        lv_obj_set_pos(s_card_title, CARD_K1_X, 6);
        clip_label(s_card_title, CARD_V2_X - CARD_K1_X - 12);

        s_card_mil = mk_label(s_card, &lv_font_montserrat_16, COL_LEAD_BG, "MILITARY");
        lv_obj_set_style_bg_color(s_card_mil, lv_color_hex(COL_THREAT), 0);
        lv_obj_set_style_bg_opa(s_card_mil, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_card_mil, 6, 0);
        lv_obj_set_style_pad_ver(s_card_mil, 2, 0);
        lv_obj_set_style_radius(s_card_mil, 3, 0);
        lv_obj_set_pos(s_card_mil, CARD_V2_X, 8);
        show_obj(s_card_mil, false);

        for (int i = 0; i < CARD_FIELDS; i++) {
            bool left = (i < CARD_ROWS_L);
            int  row  = left ? i : (i - CARD_ROWS_L);
            int  y    = CARD_ROW_Y0 + row * CARD_ROW_DY;
            s_card_key[i] = mk_label(s_card, &lv_font_montserrat_16, COL_MUTED_DIM, keys[i]);
            lv_obj_set_pos(s_card_key[i], left ? CARD_K1_X : CARD_K2_X, y + 3);
            /* Keys are clipped too: "Vertical rate" / "Dist / bearing" are the
             * long ones and must never reach into the value column. */
            clip_label(s_card_key[i], left ? (CARD_V1_X - CARD_K1_X - 8)
                                           : (CARD_V2_X - CARD_K2_X - 8));
            s_card_val[i] = mk_label(s_card, &lv_font_montserrat_18, 0xFFFFFF, "");
            lv_obj_set_pos(s_card_val[i], left ? CARD_V1_X : CARD_V2_X, y);
            clip_label(s_card_val[i], left ? (CARD_K2_X - CARD_V1_X - 12)
                                           : (BOARD_W - CARD_V2_X - 12));
        }
        show_obj(s_card, false);
    }

    /* Scrims LAST so they sit over the disc and the board. */
    s_hdr = mk_scrim(s_root, 0, HDR_H);
    s_lbl_title = mk_label(s_hdr, &lv_font_montserrat_22, 0xFFFFFF, "ADS-B");
    lv_obj_align(s_lbl_title, LV_ALIGN_LEFT_MID, 20, 0);
    s_lbl_mount = mk_label(s_hdr, &lv_font_montserrat_18, 0x808080, "");
    lv_obj_align(s_lbl_mount, LV_ALIGN_RIGHT_MID, -20, 0);

    s_strip = mk_scrim(s_root, SCREEN_SIZE - STRIP_H, STRIP_H);
    s_lbl_strip = mk_label(s_strip, &lv_font_montserrat_22, 0x808080, "");
    lv_obj_align(s_lbl_strip, LV_ALIGN_LEFT_MID, 20, 0);

    /* Empty state needs its own opaque backdrop (nina_empty_state is 80%
     * inline by contract; full-coverage consumers supply the ground). */
    s_backdrop = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_pos(s_backdrop, 0, 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    show_obj(s_backdrop, false);
    s_empty = nina_empty_state_create(s_backdrop, ICON_CLOUD_OFF,
                                      "Connecting to ADS-B receiver...",
                                      "Check the ADS-B receiver URL in settings.", 0);

    lv_obj_add_event_cb(s_root, press_cb,    LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_root, pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_root, released_cb, LV_EVENT_RELEASED, NULL);

    const app_config_t *cfg = app_config_get();
    s_mode   = (cfg->flights_mode > MODE_BOARD) ? MODE_SKY : cfg->flights_mode;
    s_up_deg = adsb_wrap360((float)cfg->flights_up_azimuth);

    apply_colors();
    apply_mode();
    recompute();
    return s_root;
}

/* ── Registry ops ─────────────────────────────────────────────────────── */

static lv_obj_t *adsb_get_obj(void)
{
    return s_root;
}

static bool adsb_is_available(void)
{
    return s_root != NULL && app_config_get()->flights_enabled;
}

static void adsb_on_show(void)
{
    adsb_page_active = true;     /* poll gate: the poller only runs while visible */
    nina_adsb_update();
}

static void adsb_on_hide(void)
{
    adsb_page_active = false;
    s_pressing = false;
}

static const page_ops_t s_adsb_ops = {
    .create       = adsb_page_create,
    .destroy      = NULL,
    .get_obj      = adsb_get_obj,
    .show         = adsb_on_show,
    .hide         = adsb_on_hide,
    .apply_theme  = adsb_apply_theme,
    .is_available = adsb_is_available,
};

const page_ops_t *nina_adsb_page_ops(void)
{
    return &s_adsb_ops;
}
