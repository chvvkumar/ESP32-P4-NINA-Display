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
 * cardinals, ring labels, the three Sky tag boxes, the header/status scrims,
 * the Scope corner blocks, the Board rows and the Board detail card. Nothing is
 * created per poll. The ONE exception is the Radar Scope contact labels: up to
 * ADSB_MAX_AC two-line texts drawn straight into the layer by disc_draw_cb()
 * (lv_draw_label over static strings filled by recompute()), because 64 label
 * objects repositioned per poll cost more than 64 draw tasks.
 *
 * RADAR SCOPE (redesign 2026-08-19): phosphor-green, no scrims. The header and
 * status strips are hidden; four corner blocks (contacts / max range / closest
 * aircraft / message rate) sit over the disc's outside corners with NO
 * background. Rim, rings, crosshairs, cardinals and ring numbers are green.
 * Every number that changes lives in its own fixed-width right-aligned label
 * (Montserrat digits are proportional, "1" is narrow) so the text never jumps.
 *
 * RED NIGHT: page_col() maps every colour the page emits to a red shade
 * (luminance into R) when theme_is_red_night(current_theme). All three modes.
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
 * travel it is a tap; beyond it on Sky/Scope it rotates flights_up_azimuth
 * live (the grabbed azimuth follows the finger), snapping to 5 deg on
 * release. Both writes go through a PSRAM config snapshot +
 * app_config_save_deferred(), which already debounces the ~350 ms NVS write by
 * ~2 s, so a burst of taps or a whole drag costs one flash write. A lone tap
 * cycles the mode, but only ADSB_DBL_TAP_MS after release, since a second tap
 * inside that window is a double tap instead: it toggles the text overlays
 * (rim labels, corner captions, Scope contact labels) off, leaving aircraft,
 * trails, rings, the ring distance numbers, the cardinals and the basemap on
 * screen; a second double tap brings the text back. RAM only, resets on the
 * next page build.
 *
 * RADAR SCOPE BASEMAP: a state-boundary picture from adsb_basemap.c
 * sits as the FIRST child of s_content (map_refresh()), under every family's
 * widgets, shown only while the Scope is the active mode and rotated to match
 * flights_up_azimuth. A second layer, s_text_layer, holds every text-bearing
 * object and is created LAST, after both family builders have run, so the
 * double tap above can hide all of it with one HIDDEN flag write.
 */

#include "nina_adsb.h"

#include "nina_dashboard_internal.h"   /* screen_size(), OUTER_PADDING, current_theme */
#include "nina_adsb_internal.h"
#include "ui_round.h"
#include "nina_empty_state.h"
#include "adsb_basemap.h"
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

/* The panel centre is a runtime value: 360 on the 4B and the 4C, 400 on the
 * 3.4C. Both were literal 360 before the round family existed. */
#define DISC_CX        (screen_center())
#define DISC_CY        (screen_center())

/* Outer disc radius, per mode, resolved once by apply_mode(). The Sky keeps the
 * panel edge on every board; the Scope pulls in to the rim circle on round so
 * the annulus outside it can carry text. */
static int s_disc_r;

#define NTICK_OUT      (s_disc_r - 3)
#define NTICK_IN       (s_disc_r - 20)

#define STRIP_H        44
#define HDR_H          44

#define ADSB_TAP_SLOP_PX 12
#define ADSB_SNAP_DEG    5.0f

/* Release-to-release window that turns a second tap into a double tap. A lone
 * tap now waits this long before cycling the mode -- the price of being able
 * to tell it apart from the first half of a double tap, since LVGL's own
 * click-streak count is only known AFTER this page's RELEASED handler runs. */
#define ADSB_DBL_TAP_MS  350

/* Family geometry: the cardinal radii, the ring-label inset, the tag height,
 * the scrim reserves and the declutter exclusion rectangles. Filled once at
 * page create by whichever family arm built the widgets. */
static adsb_geom_t s_geom;

/* Tag block width, and the height read back out of the geometry (60 square,
 * 76 round: the round lines are 28 over 28 to clear the 27 px floor). TAG_H
 * stays a macro because place_tag_box() uses it four times and clamp_tag()
 * once more. */
#define TAG_W  184
#define TAG_H  (s_geom.tag_h)

/* Pull-in of the two INNER ring numbers from their own ring. Only the outermost
 * number needs the family's larger inset (s_geom.ring_inset): at 34 the Scope's
 * 0.2 ring number would land 24 px from the centre, on top of the receiver
 * marker. */
#define ADSB_RING_INSET_INNER  22

/* Scope corner blocks (x1,y1,x2,y2). The tag scorer treats them like placed
 * tags so contact labels do not land on the corner text. */
#define CORNER_PAD   20
#define CORNER_TOP_H 84
#define CORNER_BL_Y  574
#define CORNER_BR_Y  640
#define CORNER_W_L   300
#define CORNER_W_R   240

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
#define MK_PLANE   0x10   /* Scope: heading-rotated silhouette (flights_icon_style 1) */

/* Silhouette classes for MK_PLANE, picked from readsb `category`. */
#define PLANE_JET    0    /* airliner: swept wings and tailplane   */
#define PLANE_SMALL  1    /* light aircraft: straight wing         */
#define PLANE_HELI   2    /* helicopter: body, boom, rotor cross   */

/* ── Fixed palette (the tar1090 domain convention; only the colour-brightness
 *    slider and the Red Night remap touch it, never the theme hues) ──────── */

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
 * still go through page_col() so the colour-brightness slider applies. */
#define COL_RING_OUT    0x4A5665   /* Sky: outer rim, 2 px, fully opaque */
#define COL_RING_IN     0x36404C   /* Sky: inner range/elevation rings   */
#define COL_RING_LBL    0x7E8B99   /* Sky: the "10" / "30 deg" numbers   */

/* Radar Scope phosphor greens. Rings/rim/cardinals/crosshairs take
 * COL_SCOPE_GREEN, the inner rings a dimmer green that reads about as strong
 * as COL_RING_IN did, ring numbers and corner captions sit in between. */
#define COL_SCOPE_GREEN    0x22C55E   /* rim, cardinals, corner data lines */
#define COL_SCOPE_RING_IN  0x1E7A45   /* inner rings, crosshair chords     */
#define COL_SCOPE_RING_LBL 0x3FA66A   /* ring numbers                      */
#define COL_SCOPE_CAP      0x2E9E5A   /* "CONTACTS" / "MAX RANGE" captions */
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
static lv_obj_t *s_tag_box[ADSB_TAG_COUNT];     /* Sky: scrim + border, the declutter unit */
static lv_obj_t *s_tag_l1[ADSB_TAG_COUNT];
static lv_obj_t *s_tag_l2[ADSB_TAG_COUNT];
/* Scope corner blocks. Every changing number is its own fixed-width,
 * right-aligned label; the units next to it are static text. */
static lv_obj_t *s_sc_cap_contacts;             /* TL caption "CONTACTS"          */
static lv_obj_t *s_sc_within;                   /* TL "%d / %d" (left aligned)    */
static lv_obj_t *s_sc_cap_range;                /* TR caption "MAX RANGE" (right) */
static lv_obj_t *s_sc_range;                    /* TR "%3d NM" (fixed, right)     */
static lv_obj_t *s_sc_call;                     /* BL closest callsign / hex      */
static lv_obj_t *s_sc_ident;                    /* BL "TYPE REG"                  */
static lv_obj_t *s_sc_alt;                      /* BL "N ft  N kt  NNN°" (left)   */
static lv_obj_t *s_sc_dist;                     /* BL "N.N NM" (left)             */
static lv_obj_t *s_sc_rate;                     /* BR "%5d msg/s" (fixed, right)  */
static lv_obj_t *s_sc_cue;                      /* BR "Reconnecting..." on STALE; on s_root so it never dims */
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
/* Round-family additions. NULL on square, where the page draws the numbers and
 * the text columns these replace. */
static lv_obj_t *s_scope_contacts_ring;         /* within/tracked as a rim arc  */
static lv_obj_t *s_scope_contacts_arclabel;     /* "CONTACTS n / m", bottom rim */
static lv_obj_t *s_scope_rate_arclabel;         /* "n msg/s", bottom rim        */
/* s_row_dot[] is per-contact coloured by fill_board() (distance over range on
 * the rail, ADSB_RAMP bucket for the colour), so apply_colors() never touches
 * it. It is also the round Board's own family test: NULL on square. */
static lv_obj_t *s_row_dot[ADSB_BOARD_ROWS];    /* distance dot on the rail     */
static lv_obj_t *s_row_rail[ADSB_BOARD_ROWS];   /* 2 px distance rail           */
/* Screen position of each round Board row's distance dot, in panel coordinates.
 * Written by fill_board(), read by the Board mark loop in the same pass:
 * lv_obj_get_x() would still be reporting the previous layout's coords. */
static int16_t s_row_dot_cx[ADSB_BOARD_ROWS];
static int16_t s_row_dot_cy[ADSB_BOARD_ROWS];
static lv_obj_t *s_lbl_legend;                  /* one range legend at the rail */
static lv_obj_t *s_card;                        /* lead-contact detail card */
static lv_obj_t *s_card_title;
static lv_obj_t *s_card_mil;                    /* "MILITARY" chip */
static lv_obj_t *s_card_key[CARD_FIELDS];
static lv_obj_t *s_card_val[CARD_FIELDS];
static lv_obj_t *s_backdrop;                    /* full-cover host for the empty state */
static lv_obj_t *s_empty;
/* Radar Scope basemap picture, first child of s_content (bottom of the whole
 * page). lv_image_dsc_t and the generation stamp are what map_refresh() uses
 * to decide whether adsb_basemap_render() needs to run again. */
static lv_obj_t      *s_map_img;
static lv_image_dsc_t s_map_dsc;
static uint32_t       s_map_gen;
/* Text overlay layer: every text-bearing object is reparented into this after
 * page create, so the double-tap toggle below is one HIDDEN flag write. */
static lv_obj_t *s_text_layer;
static bool      s_text_hidden;

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

/* Double-tap detection for the text-overlay toggle: a one-shot lv_timer that
 * fires the deferred single-tap mode cycle when no second tap arrives. */
static uint32_t    s_last_tap_ms;
static lv_timer_t *s_tap_timer;

/* Precomputed draw geometry (ints only — see file header) */
typedef struct {
    int16_t  x, y;
    int16_t  tx[3], ty[3];
    uint32_t color;
    uint8_t  opa;
    uint8_t  flags;
    uint8_t  shape;   /* PLANE_* class, valid iff MK_PLANE          */
    float    st, ct;  /* sin/cos of the heading, precomputed here so
                       * the draw callback multiplies but never calls
                       * trig (file header rule)                    */
} adsb_mark_t;

static adsb_mark_t s_mark[ADSB_MAX_AC];
static int         s_mark_n;

static int16_t s_ring_r[3];
static int     s_ring_n;

typedef struct { int16_t x, y, r; uint32_t color; } adsb_fov_t;
static adsb_fov_t s_fov[MAX_NINA_INSTANCES];
static int        s_fov_n;

typedef struct { int16_t x1, y1, x2, y2; } adsb_lead_t;
static adsb_lead_t s_lead[ADSB_MAX_AC];
static int         s_lead_n;

/* Radar Scope contact labels, drawn by disc_draw_cb() with lv_draw_label.
 * Filled for every drawn Scope contact in recompute(), then the first
 * flights_label_max of them (rank order) are PLACED by the declutter pass and
 * get `placed` set; the draw callback skips the rest. l1/l2 are static storage
 * so the draw task may keep the pointer (text_local = 0): recompute() and the
 * refresh both run under the display lock, never concurrently.
 * PSRAM: 64 x 48 B is 3 KB the UI task's .bss does not need. */
typedef struct {
    int16_t  x, y;        /* top-left of the TAG_W x TAG_H block */
    int16_t  mark;        /* index into s_mark (the glyph the leader reaches) */
    int16_t  gx, gy;      /* glyph position */
    int8_t   rank;        /* client rank: emergency first, then nearest */
    uint8_t  opa;
    bool     placed;
    uint32_t color;       /* arrow colour, already page_col()'d */
    char     l1[16];      /* callsign or hex (<= 10 chars)      */
    char     l2[16];      /* "%3d %c %3d" = 9 chars             */
} adsb_slbl_t;
static adsb_slbl_t *s_slbl;       /* [ADSB_MAX_AC], PSRAM; NULL = no labels */
static int          s_slbl_n;

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
/* Rim point of each cardinal (N E S W) at s_disc_r. The crosshairs are the N-S
 * and E-W chords through these, so the axes turn with the letters instead of
 * staying screen-aligned. */
static int16_t s_axis_x[4], s_axis_y[4];
static bool    s_show_rx;     /* Scope: receiver marker at the centre */

/* Theme-derived draw colours, refreshed by apply_colors() so the draw callback
 * never touches current_theme or the config mutex. */
static uint32_t s_col_line;      /* outer rim (mode dependent: grey / green) */
static uint32_t s_col_ring_in;   /* inner rings + crosshair chords */
static uint32_t s_col_dim;
static uint32_t s_col_ink;
static uint32_t s_col_emerg;     /* emergency halo, page_col()'d */

/* ── Forward declarations ─────────────────────────────────────────────── */

static void recompute(void);
static void apply_mode(void);
static void apply_colors(void);
static void apply_disc_colors(void);
static void persist_nav_fields(void);
static void map_refresh(bool force);
static void tap_timer_cb(lv_timer_t *t);

/**
 * Outer disc radius for @p mode. The Sky Dome keeps the panel edge on every
 * board (inscribed board 5 draws its horizon ring there). The Radar Scope pulls
 * in to the rim circle on a round panel, because the annulus outside it carries
 * the chord blocks. SCREEN_ROUND is a compile-time 0 or 1, so the branch folds
 * away and the square build keeps the shipped 356 on both modes.
 */
static int disc_r_for(uint8_t mode)
{
    int edge = screen_center() - 4;
    if (mode != MODE_SCOPE) {
        return edge;
    }
    return SCREEN_ROUND ? ui_rim_radius() : edge;
}

/* ── Small helpers ────────────────────────────────────────────────────── */

static int cfg_brightness(void)
{
    return app_config_get()->color_brightness;
}

static uint32_t themed(uint32_t raw)
{
    return app_config_apply_brightness(raw, cfg_brightness());
}

/**
 * The ONE colour gate for this page. Under a Red Night theme every colour the
 * page emits (ring greys, the altitude ramp, rig hues, the scope greens,
 * scrims, board rows) collapses to a red shade: Rec.601 luminance into R,
 * G = B = 0. A colour that is already a pure red shade (the Red Night theme's
 * own text/label/border values, black, and anything already mapped) passes
 * through untouched, so the function is idempotent. The colour-brightness
 * slider is applied last in both branches.
 */
static uint32_t page_col(uint32_t raw)
{
    if (theme_is_red_night(current_theme) && (raw & 0x00FFFFu) != 0u) {
        uint32_t r = (raw >> 16) & 0xFFu;
        uint32_t g = (raw >> 8) & 0xFFu;
        uint32_t b = raw & 0xFFu;
        uint32_t luma = (299u * r + 587u * g + 114u * b) / 1000u;
        raw = luma << 16;
    }
    return themed(raw);
}

static uint32_t col_bg(void)
{
    return page_col(current_theme ? current_theme->bg_main : 0x050505);
}

static uint32_t col_text(void)
{
    return page_col(current_theme ? current_theme->text_color : 0xE5E7EB);
}

static uint32_t col_label(void)
{
    return page_col(current_theme ? current_theme->label_color : 0x6B7280);
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

/** Scope-label vertical-rate cue (spec 2026-08-19): +-256 fpm dead band and a
 *  blank, not a dash, for level flight. */
static char scope_vc(float fpm)
{
    if (fpm >=  256.0f) return '^';
    if (fpm <= -256.0f) return 'v';
    return ' ';
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
    /* Some transponders broadcast an all-zero ident ("00000000"): treat it as
     * none so the hex shows instead of a row of zeros. */
    const char *f = a->flight;
    while (*f == '0') f++;
    return (a->flight[0] != '\0' && *f != '\0') ? a->flight : a->hex;
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

#if !CONFIG_NINA_FAMILY_ROUND
static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                          const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}
#endif

#if !CONFIG_NINA_FAMILY_ROUND
/** Fixed-width, right-aligned label at (x, y): the anchor for every Scope
 *  corner number. Montserrat digits are proportional ("1" is half a "0"), so a
 *  free-width label walks its neighbours around on every poll; pinning the
 *  width and aligning right keeps the right edge, and the unit after it, still. */
static lv_obj_t *mk_num(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                        int x, int y, int w)
{
    lv_obj_t *l = mk_label(parent, font, color, "");
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}
#endif

#if !CONFIG_NINA_FAMILY_ROUND
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
#endif

static void show_obj(lv_obj_t *o, bool visible)
{
    if (!o) return;
    if (visible) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/** Text write that tolerates a handle the family builder did not create. */
/* Text setter for a label OR a rim arclabel (the round Scope builds the
 * nearest-aircraft rows as arclabels): the widget class picks the setter.
 * lv_arclabel_set_text() re-lays out the run on every call, but every caller
 * here runs once per poll at most, so no shadow copy is kept. */
static void set_lbl(lv_obj_t *l, const char *t)
{
    if (!l || !t) return;
#if LV_USE_ARCLABEL
    if (lv_obj_check_type(l, &lv_arclabel_class)) {
        lv_arclabel_set_text(l, t);
        return;
    }
#endif
    lv_label_set_text(l, t);
}

/** Colour write with the same tolerance. */
static void set_col(lv_obj_t *o, uint32_t c)
{
    if (o) lv_obj_set_style_text_color(o, lv_color_hex(c), 0);
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

/* ── Aircraft silhouettes (flights_icon_style 1) ─────────────────────────
 * Nose-up local coordinates (x right, y down, nose toward -y), one row per
 * filled triangle: x0,y0,x1,y1,x2,y2. Sized to the 30 px triangle class the
 * arrows use (~38 px long, ~34 px span for the jet). Rotation happens in
 * draw_plane with the sin/cos precomputed by recompute(). */

static const int8_t JET_TRIS[][6] = {
    {   0, -19,   2, -13,  -2, -13 },   /* nose cone            */
    {  -2, -13,   2, -13,   2,  19 },   /* fuselage quad        */
    {  -2, -13,   2,  19,  -2,  19 },
    {   2,  -4,  17,   9,  17,  12 },   /* right wing quad      */
    {   2,  -4,  17,  12,   2,   5 },
    {  -2,  -4, -17,   9, -17,  12 },   /* left wing quad       */
    {  -2,  -4, -17,  12,  -2,   5 },
    {   1,  13,   9,  18,   1,  17 },   /* right tailplane      */
    {  -1,  13,  -9,  18,  -1,  17 },   /* left tailplane       */
};

static const int8_t SMALL_TRIS[][6] = {
    {   0, -14,   2, -10,  -2, -10 },   /* nose cone            */
    {  -2, -10,   2, -10,   2,  14 },   /* fuselage quad        */
    {  -2, -10,   2,  14,  -2,  14 },
    { -16,  -6,  16,  -6,  16,  -1 },   /* straight wing quad   */
    { -16,  -6,  16,  -1, -16,  -1 },
    {  -7,  10,   7,  10,   7,  14 },   /* straight tailplane   */
    {  -7,  10,   7,  14,  -7,  14 },
};

static const int8_t HELI_TRIS[][6] = {
    {   0,  -9,   5,  -2,  -5,  -2 },   /* diamond fuselage     */
    {  -5,  -2,   5,  -2,   0,   4 },
    {  -1,   4,   1,   4,   1,  15 },   /* tail boom quad       */
    {  -1,   4,   1,  15,  -1,  15 },
};

/* Two crossed rotor blades over the fuselage centre (0,-2), ~26 px tip to
 * tip, drawn as line segments: x1,y1,x2,y2. */
static const int8_t HELI_BLADES[2][4] = {
    {  -9, -11,   9,   7 },
    {   9, -11,  -9,   7 },
};

/** Heading-rotated silhouette at (x,y). Same clockwise screen rotation as the
 *  MK_TRI nose[] path: rx = px*ct - py*st, ry = px*st + py*ct. */
static void draw_plane(lv_layer_t *layer, int x, int y, float st, float ct,
                       uint8_t shape, uint32_t color, lv_opa_t opa)
{
    const int8_t (*tris)[6];
    int n;
    switch (shape) {
    case PLANE_SMALL: tris = SMALL_TRIS; n = (int)(sizeof(SMALL_TRIS) / sizeof(SMALL_TRIS[0])); break;
    case PLANE_HELI:  tris = HELI_TRIS;  n = (int)(sizeof(HELI_TRIS)  / sizeof(HELI_TRIS[0]));  break;
    default:          tris = JET_TRIS;   n = (int)(sizeof(JET_TRIS)   / sizeof(JET_TRIS[0]));   break;
    }
    for (int i = 0; i < n; i++) {
        int16_t xs[3], ys[3];
        for (int k = 0; k < 3; k++) {
            float px = (float)tris[i][2 * k];
            float py = (float)tris[i][2 * k + 1];
            xs[k] = (int16_t)(x + (int)(px * ct - py * st));
            ys[k] = (int16_t)(y + (int)(px * st + py * ct));
        }
        draw_tri(layer, xs, ys, color, opa);
    }
    if (shape == PLANE_HELI) {
        for (int i = 0; i < 2; i++) {
            float x1 = (float)HELI_BLADES[i][0], y1 = (float)HELI_BLADES[i][1];
            float x2 = (float)HELI_BLADES[i][2], y2 = (float)HELI_BLADES[i][3];
            draw_seg(layer,
                     x + (int)(x1 * ct - y1 * st), y + (int)(x1 * st + y1 * ct),
                     x + (int)(x2 * ct - y2 * st), y + (int)(x2 * st + y2 * ct),
                     color, 2, opa);
        }
    }
}

static void disc_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) return;

    bool scope = (s_mode == MODE_SCOPE);
    /* The round Board keeps the draw host visible so the heading arrows on the
     * distance rails go through this same callback; none of the disc furniture
     * belongs on it. On square the host is hidden in Board mode, so the branch
     * never fires there and the disc modes are untouched. */
    bool board = (s_mode == MODE_BOARD);

    if (!board) {
        /* Rings, outermost first so the inner hairlines land on top. Opaque and
         * 2 px: at LV_OPA_50 over a 1 px arc these were invisible on the panel. */
        draw_ring(layer, DISC_CX, DISC_CY, s_disc_r, s_col_line,
                  s_geom.rim_w[scope ? 1 : 0], LV_OPA_COVER);
        for (int i = 0; i < s_ring_n; i++) {
            if (s_ring_r[i] > 6) {
                draw_ring(layer, DISC_CX, DISC_CY, s_ring_r[i], s_col_ring_in, 2, LV_OPA_COVER);
            }
        }

        /* Cross hairs through the centre, plus the true-north tick. Both chords
         * are rotated with the compass (endpoints from place_compass) so the
         * N-S line runs through the N and S letters. The Scope's chords are the
         * dim green already, so they get more opacity than the Sky's grey
         * hairlines. */
        lv_opa_t chord_opa = scope ? LV_OPA_70 : LV_OPA_40;
        draw_seg(layer, s_axis_x[0], s_axis_y[0], s_axis_x[2], s_axis_y[2],
                 s_col_ring_in, 1, chord_opa);
        draw_seg(layer, s_axis_x[1], s_axis_y[1], s_axis_x[3], s_axis_y[3],
                 s_col_ring_in, 1, chord_opa);
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
            draw_ring(layer, m->x, m->y, 26, s_col_emerg, 3, LV_OPA_60);
        }
        if (m->flags & MK_PLANE) {
            draw_plane(layer, m->x, m->y, m->st, m->ct, m->shape, m->color, m->opa);
        } else if (m->flags & MK_TRI) {
            draw_tri(layer, m->tx, m->ty, m->color, m->opa);
        } else if (m->flags & MK_SQUARE) {
            draw_square(layer, m);
        } else {
            draw_diamond(layer, m);
        }
    }

    /* Radar Scope contact labels: two lines of text in the arrow's colour, no
     * box, no background. Same 8 px / 2 px / 31 px offsets as the Sky tag box
     * so the declutter geometry (TAG_W x TAG_H) stays honest. Suppressed by
     * the double-tap text toggle, same as every other label on the page. */
    if (scope && s_slbl && !s_text_hidden) {
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        for (int i = 0; i < s_slbl_n; i++) {
            const adsb_slbl_t *s = &s_slbl[i];
            if (!s->placed) continue;
            ld.color = lv_color_hex(s->color);
            ld.opa   = s->opa;
            ld.font  = s_geom.tag_font1;
            ld.text  = s->l1;
            lv_area_t a1 = { s->x + 8, s->y + s_geom.tag_l1_y,
                             s->x + TAG_W, s->y + s_geom.tag_l1_y + 27 };
            lv_draw_label(layer, &ld, &a1);
            ld.font  = s_geom.tag_font2;
            ld.text  = s->l2;
            lv_area_t a2 = { s->x + 8, s->y + s_geom.tag_l2_y,
                             s->x + TAG_W, s->y + s_geom.tag_l2_y + 32 };
            lv_draw_label(layer, &ld, &a2);
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
        /* Second tap inside the window: toggle the text overlay and cancel
         * the pending single-tap mode cycle. Otherwise arm the timer and wait
         * to see if a second tap follows. */
        uint32_t now = lv_tick_get();
        if (s_tap_timer && lv_tick_elaps(s_last_tap_ms) <= ADSB_DBL_TAP_MS) {
            lv_timer_delete(s_tap_timer);
            s_tap_timer = NULL;
            s_text_hidden = !s_text_hidden;
            show_obj(s_text_layer, !s_text_hidden);
            lv_obj_invalidate(s_disc);
            return;
        }
        s_last_tap_ms = now;
        if (s_tap_timer) {
            lv_timer_delete(s_tap_timer);
        }
        s_tap_timer = lv_timer_create(tap_timer_cb, ADSB_DBL_TAP_MS, NULL);
        lv_timer_set_repeat_count(s_tap_timer, 1);
        return;
    }
    if (s_mode == MODE_BOARD) {
        return;
    }
    s_up_deg = adsb_wrap360(roundf(s_up_deg / ADSB_SNAP_DEG) * ADSB_SNAP_DEG);
    recompute();
    map_refresh(true);   /* rotation changed; renders once, at drag end only */
    persist_nav_fields();
}

/**
 * Fires ADSB_DBL_TAP_MS after a lone tap with no second tap following: cycles
 * the mode the way every tap used to. A one-shot lv_timer (repeat count 1)
 * deletes itself right after invoking this callback, so this must only NULL
 * the pointer and never call lv_timer_delete() on it.
 */
static void tap_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_tap_timer = NULL;
    s_mode = (uint8_t)((s_mode + 1) % 3);
    apply_mode();
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

/* ── Radar Scope basemap ──────────────────────────────────────────────── */

/**
 * Show, hide or repaint the basemap image under the Radar Scope.
 *
 * force=false is the cheap poll-tick path: it only re-renders when
 * adsb_basemap_generation() has moved past the last frame this page drew, so
 * a steady Scope view costs nothing extra between fetches. force=true is used
 * whenever something that changes the PICTURE itself happened outside a new
 * fetch: a mode switch, the end of a rotate drag, a Red Night remap or a
 * theme change.
 */
static void map_refresh(bool force)
{
    if (s_mode != MODE_SCOPE || !s_map_img) {
        show_obj(s_map_img, false);
        return;
    }
    uint32_t gen = adsb_basemap_generation();
    if (!force && gen == s_map_gen && s_map_dsc.data != NULL) {
        return;
    }
    int side = 0;
    const uint16_t *buf = adsb_basemap_render(s_up_deg, &side);
    if (!buf) {
        show_obj(s_map_img, false);
        return;
    }
    s_map_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_map_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_map_dsc.header.flags  = 0;
    s_map_dsc.header.w      = (uint16_t)side;
    s_map_dsc.header.h      = (uint16_t)side;
    s_map_dsc.header.stride = (uint16_t)(side * 2);
    s_map_dsc.data_size     = (uint32_t)side * (uint32_t)side * 2u;
    s_map_dsc.data          = (const uint8_t *)buf;
    /* RGB565 is an uncompressed "true colour" format, so LVGL's built-in
     * decoder uses this buffer directly and never enters it into the image
     * cache (lv_bin_decoder.c: the uncompressed-format path sets
     * use_directly = true and skips the cache-add call) -- no cache-drop is
     * needed even though &s_map_dsc keeps the same address every frame. */
    lv_image_set_src(s_map_img, &s_map_dsc);
    lv_obj_invalidate(s_map_img);
    show_obj(s_map_img, true);
    s_map_gen = gen;
}

/* ── Mode layout ──────────────────────────────────────────────────────── */

static void apply_mode(void)
{
    bool disc_mode = (s_mode != MODE_BOARD);
    bool scope     = (s_mode == MODE_SCOPE);
    adsb_basemap_set_scope(scope, disc_r_for(MODE_SCOPE));
    s_disc_r = disc_r_for(s_mode);

    /* The round Board draws heading arrows through the same draw callback, so
     * the host stays visible there; the callback skips the rings in Board mode.
     * board_marks is false on square, where the host hides exactly as before. */
    show_obj(s_disc, disc_mode || s_geom.board_marks);
    for (int i = 0; i < 4; i++) show_obj(s_lbl_card[i], disc_mode);
    for (int i = 0; i < 3; i++) show_obj(s_lbl_ring[i], disc_mode);
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        show_obj(s_tag_box[i], false);   /* recompute() re-shows the live ones */
    }
    show_obj(s_board, !disc_mode);
    /* The mount pointing is an observing aid; the Board is flight awareness
     * and carries no elevation or azimuth at all. */
    show_obj(s_lbl_mount, disc_mode);

    /* Radar Scope: no scrims at all, corner blocks instead. On round the Board
     * draws none either (inscribed board 7 has no caps), and the caps would dim
     * the top row and the legend; square keeps both on the Board as today. */
    bool caps = !scope && !(s_geom.board_marks && s_mode == MODE_BOARD);
    show_obj(s_hdr,   caps);
    show_obj(s_strip, caps);
    lv_obj_t *corners[] = {
        s_sc_cap_contacts, s_sc_within, s_sc_cap_range, s_sc_range,
        s_sc_call, s_sc_ident, s_sc_alt, s_sc_dist, s_sc_rate, s_sc_cue,
        s_scope_contacts_ring, s_scope_contacts_arclabel, s_scope_rate_arclabel,
    };
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        show_obj(corners[i], scope);
    }
    apply_disc_colors();   /* ends in map_refresh(true): mode and colours in one render */
}

/* ── Coordinate pipeline ──────────────────────────────────────────────── */

/* Tag boxes already committed this cycle — the declutter pass tests against
 * these and nothing else. Reset alongside s_lead_n in recompute(). */
static lv_area_t s_tag_area[ADSB_MAX_AC];
static int       s_tag_area_n;

/* Ring-number label boxes ("10", "50 NM"), recorded by place_ring_label() so
 * a contact label is not parked on top of one. */
static lv_area_t s_ring_lbl_area[3];
static bool      s_ring_lbl_used[3];     /* slot placed this cycle */

#if !CONFIG_NINA_FAMILY_ROUND
/**
 * The four Scope corner text blocks, scored like placed tags so a contact label
 * does not land on the corner text. Filled at page create rather than being a
 * static initialiser: the panel width is a runtime value, so the right edge
 * cannot be a constant expression.
 */
static void adsb_fill_corner_areas(void)
{
    const int32_t w = (int32_t)screen_size();
    s_geom.no_go[0] = (lv_area_t){ 0,              0,           CORNER_W_L, CORNER_TOP_H };
    s_geom.no_go[1] = (lv_area_t){ w - CORNER_W_R, 0,           w,          CORNER_TOP_H };
    s_geom.no_go[2] = (lv_area_t){ 0,              CORNER_BL_Y, CORNER_W_L, w            };
    s_geom.no_go[3] = (lv_area_t){ w - CORNER_W_R, CORNER_BR_Y, w,          w            };
    s_geom.no_go_n  = 4;
}
#endif

static bool boxes_hit(const lv_area_t *a, const lv_area_t *b)
{
    return !(a->x2 < b->x1 || b->x2 < a->x1 || a->y2 < b->y1 || b->y2 < a->y1);
}

/** Keep a TAG_W x TAG_H block inside the disc, clear of both scrims. */
static void clamp_tag(int *ax, int *ay)
{
    int idx = (s_mode == MODE_SCOPE) ? 1 : 0;
    if (*ax < 6)                          *ax = 6;
    if (*ax > screen_size() - TAG_W - 6)  *ax = screen_size() - TAG_W - 6;
    if (*ay < s_geom.scrim_top[idx] + 4)  *ay = s_geom.scrim_top[idx] + 4;
    if (*ay > screen_size() - s_geom.scrim_bot[idx] - s_geom.tag_h - 4) {
        *ay = screen_size() - s_geom.scrim_bot[idx] - s_geom.tag_h - 4;
    }
}

/** Innermost drawn ring, the "crowded middle" threshold for the leader length.
 *  place_rings() runs before any tag is placed, so s_ring_r is current. */
static int inner_ring_r(void)
{
    return (s_ring_n > 0 && s_ring_r[0] > 20) ? s_ring_r[0] : (s_disc_r / 3);
}

/**
 * A tag block on a round panel must keep all four corners inside the rim
 * circle: a box that satisfies the rectangular clamp can still hang off the
 * glass at 45 degrees. SCREEN_ROUND is a compile-time 0 or 1, so the whole body
 * folds away on square.
 */
static int rim_penalty(const lv_area_t *box)
{
    if (!SCREEN_ROUND) {
        return 0;
    }
    int rs = ui_rim_radius();
    int cx = screen_center(), cy = screen_center();
    const int xs[2] = { box->x1, box->x2 };
    const int ys[2] = { box->y1, box->y2 };
    int worst = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int dx = xs[i] - cx, dy = ys[j] - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > worst) worst = d2;
        }
    }
    return (worst > rs * rs) ? 6 : 0;
}

/** Cost of putting a tag box here: lower is better. A hidden glyph costs 1, a
 *  hidden tag box costs 4 (two lines of text lost, not one triangle) and a
 *  scrim overlap 2. @p skip is the tagged contact's own glyph, which the leader
 *  line is supposed to reach. Glyph half-extent is 20 px (18 px triangle nose
 *  plus a margin). */
static int tag_score(const lv_area_t *box, int skip)
{
    int s = 0;
    for (int i = 0; i < s_mark_n; i++) {
        if (i == skip) continue;
        int gx = s_mark[i].x, gy = s_mark[i].y;
        if (gx + 20 < box->x1 || gx - 20 > box->x2) continue;
        if (gy + 20 < box->y1 || gy - 20 > box->y2) continue;
        s++;
    }
    for (int i = 0; i < s_tag_area_n; i++) {
        if (boxes_hit(box, &s_tag_area[i])) s += 4;
    }
    if (s_mode == MODE_SCOPE) {
        for (int i = 0; i < s_geom.no_go_n; i++) {
            if (boxes_hit(box, &s_geom.no_go[i])) s += 4;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (s_ring_lbl_used[i] && boxes_hit(box, &s_ring_lbl_area[i])) s += 3;
    }
    int idx = (s_mode == MODE_SCOPE) ? 1 : 0;
    if (box->y1 < s_geom.scrim_top[idx] || box->y2 > screen_size() - s_geom.scrim_bot[idx]) s += 2;
    s += rim_penalty(box);
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
 * Cost is tags x 8 candidates x (<=64 marks + placed tags), integer compares
 * only: 3 tags on the Sky, up to ADSB_MAX_AC on the Scope.
 * ponytail: O(labels x marks) per recompute, ~65K compares worst case at 64
 * labels; fine at touch rate on the P4. A grid bucket would cut it if the
 * drag ever stutters.
 *
 * Returns the chosen block in @p out and records it in s_tag_area / s_lead
 * (the leader is dropped when the block touches the glyph, as before).
 */
static void place_tag_box(int x, int y, int mark_idx, lv_area_t *out)
{
    static const int8_t qx[4] = {  1,  1, -1, -1 };
    static const int8_t qy[4] = {  1, -1, -1,  1 };
    static const int8_t vstep[4] = { 1, -1, 2, -2 };
    int start = (x >= DISC_CX) ? ((y >= DISC_CY) ? 0 : 1)
                               : ((y >= DISC_CY) ? 3 : 2);

    int dx0 = x - DISC_CX, dy0 = y - DISC_CY;
    int inner = inner_ring_r();
    bool crowded = (dx0 * dx0 + dy0 * dy0) < (inner * inner);
    int gap_x = crowded ? 70 : 26;
    int gap_y = crowded ? 24 : 12;

    int best_score = -1;
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
            best   = box;
            if (sc == 0) break;      /* clean air: nothing can beat it */
        }
    }

    *out = best;

    if (s_tag_area_n < ADSB_MAX_AC) {
        s_tag_area[s_tag_area_n++] = best;
    }
    if (s_lead_n < ADSB_MAX_AC) {
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

/** Sky Dome: place one of the three boxed tags. */
static void place_tag(int slot, int x, int y, int mark_idx,
                      const char *l1, const char *l2)
{
    /* review_impl_D3 M-1: the seam promises that a family builder may leave any
     * slot unbuilt, so the disc chrome null-checks too. Both families build the
     * tags today, so this never fires. */
    if (!s_tag_box[slot]) {
        return;
    }
    lv_area_t box;
    place_tag_box(x, y, mark_idx, &box);
    set_lbl(s_tag_l1[slot], l1);
    set_lbl(s_tag_l2[slot], l2);
    lv_obj_set_pos(s_tag_box[slot], box.x1, box.y1);
    show_obj(s_tag_box[slot], true);
}

/**
 * Radar Scope: place the text labels for the first @p max drawn contacts in
 * rank order (the client ranks nearest-first; ranks are contiguous from 0).
 * s_slbl[] already holds every drawn contact; this marks the winners `placed`
 * and gives each its block origin. Nearest first, so rank 0 gets the pick of
 * the free air, exactly as the Sky tags do.
 */
static void place_scope_labels(int max)
{
    if (!s_slbl || max <= 0) return;
    /* Rank order = emergency first, then nearest to the receiver (client's
     * rank_contacts); no telescope input on the Scope. Selection pass, n <= 64. */
    for (int placed = 0; placed < max; placed++) {
        adsb_slbl_t *best = NULL;
        for (int i = 0; i < s_slbl_n; i++) {
            adsb_slbl_t *s = &s_slbl[i];
            if (s->placed) continue;
            if (!best || s->rank < best->rank) best = s;
        }
        if (!best) break;
        int ax = best->gx, ay = best->gy;
        if (s_geom.scope_lbl_r > 0) {
            /* Ride outward along the contact's own bearing, stopped short of
             * the bezel: this is the round Scope's declutter rule and the one
             * place a label never sits on a contact. */
            int dx = ax - DISC_CX, dy = ay - DISC_CY;
            float len = sqrtf((float)(dx * dx + dy * dy));
            if (len > 1.0f) {
                float want = len + 48.0f;
                if (want > (float)s_geom.scope_lbl_r) {
                    want = (float)s_geom.scope_lbl_r;
                }
                if (want > len) {
                    ax = DISC_CX + (int)((float)dx * want / len);
                    ay = DISC_CY + (int)((float)dy * want / len);
                }
            }
        }
        lv_area_t box;
        int lead_before = s_lead_n;
        place_tag_box(ax, ay, best->mark, &box);
        if ((ax != best->gx || ay != best->gy) && s_lead_n > lead_before) {
            /* place_tag_box() anchored the leader on the pushed point; the line
             * has to reach the contact itself or it floats. Snapshot the count
             * rather than assuming a bare s_lead_n > 0: only reset the leader
             * this call actually appended. */
            s_lead[s_lead_n - 1].x1 = (int16_t)best->gx;
            s_lead[s_lead_n - 1].y1 = (int16_t)best->gy;
        }
        best->x = (int16_t)box.x1;
        best->y = (int16_t)box.y1;
        best->placed = true;
    }
}

/** Cardinals and the true-north tick, both functions of the rotation only. */
static void place_compass(void)
{
    static const char *names[4] = { "N", "E", "S", "W" };
    for (int i = 0; i < 4; i++) {
        float t = (i * 90.0f - s_up_deg) * ADSB_DEG2RAD;
        float si = sinf(t), co = cosf(t);
        /* The pull-in is chosen by the letter's SCREEN direction, not by the
         * letter, so the asymmetry that clears the two round chord caps
         * survives a rotation. Both offsets are 24 on square, which is the
         * shipped CARD_R at every angle. */
        int idx = (s_mode == MODE_SCOPE) ? 1 : 0;
        int off = (co < 0.0f ? -co : co) > 0.707f ? s_geom.card_off_v[idx]
                                                  : s_geom.card_off_h[idx];
        /* Round Scope: rim arclabels own all four diagonals, 15..75 degrees
         * either side of up and of down (nearest aircraft on top, CONTACTS
         * and msg/s at the bottom), in the same rim band as the letters
         * (bench B11 showed the collision, "1W5 NM"). A letter the rotation
         * carries there steps in under the glyph band. */
        if (idx == 1) {
            float as = si < 0.0f ? -si : si;
            if (as > 0.259f && as < 0.966f) off = s_geom.card_off_diag;
        }
        int cr = s_disc_r - off;
        int x = DISC_CX + (int)(cr * si);
        int y = DISC_CY - (int)(cr * co);
        /* Same bearing at the rim: the two crosshair chords. */
        s_axis_x[i] = (int16_t)(DISC_CX + (int)(s_disc_r * si));
        s_axis_y[i] = (int16_t)(DISC_CY - (int)(s_disc_r * co));
        /* Keep the letter clear of the header and status scrims: a letter
         * that the rotation carries to the very top or bottom slides
         * inward instead of vanishing under the strip text. The crosshair
         * endpoints above are set either way (review_impl_D3 M-1). */
        int ly = y - 16;
        if (ly < s_geom.scrim_top[idx] + 2)                  ly = s_geom.scrim_top[idx] + 2;
        if (ly > screen_size() - s_geom.scrim_bot[idx] - 34) ly = screen_size() - s_geom.scrim_bot[idx] - 34;
        if (s_lbl_card[i]) {
            set_lbl(s_lbl_card[i], names[i]);
            lv_obj_set_pos(s_lbl_card[i], x - 12, ly);
        }
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
    /* No number, no rectangle to reserve (review_impl_D3 M-1). */
    if (!s_lbl_ring[slot]) {
        s_ring_lbl_used[slot] = false;
        return;
    }
    if (s_geom.ring_lbl_west && s_mode == MODE_SCOPE) {
        /* Round Scope: the numbers run along the W axis and follow it round
         * when the up azimuth rotates. Each label is centred on the W bearing
         * just inside its ring (the range label steps in past the W cardinal,
         * which sits card_off_h inside the rim with its glyph reaching about
         * 40 px in), then pushed one line toward N so it sits beside the axis
         * chord rather than on it. The text itself stays upright. Width and
         * height come from the laid-out label, so a two-digit and a "125 NM"
         * label both centre on their own ink. */
        lv_obj_t *l = s_lbl_ring[slot];
        set_lbl(l, text);
        show_obj(l, true);   /* before the layout pass, so the size is live */
        lv_obj_update_layout(l);
        int w = lv_obj_get_width(l);
        int h = lv_obj_get_height(l);
        float tw = (270.0f - s_up_deg) * ADSB_DEG2RAD;   /* W bearing on screen */
        float tn = (-s_up_deg) * ADSB_DEG2RAD;           /* N bearing on screen */
        int rr = r - ((slot == 2) ? s_geom.ring_inset + 14 : 6) - w / 2;
        int side = h / 2 + 4;
        int cxl = DISC_CX + (int)(rr * sinf(tw) + side * sinf(tn));
        int cyl = DISC_CY - (int)(rr * cosf(tw) + side * cosf(tn));
        int x = cxl - w / 2;
        int y = cyl - h / 2;
        lv_obj_set_pos(l, x, y);
        s_ring_lbl_area[slot] = (lv_area_t){ x - 4, y - 4, x + w + 4, y + h + 4 };
        s_ring_lbl_used[slot] = true;
        return;
    }
    /* Only the outermost number takes the family inset; the two inner ones
     * would otherwise crawl toward the centre on the Scope. */
    int inset = (slot == 2) ? s_geom.ring_inset : ADSB_RING_INSET_INNER;
    int d = (int)(0.707f * (float)(r - inset));
    set_lbl(s_lbl_ring[slot], text);
    lv_obj_set_pos(s_lbl_ring[slot], DISC_CX - d - 20, DISC_CY - d - 14);
    show_obj(s_lbl_ring[slot], true);
    /* "50 NM" at 22 px is about 72 x 26; over-cover slightly for the margin.
     * ring_lbl_w is the family's face width (84 square, 100 round: the 28 px
     * round face is wider than the 22 px square one). */
    s_ring_lbl_area[slot] = (lv_area_t){ DISC_CX - d - 24, DISC_CY - d - 18,
                                         DISC_CX - d - 24 + s_geom.ring_lbl_w,
                                         DISC_CY - d + 14 };
    s_ring_lbl_used[slot] = true;
}

/** Ring radii + their labels. Sky gets 60/30 plus the gate at the rim. */
static void place_rings(float gate, float range)
{
    s_ring_n = 0;
    for (int i = 0; i < 3; i++) s_ring_lbl_used[i] = false;
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
                             (float)DISC_CX, (float)DISC_CY, (float)s_disc_r, &x, &y);
            int r = (int)(DISC_CY - y);
            s_ring_r[s_ring_n++] = (int16_t)r;
            snprintf(buf, sizeof(buf), "%d\xc2\xb0", (int)tiers[i]);
            place_ring_label(i, r, buf);
        }
        /* The rim IS the gate: label it rather than drawing a ring on top. */
        snprintf(buf, sizeof(buf), "%d\xc2\xb0", (int)gate);
        place_ring_label(2, s_disc_r, buf);
    } else {
        const float frac[2] = { 0.2f, 0.5f };
        for (int i = 0; i < 2; i++) {
            int r = (int)(s_disc_r * frac[i]);
            s_ring_r[s_ring_n++] = (int16_t)r;
            snprintf(buf, sizeof(buf), "%d", (int)(range * frac[i] + 0.5f));
            place_ring_label(i, r, buf);
        }
        snprintf(buf, sizeof(buf), "%d NM", (int)(range + 0.5f));
        place_ring_label(2, s_disc_r, buf);
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
                         (float)DISC_CX, (float)DISC_CY, (float)s_disc_r, &cxp, &cyp);
        float half = pt[i].fov_deg * 0.5f;
        float edge_el = pt[i].alt_deg + half;
        if (edge_el > 89.5f) edge_el = pt[i].alt_deg - half;
        adsb_sky_project(pt[i].az_deg, edge_el, gate, s_up_deg,
                         (float)DISC_CX, (float)DISC_CY, (float)s_disc_r, &exp_x, &exp_y);

        float dx = exp_x - cxp, dy = exp_y - cyp;
        int r = (int)(sqrtf(dx * dx + dy * dy) + 0.5f);
        if (r < 6) r = 6;      /* a true 1.2 deg FOV is a few px: floor it, honestly */

        uint8_t inst = pt[i].instance;
        if (inst >= MAX_NINA_INSTANCES) inst = 0;
        s_fov[s_fov_n].x     = (int16_t)cxp;
        s_fov[s_fov_n].y     = (int16_t)cyp;
        s_fov[s_fov_n].r     = (int16_t)r;
        s_fov[s_fov_n].color = page_col(RIG_COL[inst]);
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
                                 (float)DISC_CX, (float)DISC_CY, (float)s_disc_r, &fx, &fy);
            }
        } else {
            on_disc = (p->dist_nm <= range);
            if (on_disc) {
                adsb_scope_project(p->bearing_deg, p->dist_nm, range, s_up_deg,
                                   (float)DISC_CX, (float)DISC_CY, (float)s_disc_r, &fx, &fy);
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
    char dist[16];
    fmt_dist(dist, sizeof(dist), a->dist_nm);
    int hdg = (int)((a->track_deg < 0.0f) ? 0.0f : a->track_deg + 0.5f);

    if (!s_lbl_gsub2) {
        /* One line only (round Board): route or ident, then the figures the
         * second line used to carry, ASCII separators. */
        /* 192, not 128: buf is a 128 byte array, so GCC's worst case for the
         * whole format is 168 bytes and -Werror=format-truncation rejects the
         * smaller destination even though the real line is about 70 chars. */
        char one[192];
        snprintf(one, sizeof(one), "%s  %s  %d ft  %03d", buf, dist,
                 alt_hundreds(a) * 100, hdg);
        set_lbl(s_lbl_gsub, one);
        return;
    }

    set_lbl(s_lbl_gsub, buf);
    snprintf(buf, sizeof(buf), "%s  /  %03d %c  /  hdg %03d  /  %d kt",
             dist, alt_hundreds(a), vrate_char(a->vrate_fpm), hdg,
             (int)(a->gs_kt + 0.5f));
    set_lbl(s_lbl_gsub2, buf);
}

/** The nine-field detail card under the rows. Values only — the keys and every
 *  position are set once at create. */
static void fill_card(const adsb_ac_t *a)
{
    /* The round Board has no detail card (inscribed board 7); one guard here
     * covers every write below. */
    if (!s_card) return;

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
                                lv_color_hex(a->emergency ? page_col(COL_EMERG) : s_col_ink), 0);

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
        set_lbl(s_lbl_gkk, "TRAFFIC");
        set_col(s_lbl_gkk, s_col_dim);
        set_lbl(s_lbl_glance, "CLEAR SKY");
        set_col(s_lbl_glance, s_col_ink);
        snprintf(buf, sizeof(buf), "Nothing within %d nm", (int)(range + 0.5f));
        set_lbl(s_lbl_gsub, buf);
        set_lbl(s_lbl_gsub2, "");
        for (int i = 0; i < 5; i++) {
            show_obj(s_hdr_col[i], false);
        }
        for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
            show_obj(s_row_panel[i], false);
        }
        show_obj(s_card, false);
        /* An empty sky must not leave a stale rail scale on screen. */
        show_obj(s_lbl_legend, false);
        return;
    }

    set_lbl(s_lbl_gkk, lead->emergency ? "EMERGENCY" : "NEAREST");
    set_col(s_lbl_gkk, page_col(lead->emergency ? COL_EMERG : COL_THREAT));
    set_lbl(s_lbl_glance, call_of(lead));
    set_col(s_lbl_glance, s_col_ink);
    fill_lead_lines(lead);
    fill_card(lead);

    for (int i = 0; i < 5; i++) {
        show_obj(s_hdr_col[i], true);
    }

    /* Rows are ranks 1..5 — the lead already has the whole block above. */
    for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
        /* Round shows the lead as row 0 and marks it with the lead colours (the
         * amber eyebrow is not built there). Square keeps ranks 1..5, because
         * its lead already owns the whole block above the rows. */
        const adsb_ac_t *a = by_rank(s_row_dot[0] ? i : (i + 1));
        if (!a) {
            show_obj(s_row_panel[i], false);
            continue;
        }

        set_lbl(s_row_call[i], call_of(a));
        set_col(s_row_call[i], a->emergency ? page_col(COL_EMERG) : s_col_ink);

        set_lbl(s_row_route[i], row_route_of(a));

        /* Zero padded: LVGL has no tabular figures, so a variable-width
         * altitude column jitters on every poll. */
        snprintf(buf, sizeof(buf), "%03d%c", alt_hundreds(a), vrate_char(a->vrate_fpm));
        set_lbl(s_row_alt[i], buf);
        set_col(s_row_alt[i], page_col(alt_color(a)));

        snprintf(buf, sizeof(buf), "%03d",
                 (int)((a->track_deg < 0.0f) ? 0.0f : a->track_deg + 0.5f));
        set_lbl(s_row_hdg[i], buf);

        fmt_dist(buf, sizeof(buf), a->dist_nm);
        set_lbl(s_row_dist[i], buf);

        /* Round Board: the four dropped columns are one rail. The dot's
         * position is distance over range and its colour is the same
         * ADSB_RAMP bucket the square used for the altitude text, which is the
         * only place altitude survives on this page. */
        if (s_row_dot[i] && s_row_rail[i]) {
            float f = (range > 1.0f) ? (a->dist_nm / range) : 0.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            int rail_w = lv_obj_get_style_width(s_row_rail[i], LV_PART_MAIN);
            int rail_x = lv_obj_get_style_x(s_row_rail[i], LV_PART_MAIN);
            int rail_y = lv_obj_get_style_y(s_row_rail[i], LV_PART_MAIN);
            int dot_d  = lv_obj_get_style_width(s_row_dot[i], LV_PART_MAIN);
            int dx = rail_x + (int)(f * (float)rail_w);
            int dy = rail_y + 1;
            lv_obj_set_pos(s_row_dot[i], dx - dot_d / 2, dy - dot_d / 2);
            lv_obj_set_style_bg_color(s_row_dot[i],
                                      lv_color_hex(a->emergency ? page_col(COL_EMERG)
                                                                : page_col(alt_color(a))),
                                      0);
            s_row_dot_cx[i] = (int16_t)(lv_obj_get_style_x(s_row_panel[i], LV_PART_MAIN) + dx);
            s_row_dot_cy[i] = (int16_t)(lv_obj_get_style_y(s_row_panel[i], LV_PART_MAIN) + dy);
        }

        if (s_row_panel[i]) {
            lv_obj_set_style_opa(s_row_panel[i],
                                 (a->seen_pos_s > STALE_DIM_S) ? LV_OPA_40 : LV_OPA_COVER, 0);
        }
        show_obj(s_row_panel[i], true);
    }

    /* One legend at the rail's far end: the same range for all five dots. */
    if (s_lbl_legend) {
        char leg[16];
        snprintf(leg, sizeof(leg), "%d NM", (int)(range + 0.5f));
        set_lbl(s_lbl_legend, leg);
        show_obj(s_lbl_legend, true);
    }
}

/**
 * Radar Scope corner blocks. Every number goes into its own fixed-width
 * right-aligned label (mk_num), so nothing on screen shifts when a digit
 * count changes. The closest aircraft is by dist_nm over has_pos contacts,
 * deliberately NOT by rank: rank is the observing-aid order (emergency,
 * separation from the mount), the corner is a plain "nearest traffic" read.
 */
static void fill_scope_corners(int within, int tracked, float range, page_conn_t st)
{
    char buf[32];

    if (tracked < 0)   tracked = 0;
    if (tracked > 999) tracked = 999;
    if (s_scope_contacts_ring) {
        int v = (tracked > 0) ? (within * 1000 / tracked) : 0;
        if (v > 1000) v = 1000;
        lv_arc_set_value(s_scope_contacts_ring, v);
    }
    if (within > 99) within = 99;
    snprintf(buf, sizeof(buf), "%d / %d", within, tracked);
    set_lbl(s_sc_within, buf);
#if LV_USE_ARCLABEL
    if (s_scope_contacts_arclabel) {
        /* Round Scope: caption and count share one rim run. Shadowed because
         * lv_arclabel_set_text() re-lays out every glyph on each call and this
         * runs per poll. */
        static char cnt_shadow[48];
        char cnt[48];
        snprintf(cnt, sizeof(cnt), "CONTACTS %d / %d", within, tracked);
        if (strcmp(cnt_shadow, cnt) != 0) {
            snprintf(cnt_shadow, sizeof(cnt_shadow), "%s", cnt);
            lv_arclabel_set_text(s_scope_contacts_arclabel, cnt_shadow);
        }
    }
#endif

    int rng = (int)(range + 0.5f);
    if (rng > 999) rng = 999;
    snprintf(buf, sizeof(buf), "%3d NM", rng);
    set_lbl(s_sc_range, buf);

    /* msg_rate is float msg/s from the client; 0 until two polls are in. */
    int rate = (int)(s_snap->msg_rate + 0.5f);
    if (rate < 0)     rate = 0;
    if (rate > 99999) rate = 99999;
    snprintf(buf, sizeof(buf), "%5d msg/s", rate);
    set_lbl(s_sc_rate, buf);
#if LV_USE_ARCLABEL
    if (s_scope_rate_arclabel) {
        /* Round Scope: the rate rides the bottom rim, unpadded (an arc run has
         * no right edge to hold still), shadowed like the count above. */
        static char rate_shadow[48];
        char rt[48];
        snprintf(rt, sizeof(rt), "%d msg/s", rate);
        if (strcmp(rate_shadow, rt) != 0) {
            snprintf(rate_shadow, sizeof(rate_shadow), "%s", rt);
            lv_arclabel_set_text(s_scope_rate_arclabel, rate_shadow);
        }
    }
#endif
    set_lbl(s_sc_cue, (st == PAGE_CONN_STALE) ? "Reconnecting..." : "");

    const adsb_ac_t *nearest = NULL;
    for (int i = 0; i < s_snap->count; i++) {
        const adsb_ac_t *a = &s_snap->ac[i];
        if (!a->has_pos) continue;
        if (!nearest || a->dist_nm < nearest->dist_nm) nearest = a;
    }

    lv_obj_t *bl[] = { s_sc_call, s_sc_ident, s_sc_alt, s_sc_dist };
    for (size_t i = 0; i < sizeof(bl) / sizeof(bl[0]); i++) {
        show_obj(bl[i], nearest != NULL);
    }
    if (!nearest) return;

    set_lbl(s_sc_call, call_of(nearest));

    if (nearest->type[0] && nearest->reg[0]) {
        snprintf(buf, sizeof(buf), "%s %s", nearest->type, nearest->reg);
    } else {
        snprintf(buf, sizeof(buf), "%s", nearest->type[0] ? nearest->type : nearest->reg);
    }
    set_lbl(s_sc_ident, buf);

    int alt = (int)(nearest->alt_ft + 0.5f);
    if (alt < 0)     alt = 0;
    if (alt > 99999) alt = 99999;
    int gs = (int)(nearest->gs_kt + 0.5f);
    if (gs < 0)   gs = 0;
    if (gs > 999) gs = 999;
    int trk = (nearest->track_deg < 0.0f) ? 0 : (int)(nearest->track_deg + 0.5f);
    if (trk > 359) trk = 359;
    float d = nearest->dist_nm;
    if (d < 0.0f)   d = 0.0f;
    if (d > 999.9f) d = 999.9f;

    char line[64];
    if (s_sc_dist) {
        snprintf(line, sizeof(line), "%d ft  %d kt  %03d\xc2\xb0", alt, gs, trk);
        set_lbl(s_sc_alt, line);
        snprintf(buf, sizeof(buf), "%.1f NM", (double)d);
        set_lbl(s_sc_dist, buf);
    } else {
        /* One merged figures line: two lines of it run past the bezel on the
         * lower-left chord, so the track drops and the distance joins. */
        snprintf(line, sizeof(line), "%d ft  %d kt  %.1f NM", alt, gs, (double)d);
        set_lbl(s_sc_alt, line);
    }
}

/** Silhouette class from the readsb emitter category: A7 rotorcraft, A1/A2
 *  light/small fixed wing, everything else (including "" and the B and C
 *  non-aircraft code groups) the airliner outline. */
static uint8_t shape_of_cat(const char *cat)
{
    if (cat[0] == 'A') {
        if (cat[1] == '7') return PLANE_HELI;
        if (cat[1] == '1' || cat[1] == '2') return PLANE_SMALL;
    }
    return PLANE_JET;
}

/**
 * Round Board: one heading arrow per drawn row, just outside that row's
 * distance dot. Drawn through the same disc_draw_cb() as the disc modes, which
 * skips the rings in Board mode. Square leaves board_marks false and this
 * function returns immediately.
 *
 * Runs after fill_board(), which is what fills s_row_dot_cx / s_row_dot_cy.
 *
 * s_up_deg is deliberately NOT applied: the Board carries no bearing and its
 * 2026-08-18 retarget says the arrow is a true heading, not a relative one.
 */
static void fill_board_marks(void)
{
    if (!s_geom.board_marks) {
        return;
    }
    for (int i = 0; i < ADSB_BOARD_ROWS && s_mark_n < ADSB_MAX_AC; i++) {
        const adsb_ac_t *a = by_rank(s_row_dot[0] ? i : (i + 1));
        if (!a || !s_row_panel[i] || !s_row_dot[i]) continue;
        if (lv_obj_has_flag(s_row_panel[i], LV_OBJ_FLAG_HIDDEN)) continue;

        /* Dot centre plus 22, with a 10 px nose: the row panel's right edge is
         * 559 on the narrowest row and the rail ends at 528, so +30 with a
         * 14 px nose ran past it. */
        int px = s_row_dot_cx[i] + 22;
        int py = s_row_dot_cy[i];

        adsb_mark_t *m = &s_mark[s_mark_n];
        memset(m, 0, sizeof(*m));
        m->x = (int16_t)px;
        m->y = (int16_t)py;
        m->color = page_col(a->emergency ? COL_EMERG : alt_color(a));
        /* Dim with the row panel this arrow belongs to (review D5 M-1). */
        m->opa = (a->seen_pos_s > STALE_DIM_S) ? LV_OPA_40 : LV_OPA_COVER;

        float hdg = (a->track_deg < 0.0f) ? 0.0f : a->track_deg;
        float t = hdg * ADSB_DEG2RAD;
        float st_ = sinf(t), ct = cosf(t);
        static const float nose[3][2] = { { 0.0f, -10.0f }, { 6.0f, 7.0f },
                                          { -6.0f, 7.0f } };
        for (int k = 0; k < 3; k++) {
            float rx = nose[k][0] * ct - nose[k][1] * st_;
            float ry = nose[k][0] * st_ + nose[k][1] * ct;
            m->tx[k] = (int16_t)(px + (int)rx);
            m->ty[k] = (int16_t)(py + (int)ry);
        }
        m->flags |= MK_TRI;
        s_mark_n++;
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
        /* Round's chord caps cannot fit the square sentence (review_impl_D3
         * I-1): drop the "MOUNT " word, the disc mode is already obvious. */
        snprintf(buf, sizeof(buf), s_geom.short_caps ? "AZ %03d EL %02d"
                                                      : "MOUNT AZ %03d EL %02d",
                 (int)(adsb_wrap360(pt[0].az_deg) + 0.5f), (int)(pt[0].alt_deg + 0.5f));
    } else {
        buf[0] = '\0';
    }
    set_lbl(s_lbl_mount, buf);

    /* Connection tiers, shared with every other data page. */
    page_conn_t st = s_have
        ? page_conn_eval(s_snap->ever_ok, s_snap->fail_count == 0, s_snap->fail_count)
        : PAGE_CONN_CONNECTING;

    if (st == PAGE_CONN_CONNECTING || st == PAGE_CONN_DOWN) {
        /* recompute() runs on every poll update, so the wait wording is
         * re-judged each cycle and gives way on its own once the link is up. */
        nina_empty_state_set_title(s_empty, st == PAGE_CONN_CONNECTING
                                   ? nina_empty_state_wait_title("Connecting to ADS-B receiver...")
                                   : "Cannot reach ADS-B receiver");
        nina_empty_state_set_busy(s_empty, st == PAGE_CONN_CONNECTING);
        show_obj(s_backdrop, true);
        nina_empty_state_show(s_empty);
        set_lbl(s_lbl_strip, st == PAGE_CONN_CONNECTING
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
        if (s_geom.short_caps) {
            /* Round chord width: no elevation gate, no STALE suffix. The
             * content layer already dims on STALE (review_impl_D3 I-1). */
            snprintf(buf, sizeof(buf), "%d / %d seen", above, tracked);
        } else {
            snprintf(buf, sizeof(buf), "%d above %d\xc2\xb0 / %d tracked   %s",
                     above, (int)gate, tracked,
                     (st == PAGE_CONN_STALE) ? "Reconnecting..." : "");
        }
    } else {
        snprintf(buf, sizeof(buf), "%d within %d nm / %d tracked   %s",
                 within, (int)(range + 0.5f), tracked,
                 (st == PAGE_CONN_STALE) ? "Reconnecting..." : "");
    }
    set_lbl(s_lbl_strip, buf);

    if (s_mode == MODE_BOARD) {
        s_mark_n = 0;
        s_lead_n = 0;
        s_trun_n = 0;
        s_slbl_n = 0;
        /* The FOV circles are a Sky Dome aid. They were harmless here while the
         * draw host was hidden in Board mode; on round the host stays visible,
         * so a stale ring from the last Sky visit would be drawn over the rows. */
        s_fov_n = 0;
        fill_board(range);
        fill_board_marks();
        /* The disc-mode path invalidates at the end of the function; this one
         * returns before it, and the arrows live on the host. */
        lv_obj_invalidate(s_disc);
        return;
    }

    /* ── Disc modes ── */
    place_compass();
    place_rings(gate, range);
    /* Mount pointing circles are the Sky Dome's business; the Scope is a plain
     * distance picture with no telescope input. */
    if (s_mode == MODE_SKY) place_fov(pt, np, gate);
    else                    s_fov_n = 0;
    s_show_rx = (s_mode == MODE_SCOPE);
    if (s_mode == MODE_SCOPE) {
        fill_scope_corners(within, tracked, range, st);
    }

    s_mark_n     = 0;
    s_lead_n     = 0;
    s_trun_n     = 0;
    s_tag_area_n = 0;
    s_slbl_n     = 0;
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        show_obj(s_tag_box[i], false);
    }

    /* Tags are queued here and placed AFTER the mark loop: the declutter score
     * counts glyphs, so it needs every positioned contact, not just the ones
     * walked so far. l1 points into s_snap, which outlives this function.
     * Sky: three boxed tags via pend[]. Scope: every drawn contact goes into
     * s_slbl[] and place_scope_labels() picks the first flights_label_max. */
    struct { int slot, x, y, mark; const char *l1; char l2[32]; } pend[ADSB_TAG_COUNT] = {{ 0 }};
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
                             (float)DISC_CX, (float)DISC_CY, (float)s_disc_r, &fx, &fy);
        } else {
            if (a->dist_nm > range) continue;
            adsb_scope_project(a->bearing_deg, a->dist_nm, range, s_up_deg,
                               (float)DISC_CX, (float)DISC_CY, (float)s_disc_r, &fx, &fy);
        }
        int x = (int)(fx + 0.5f);
        int y = (int)(fy + 0.5f);
        if (x < 4 || x > screen_size() - 4 || y < 4 || y > screen_size() - 4) {
            continue;
        }

        int midx = s_mark_n;
        adsb_mark_t *m = &s_mark[s_mark_n];
        memset(m, 0, sizeof(*m));
        m->x     = (int16_t)x;
        m->y     = (int16_t)y;
        m->color = page_col(alt_color(a));
        m->opa   = (a->seen_pos_s > STALE_DIM_S) ? LV_OPA_50 : LV_OPA_COVER;
        if (a->db_flags & 0x01) m->flags |= MK_SQUARE;
        if (a->emergency)       m->flags |= MK_EMERG;

        /* Where it came from, in the glyph's own colour. */
        build_trail(i, m->color, gate, range);

        if (s_mode == MODE_SCOPE) {
            /* Nose along (track - up), for both icon styles. */
            float hdg = (a->track_deg < 0.0f) ? 0.0f : a->track_deg;
            float t = (hdg - s_up_deg) * ADSB_DEG2RAD;
            float st_ = sinf(t), ct = cosf(t);
            if (cfg->flights_icon_style == 1 && !(m->flags & MK_SQUARE)) {
                /* Aircraft silhouette: store the rotation, draw_plane spins
                 * the shape table in the draw callback. Military squares and
                 * the emergency halo keep the style-0 path below. */
                m->st    = st_;
                m->ct    = ct;
                m->shape = shape_of_cat(a->cat);
                m->flags |= MK_PLANE;
            } else {
                /* Heading-rotated triangle. */
                static const float nose[3][2] = { { 0.0f, -18.0f }, { 10.0f, 12.0f }, { -10.0f, 12.0f } };
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
            }
        } else if (a->el_deg >= 20.0f) {
            m->flags |= MK_FILLED;
        }
        s_mark_n++;

        if (s_mode == MODE_SCOPE) {
            /* Every drawn contact is a label candidate; the count cap is
             * applied nearest-first (rank) by place_scope_labels(). Line 2 is
             * FL / climb cue / ground speed: "%3d %c %3d". Written straight
             * into the slot (cppcheck: no alias before first assignment). */
            if (s_slbl && s_slbl_n < ADSB_MAX_AC) {
                int gs = (int)(a->gs_kt + 0.5f);
                if (gs < 0)   gs = 0;
                if (gs > 999) gs = 999;
                snprintf(s_slbl[s_slbl_n].l1, sizeof(s_slbl[s_slbl_n].l1), "%s", call_of(a));
                snprintf(s_slbl[s_slbl_n].l2, sizeof(s_slbl[s_slbl_n].l2), "%3d %c %3d",
                         alt_hundreds(a), scope_vc(a->vrate_fpm), gs);
                s_slbl[s_slbl_n].gx     = (int16_t)x;
                s_slbl[s_slbl_n].gy     = (int16_t)y;
                s_slbl[s_slbl_n].x      = (int16_t)x;
                s_slbl[s_slbl_n].y      = (int16_t)y;
                s_slbl[s_slbl_n].mark   = (int16_t)midx;
                s_slbl[s_slbl_n].rank   = a->rank;
                s_slbl[s_slbl_n].opa    = m->opa;
                s_slbl[s_slbl_n].color  = m->color;
                s_slbl[s_slbl_n].placed = false;
                s_slbl_n++;
            }
        } else if (a->rank >= 0 && a->rank < ADSB_TAG_COUNT && tags_done < ADSB_TAG_COUNT) {
            /* Sky: three boxed tags. Written straight into the slot: cppcheck
             * flags an alias pointer taken before the array element is first
             * assigned. */
            snprintf(pend[pend_n].l2, sizeof(pend[pend_n].l2), "%03d%c  %02d\xc2\xb0",
                     alt_hundreds(a), vrate_char(a->vrate_fpm), (int)(a->el_deg + 0.5f));
            pend[pend_n].slot = a->rank;
            pend[pend_n].x    = x;
            pend[pend_n].y    = y;
            pend[pend_n].mark = midx;
            pend[pend_n].l1   = call_of(a);
            pend_n++;
            tags_done++;
        }
    }

    if (s_mode == MODE_SCOPE) {
        /* 0 = none, 64 (ADSB_MAX_AC) = all drawn, else the first N by rank. */
        int lmax = cfg->flights_label_max;
        if (lmax > ADSB_MAX_AC) lmax = ADSB_MAX_AC;
        place_scope_labels(lmax);
    } else {
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
    map_refresh(false);   /* picks up a freshly fetched frame, if any */
}

void nina_adsb_config_changed(void)
{
    if (!s_root) return;
    const app_config_t *cfg = app_config_get();
    s_mode   = (cfg->flights_mode > MODE_BOARD) ? MODE_SKY : cfg->flights_mode;
    s_up_deg = adsb_wrap360((float)cfg->flights_up_azimuth);
    /* apply_mode() re-renders the map with the new up azimuth; a range change
     * makes the poller refetch and the next nina_adsb_update() picks the new
     * generation up on its own. */
    apply_mode();
    recompute();
}

uint8_t nina_adsb_get_mode(void)
{
    return s_mode;
}

/* ── Theme ────────────────────────────────────────────────────────────── */

/** Disc furniture colours are mode dependent: Sky keeps the fixed greys,
 *  Scope is phosphor green. Called from apply_colors() and apply_mode(). */
static void apply_disc_colors(void)
{
    bool scope = (s_mode == MODE_SCOPE);
    s_col_line    = page_col(scope ? COL_SCOPE_GREEN   : COL_RING_OUT);
    s_col_ring_in = page_col(scope ? COL_SCOPE_RING_IN : COL_RING_IN);
    uint32_t card = scope ? page_col(COL_SCOPE_GREEN) : s_col_ink;
    uint32_t rlbl = page_col(scope ? COL_SCOPE_RING_LBL : COL_RING_LBL);
    for (int i = 0; i < 4; i++) {
        set_col(s_lbl_card[i], card);
    }
    for (int i = 0; i < 3; i++) {
        set_col(s_lbl_ring[i], rlbl);
    }
    map_refresh(true);   /* Red Night / colour-brightness remap is baked into
                           * the rendered buffer, not applied at draw time */
}

static void apply_colors(void)
{
    /* Rings use the fixed scope greys, not bento_border: see COL_RING_OUT. */
    s_col_dim     = col_label();
    s_col_ink     = col_text();
    s_col_emerg   = page_col(COL_EMERG);
    apply_disc_colors();

    /* Scope corner blocks. */
    {
        uint32_t cap = page_col(COL_SCOPE_CAP);
        uint32_t grn = page_col(COL_SCOPE_GREEN);
        set_col(s_sc_cap_contacts, cap);
        set_col(s_sc_cap_range,    cap);
        set_col(s_sc_within, s_col_ink);
        set_col(s_sc_range,  s_col_ink);
        set_col(s_sc_call,   s_col_ink);
        lv_obj_t *greens[] = { s_sc_ident, s_sc_alt, s_sc_dist, s_sc_rate, s_sc_cue };
        for (size_t i = 0; i < sizeof(greens) / sizeof(greens[0]); i++) {
            set_col(greens[i], grn);
        }
    }

    lv_obj_set_style_bg_color(s_root, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_bg_color(s_backdrop, lv_color_hex(col_bg()), 0);
    if (s_hdr)   lv_obj_set_style_bg_color(s_hdr, lv_color_hex(col_bg()), 0);
    if (s_strip) lv_obj_set_style_bg_color(s_strip, lv_color_hex(col_bg()), 0);

    set_col(s_lbl_title, s_col_ink);
    set_col(s_lbl_mount, s_col_dim);
    set_col(s_lbl_strip, s_col_dim);
    set_col(s_lbl_glance, s_col_ink);
    set_col(s_lbl_gsub,   page_col(COL_SUB));
    set_col(s_lbl_gsub2,  page_col(COL_MUTED));
    for (int i = 0; i < 5; i++) {
        set_col(s_hdr_col[i], page_col(COL_MUTED_DIM));
    }
    for (int i = 0; i < ADSB_TAG_COUNT; i++) {
        if (s_tag_box[i]) {
            lv_obj_set_style_bg_color(s_tag_box[i], lv_color_hex(col_bg()), 0);
            lv_obj_set_style_border_color(s_tag_box[i], lv_color_hex(page_col(COL_RING_IN)), 0);
        }
        set_col(s_tag_l1[i], s_col_ink);
        set_col(s_tag_l2[i], page_col(COL_MUTED));
    }
    for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
        if (s_row_panel[i]) {
            /* Round shows the lead as row 0 and has no amber eyebrow, so the
             * lead colours on that row are the only lead marker left. Square
             * leaves s_row_dot[0] NULL and every row keeps the plain pair. */
            bool lead_row = (s_row_dot[0] != NULL) && (i == 0);
            lv_obj_set_style_bg_color(s_row_panel[i],
                                      lv_color_hex(page_col(lead_row ? COL_LEAD_BG
                                                                     : COL_ROW_BG)), 0);
            lv_obj_set_style_border_color(s_row_panel[i],
                                          lv_color_hex(page_col(lead_row ? COL_LEAD_BRD
                                                                         : COL_ROW_BRD)), 0);
        }
        set_col(s_row_call[i], s_col_ink);
        set_col(s_row_route[i], page_col(COL_MUTED));
        set_col(s_row_hdg[i], page_col(COL_MUTED));
        set_col(s_row_dist[i], page_col(COL_MUTED));
    }
    if (s_card) {
        lv_obj_set_style_bg_color(s_card, lv_color_hex(page_col(COL_ROW_BG)), 0);
        lv_obj_set_style_border_color(s_card, lv_color_hex(page_col(COL_ROW_BRD)), 0);
    }
    set_col(s_card_title, s_col_ink);
    if (s_card_mil) {
        lv_obj_set_style_bg_color(s_card_mil, lv_color_hex(page_col(COL_THREAT)), 0);
    }
    set_col(s_card_mil, page_col(COL_LEAD_BG));
    for (int i = 0; i < CARD_FIELDS; i++) {
        set_col(s_card_key[i], page_col(COL_MUTED_DIM));
        set_col(s_card_val[i], s_col_ink);
    }

    /* Round-family widgets. All NULL on square. */
    set_col(s_lbl_legend, page_col(COL_MUTED_DIM));
    if (s_scope_contacts_ring) {
        lv_obj_set_style_arc_color(s_scope_contacts_ring,
                                   lv_color_hex(page_col(COL_SCOPE_RING_IN)),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_scope_contacts_ring,
                                   lv_color_hex(page_col(COL_SCOPE_GREEN)),
                                   LV_PART_INDICATOR);
    }
    set_col(s_scope_contacts_arclabel, page_col(COL_SCOPE_CAP));
    set_col(s_scope_rate_arclabel, page_col(COL_SCOPE_GREEN));
    for (int i = 0; i < ADSB_BOARD_ROWS; i++) {
        if (s_row_rail[i]) {
            lv_obj_set_style_bg_color(s_row_rail[i],
                                      lv_color_hex(page_col(COL_ROW_BRD)), 0);
        }
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

#if !CONFIG_NINA_FAMILY_ROUND
/** Translucent scrim strip over the disc (header / status). */
static lv_obj_t *mk_scrim(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, screen_size(), h);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
#endif

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
    /* Not fatal either: the Scope draws arrows without labels. */
    if (!s_slbl) {
        s_slbl = heap_caps_calloc(ADSB_MAX_AC, sizeof(adsb_slbl_t), MALLOC_CAP_SPIRAM);
        if (!s_slbl) ESP_LOGW(TAG, "no PSRAM for scope labels; drawing arrows only");
    }

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, screen_size(), screen_size());
    /* Full bleed, so the disc really reaches the edge. Centring rather than
     * negating a literal pad: on square a 720 root centred in main_cont's 688
     * content box lands at (-16,-16), exactly what
     * lv_obj_set_pos(-OUTER_PADDING, -OUTER_PADDING) produced, and it stays
     * centred whatever pad main_cont carries (same fix as nina_clock.c). */
    lv_obj_center(s_root);
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
    lv_obj_set_size(s_content, screen_size(), screen_size());
    lv_obj_set_pos(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Radar Scope basemap: the FIRST child of s_content, so it sits under
     * everything else either family builds below (draw host, Board rows, tag
     * boxes). Hidden until map_refresh() has a frame to show. */
    s_map_img = lv_image_create(s_content);
    lv_obj_set_pos(s_map_img, 0, 0);
    lv_obj_set_size(s_map_img, screen_size(), screen_size());
    lv_obj_clear_flag(s_map_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    show_obj(s_map_img, false);

#if CONFIG_NINA_FAMILY_ROUND
    {
        const adsb_slots_t slots = {
            .disc = &s_disc,
            .lbl_card = s_lbl_card, .lbl_ring = s_lbl_ring,
            .tag_box = s_tag_box, .tag_l1 = s_tag_l1, .tag_l2 = s_tag_l2,
            .hdr = &s_hdr, .lbl_title = &s_lbl_title, .lbl_mount = &s_lbl_mount,
            .strip = &s_strip, .lbl_strip = &s_lbl_strip,
            .sc_within = &s_sc_within, .sc_call = &s_sc_call,
            .sc_alt = &s_sc_alt, .sc_dist = &s_sc_dist,
            .sc_rate = &s_sc_rate, .sc_cue = &s_sc_cue,
            .sc_contacts_ring = &s_scope_contacts_ring,
            .sc_contacts_arclabel = &s_scope_contacts_arclabel,
            .sc_rate_arclabel = &s_scope_rate_arclabel,
            .board = &s_board, .lbl_glance = &s_lbl_glance, .lbl_gsub = &s_lbl_gsub,
            .row_panel = s_row_panel, .row_call = s_row_call,
            .row_dot = s_row_dot, .row_rail = s_row_rail,
            .lbl_legend = &s_lbl_legend,
        };
        adsb_round_build(s_root, s_content, &slots, &s_geom);
    }
#else
    /* Square geometry, exactly the values the construction below hard-codes.
     * Assigned BEFORE the widgets because TAG_H now reads s_geom.tag_h and the
     * tag boxes are sized with it. */
    s_geom = (adsb_geom_t){
        .card_off_v = { 24, 24 },
        .card_off_h = { 24, 24 },
        .card_off_diag = 24,
        .rim_w      = { 2, 2 },
        .ring_inset = ADSB_RING_INSET_INNER,
        .ring_lbl_w = 84,
        .tag_h      = 60,
        .tag_font1  = &lv_font_montserrat_24,
        .tag_font2  = &lv_font_montserrat_22,
        .tag_l1_y   = 2,
        .tag_l2_y   = 31,
        .scrim_top  = { HDR_H, HDR_H },
        .scrim_bot  = { STRIP_H, STRIP_H },
    };
    adsb_fill_corner_areas();

    /* Draw host: full screen, transparent, one DRAW_MAIN_END callback. */
    s_disc = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_disc);
    lv_obj_set_size(s_disc, screen_size(), screen_size());
    lv_obj_set_pos(s_disc, 0, 0);
    lv_obj_clear_flag(s_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        s_lbl_card[i] = mk_label(s_content, &lv_font_montserrat_28, 0xFFFFFF, "");
    }
    for (int i = 0; i < 3; i++) {
        s_lbl_ring[i] = mk_label(s_content, &lv_font_montserrat_22, COL_RING_LBL, "");
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
        s_tag_l1[i] = mk_label(s_tag_box[i], &lv_font_montserrat_24, 0xFFFFFF, "");
        lv_obj_set_pos(s_tag_l1[i], 8, 2);
        s_tag_l2[i] = mk_label(s_tag_box[i], &lv_font_montserrat_22, 0x808080, "");
        lv_obj_set_pos(s_tag_l2[i], 8, 31);
        show_obj(s_tag_box[i], false);
    }

    /* Radar Scope corner blocks (shown by apply_mode in Scope only). No
     * backgrounds. Line heights: Montserrat 36 = 40 px, 22 = 24 px, 18 = 21 px.
     * Digit advance at 36 pt is <= 24 px, at 22 pt <= 15 px; the fixed widths
     * below are sized from that. */
    {
        const int right_x = screen_size() - CORNER_PAD;
        /* Top-left: CONTACTS  "NN / NNN" */
        s_sc_cap_contacts = mk_label(s_content, &lv_font_montserrat_18, COL_SCOPE_CAP, "CONTACTS");
        lv_obj_set_pos(s_sc_cap_contacts, CORNER_PAD, 12);
        s_sc_within = mk_label(s_content, &lv_font_montserrat_36, 0xFFFFFF, "");
        lv_obj_set_pos(s_sc_within, CORNER_PAD, 32);
        /* Top-right: MAX RANGE  "NNN NM", both right aligned on x = 700 */
        s_sc_cap_range = mk_num(s_content, &lv_font_montserrat_18, COL_SCOPE_CAP, right_x - 200, 12, 200);
        lv_label_set_text(s_sc_cap_range, "MAX RANGE");
        s_sc_range = mk_num(s_content, &lv_font_montserrat_36, 0xFFFFFF, right_x - 200, 32, 200);
        /* Bottom-left: closest aircraft, four lines ending 4 px above the edge */
        s_sc_call  = mk_label(s_content, &lv_font_montserrat_36, 0xFFFFFF, "");
        lv_obj_set_pos(s_sc_call, CORNER_PAD, 578);
        clip_label(s_sc_call, CORNER_W_L - CORNER_PAD);
        s_sc_ident = mk_label(s_content, &lv_font_montserrat_22, COL_SCOPE_GREEN, "");
        lv_obj_set_pos(s_sc_ident, CORNER_PAD, 620);
        clip_label(s_sc_ident, CORNER_W_L - CORNER_PAD);
        s_sc_alt = mk_label(s_content, &lv_font_montserrat_22, COL_SCOPE_GREEN, "");
        lv_obj_set_pos(s_sc_alt, CORNER_PAD, 646);
        clip_label(s_sc_alt, CORNER_W_L - CORNER_PAD);
        s_sc_dist = mk_label(s_content, &lv_font_montserrat_22, COL_SCOPE_GREEN, "");
        lv_obj_set_pos(s_sc_dist, CORNER_PAD, 672);
        /* Bottom-right: message rate on the same baseline as the BL bottom
         * line; the reconnect cue above it. The cue lives on s_root (added
         * after the scrims below) so the STALE dim never touches it. */
        s_sc_rate = mk_num(s_content, &lv_font_montserrat_22, COL_SCOPE_GREEN, right_x - CORNER_W_R, 672, CORNER_W_R);
    }

    /* Board: lead block, five ranked rows, lead detail card. */
    s_board = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_board);
    lv_obj_set_size(s_board, screen_size(), screen_size());
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
    s_lbl_title = mk_label(s_hdr, &lv_font_montserrat_26, 0xFFFFFF, "ADS-B");
    lv_obj_align(s_lbl_title, LV_ALIGN_LEFT_MID, 20, 0);
    s_lbl_mount = mk_label(s_hdr, &lv_font_montserrat_22, 0x808080, "");
    lv_obj_align(s_lbl_mount, LV_ALIGN_RIGHT_MID, -20, 0);

    s_strip = mk_scrim(s_root, screen_size() - STRIP_H, STRIP_H);
    s_lbl_strip = mk_label(s_strip, &lv_font_montserrat_26, 0x808080, "");
    lv_obj_align(s_lbl_strip, LV_ALIGN_LEFT_MID, 20, 0);

    /* Scope reconnect cue: bottom-right, above the message rate, undimmed. */
    s_sc_cue = mk_num(s_root, &lv_font_montserrat_22, COL_SCOPE_GREEN,
                      screen_size() - CORNER_PAD - CORNER_W_R, 646, CORNER_W_R);
    lv_obj_clear_flag(s_sc_cue, LV_OBJ_FLAG_CLICKABLE);
#endif

    /* Text overlay layer, built LAST (after both family builders), so every
     * text-bearing object created above can be reparented into ONE container.
     * The double-tap toggle then hides all of it with a single HIDDEN flag
     * write instead of walking dozens of objects. This does not fight the
     * many per-slot show_obj() calls elsewhere in the file: LVGL treats an
     * object as hidden if EITHER it or an ancestor carries LV_OBJ_FLAG_HIDDEN,
     * so a child's own flag still works exactly as before, and clearing the
     * layer's flag just uncovers whatever the child flags already say.
     * s_content, s_root and this layer are all full-panel at (0,0), so every
     * position/align coordinate written above against s_content stays valid
     * after the reparent. */
    s_text_layer = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_text_layer);
    lv_obj_set_size(s_text_layer, screen_size(), screen_size());
    lv_obj_set_pos(s_text_layer, 0, 0);
    lv_obj_set_style_bg_opa(s_text_layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_text_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                    LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    {
        /* s_lbl_title/s_lbl_mount and s_lbl_strip are children of s_hdr/
         * s_strip and move with their parent; s_disc, s_board and
         * s_scope_contacts_ring (not text) and s_map_img (the picture, not
         * text) are deliberately left off this list, and so are the three
         * ring numbers (s_lbl_ring[]) and the four cardinals (s_lbl_card[]):
         * the user wants the range markers and N E S W to stay readable with
         * the rest of the text hidden, so they remain direct children of
         * s_content above the draw host. */
        lv_obj_t *text_objs[] = {
            s_hdr, s_strip,
            s_sc_cap_contacts, s_sc_within, s_sc_cap_range, s_sc_range,
            s_sc_call, s_sc_ident, s_sc_alt, s_sc_dist, s_sc_rate, s_sc_cue,
            s_scope_contacts_arclabel, s_scope_rate_arclabel,
        };
        for (size_t i = 0; i < sizeof(text_objs) / sizeof(text_objs[0]); i++) {
            if (text_objs[i]) {
                lv_obj_set_parent(text_objs[i], s_text_layer);
            }
        }
        for (int i = 0; i < ADSB_TAG_COUNT; i++) {
            if (s_tag_box[i]) {
                lv_obj_set_parent(s_tag_box[i], s_text_layer);
            }
        }
    }

    /* One owner for the draw callback, whichever family built the host. */
    lv_obj_add_event_cb(s_disc, disc_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    /* Empty state needs its own opaque backdrop (nina_empty_state is 80%
     * inline by contract; full-coverage consumers supply the ground). */
    s_backdrop = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, screen_size(), screen_size());
    lv_obj_set_pos(s_backdrop, 0, 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    show_obj(s_backdrop, false);
    s_empty = nina_empty_state_create(s_backdrop, ICON_CLOUD_OFF,
                                      nina_empty_state_wait_title("Connecting to ADS-B receiver..."),
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
    /* Drop the map before its buffer goes: this runs under the LVGL lock on
     * the UI side, so nothing is mid-flush on the pixels being freed. The
     * source frame is freed separately by the poller's park hook. */
    if (s_map_img) {
        lv_image_set_src(s_map_img, NULL);
        show_obj(s_map_img, false);
    }
    s_map_dsc.data = NULL;
    s_map_gen = 0;
    adsb_basemap_release_display();
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
