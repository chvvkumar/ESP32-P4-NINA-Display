/**
 * @file nina_sky_scene.c
 * @brief The sky scene renderer shared by NINA page layouts 8 and 9.
 *
 * Renderer choice (plan D1): ONE full-panel LVGL object whose
 * LV_EVENT_DRAW_MAIN_END callback issues lv_draw_rect / lv_draw_line /
 * lv_draw_arc / lv_draw_triangle, the ADS-B Sky Dome pattern (nina_adsb.c).
 * All trigonometry runs in nina_sky_scene_update() and in the animation tick,
 * which build a fixed-size draw list of integer panel coordinates; the draw
 * callback reads only that list, the palette and its own bake caches (the
 * sky strips and the Moon glow picture, re-baked on a key change only).
 *
 * ponytail: one LVGL object redrawn from draw-event primitives; ceiling is
 * the frame rate during the 2.4 s fly-around (every tick repaints the full
 * panel). Upgrade path: render into an own RGB565 PSRAM buffer and show it as
 * an lv_image.
 *
 * Invalidate discipline (the panel is full refresh): the draw list is hashed
 * (FNV-1a 32) after every re-projection and the object is invalidated only
 * when the hash changed. The 33 ms timer runs only while anim_active() says
 * so: the camera flies, the displayed aim is still settling, the mount
 * slews or flips, the tube eases through the pole after a change of pier
 * side, or the slew trail or the sweep arc is still fading.
 *
 * Telescope: the solid model of sky_rig_model.h, cut into flat primitives by
 * sky_rig_tessellate() during project() (scratch in the scene block) and
 * replayed by the draw pass through the mapped tone table rig_tone[].
 *
 * Render probe (debug_mode only): while the timer runs, the display's render
 * start and ready events are timed and one log line is written when the timer
 * pauses. It times the whole display render, which on these pages is the scene.
 *
 * Locks: every entry point runs with the LVGL lock held by the caller.
 * nina_sky_scene_update() also runs inside the caller's nina_client lock and is
 * the ONLY function that reads nina_client_t; the timer callback works on the
 * scene's own copied state.
 */

#include "nina_sky_scene.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "core/lv_obj_event_private.h"   /* lv_cover_check_info_t */
#include "misc/lv_area_private.h"        /* lv_area_is_in */

#include "app_config.h"
#include "display_defs.h"
#include "moon_ephemeris.h"
#include "sky_night_math.h"
#include "sky_rig_draw.h"
#include "sky_rig_model.h"
#include "sky_stars.h"
#include "themes.h"
#include "ui_helpers.h"

static const char *TAG = "sky_scene";

/* ---- sizes and fixed style -------------------------------------------------- */

#define SCN_BEADS_MAX   256
#define SCN_SEG_MAX     76     /* one arc of the path: at most 74 points */
#define SCN_HZ_PTS      181    /* smooth horizon outline: yaw -180..+180 every 2 degrees
                                * (a camera pitched up with the wide lens sees hills more
                                * than 90 degrees off its yaw; points behind it fail to project) */
#define SCN_HZ_STEP_DEG 2.0f
#define SCN_HZ_MAX      182    /* outline edge segments kept (on screen only) */
#define SCN_COMPASS_MAX 24     /* compass labels along the level horizon */
#define SCN_STRIP_MAX   800    /* rows of a dithered gradient strip (largest panel) */
#define SCN_STRIP_W     16     /* px, even: strip width. At 2 px the sky was 360 tiles per row,
                                * about 260k tiny copies a frame; 16 px is 45. Costs
                                * SCN_STRIP_W * 1600 bytes of PSRAM per strip. */
#define SCN_COMPASS_MIN_PX 96.0f /* least centre-to-centre spacing of two labels */
#define SCN_COL_W       6      /* land column width, px */
#define SCN_COL_MAX     136    /* column runs: ceil(800 / SCN_COL_W) = 134 at most */
#define SCN_COL_MERGE   2      /* px: neighbour columns this close share one rectangle */
#define SCN_EDGE_W      7      /* land-coloured line along the outline, px */
#define SCN_BEAM_MAX    6
#define SCN_CLIP_PAD    256.0f /* segments are clipped to the panel grown by this */
#define SCN_TICK_MS     33
#define SCN_AIM_TAU_S   0.35f
#define SCN_AIM_DONE    0.05f  /* degrees: the displayed aim counts as settled */
#define SCN_RING_R      24
#define SCN_ARROW_LEN   26.0f
#define SCN_ARROW_HALF  10.0f
#define SCN_FLIP_HALF   14.0f
#define SCN_GRASS_N     140    /* grass blades at fixed world spots round the pier */
#define SCN_GRASS_R     7.0f   /* metres: blades scattered inside this radius */
#define SCN_GRASS_R0    0.35f  /* metres: none under the pier base */
#define SCN_FLY_LITE    1      /* 1: while the camera flies, drop the costly sky decor, the beam's
                                * soft glow and the telescope's outline and fine round ends */
#define SCN_BODIES_S    60     /* seconds between two Sun and Moon position refreshes */
#define SCN_RMS_HIST    8      /* guiding RMS readings kept for the ring shape */
#define SCN_SKY_LEVELS  24     /* sky strips, one per step of distance from the Sun's column */
#define SCN_FIG_MAX     64     /* star pattern segments kept */
#define SCN_HAZE_H0     36     /* haze above the skyline: three bands ending this many px up */
#define SCN_HAZE_H1     22
#define SCN_HAZE_H2     10
#define SCN_MOON_R      11     /* Moon disc radius, px */
#define SCN_MOON_GLOW_MAX 248  /* Moon glow picture side, px (largest: 244 on the 800 px panel) */
#define SCN_GLOW_DITHER 10.0f  /* alpha dither of the glow picture, about one 5-bit colour step */
#define SCN_FLOOR_GLOW_H 46    /* rows of the floor that carry the haze glow */
#define SCN_LEFT_MAX    SCN_SEG_MAX /* time-left segments: emit_arc() appends up to SCN_SEG_MAX */
#define SCN_RING_N      64     /* segments of the wavy guiding ring */
#define SCN_PULSE_END_US 1700000 /* the finish pulse flag is dropped at this age */
#define SCN_RIG_W_MAX   80     /* widest telescope line and largest dot radius, px */
#define SCN_TRAIL_MAX   20     /* aim directions kept for the slew trail */
#define SCN_TRAIL_MIN_DEG 0.5f /* least spacing of two trail points */
#define SCN_SWEEP_MAX   60     /* line pieces of the sweep arc */
#define SCN_SWEEP_STEP_DEG 6.0f /* one piece spans at most this much of the arc */
#define SCN_SKY_TURN_S  30.0f  /* seconds between two turns of the stars, the patterns, the Sun
                                * and the Moon with sidereal time: 0.125 degrees of sky, at most
                                * 2.7 px (720 panel) or 3.2 px (800) at the tightest zoom */
#define SCN_SKY_JUMP_DEG 0.05f /* a sidereal clock this far from the expected one turns at once */
#define SCN_HAZE_MERGE  8      /* px: haze runs merge more freely than land, the haze is soft */
#define SCN_FLY_SPAN_Q  8      /* px: sky gradient span step while the camera flies */
#define SCN_SWEEP_MAX_DEG 350.0f /* the sweep never closes on itself */
#define SCN_STALE_TICKS 150    /* tick_cb() runs with no update() before motion flags are dropped */
#define SCN_SHADOW_K    3.2f   /* contact shadow half width, in pier widths */
#define SCN_SHADOW_FLAT 0.26f  /* its height over its width */
#define SCN_SHADOW_MAX  400    /* largest shadow half width, px */
#define SCN_RED_NIGHT_LIFT 0.2f /* Red Night: the rig red moves this far toward white, so it
                                 * sits between the black and the steel by brightness */

#define OPA_PCT(p) ((lv_opa_t)((p) * 255 / 100))

/* Palette slots: raw colours, mapped through Red Night and brightness. */
enum {
    PAL_SKY_TOP = 0,
    PAL_SKY_GLOW,      /* lower sky, reached at the deepest dip of the outline */
    PAL_AMBER,
    PAL_FLIP_DARK,
    PAL_FLIP_LIGHT,
    PAL_TARGET_C,
    PAL_LAND,          /* obstructions: trees and roofs against the sky */
    PAL_FLOOR_FAR,     /* yard floor at the level horizon */
    PAL_FLOOR_NEAR,    /* yard floor at the panel bottom, and the readout band */
    PAL_GRASS,         /* grass blades on the floor */
    PAL_CYAN,
    PAL_EDGE,          /* thin dark edge round every telescope part */
    PAL_SKY_TOP_DUSK,  /* top of the sky at full twilight */
    PAL_SKY_GLOW_DUSK, /* lower sky at full twilight */
    PAL_SKY_TOP_MOON,  /* top of the sky under a bright Moon */
    PAL_SKY_GLOW_MOON, /* lower sky under a bright Moon */
    PAL_SUN_NEAR,      /* Sun-side glow, centre */
    PAL_SUN_FAR,       /* Sun-side glow, middle */
    PAL_HAZE,          /* horizon haze at night */
    PAL_HAZE_DUSK,     /* horizon haze at twilight */
    PAL_RIM,           /* lit rim on the far hills */
    PAL_UNSAFE,        /* unsafe tint, dashes, word */
    PAL_STAR_0,        /* star tint by colour index, 0 blue white .. 4 orange */
    PAL_STAR_1,
    PAL_STAR_2,
    PAL_STAR_3,
    PAL_STAR_4,
    PAL_FIGURE,        /* star pattern lines */
    PAL_POLE,          /* pole marker */
    PAL_MOON_GLOW,
    PAL_MOON_DARK,     /* unlit Moon disc */
    PAL_MOON_LIT,      /* lit part of the Moon */
    PAL_GUIDE,         /* ring outline, guiding good */
    PAL_GUIDE_POOR,    /* ring outline, guiding poor */
    PAL_WHITE,         /* tip and pulse highlights */
    PAL_SHADOW,        /* contact shadow */
    PAL_RIG_WHITE,     /* tube, dew shield */
    PAL_RIG_BLACK,     /* rings, strap, focuser, filter wheel, weight, pier, joints */
    PAL_RIG_RED,       /* mount head and base, camera, collar, handle, rail, rims */
    PAL_RIG_STEEL,     /* counterweight shaft */
    PAL_RIG_BAND,      /* pale band on the pier */
    PAL_MOUTH,         /* open tube mouth */
    PAL_LENS,          /* lens inside the mouth */
    PAL_GLINT,         /* lens glint */
    PAL_N,
};

/* Mixed slots: palette_build() mixes them from the raw slots, then maps them. */
enum {
    PAL_X_LAND_FAR = PAL_N, /* far hills: land 0.30 toward the lower sky */
    PAL_X_LAND_MID,         /* middle hills: land 0.16 toward the lower sky */
    PAL_X_LEFT,             /* time left on the path: amber half way to the sky */
    PAL_X_LEFT_TICK,        /* its end tick: amber 0.3 toward the sky */
    PAL_X_GATHER,           /* light gathered inside the ring */
    PAL_ALL,
};

static const uint32_t PAL_RAW[PAL_N] = {
    [PAL_SKY_TOP]       = 0x03050d,
    [PAL_SKY_GLOW]      = 0x121d33,
    [PAL_AMBER]         = 0xf4b860,
    [PAL_FLIP_DARK]     = 0x0b1019,
    [PAL_FLIP_LIGHT]    = 0xe9e6dc,
    [PAL_TARGET_C]      = 0xfff3dc,
    [PAL_LAND]          = 0x030507,
    [PAL_FLOOR_FAR]     = 0x0f1b17,
    [PAL_FLOOR_NEAR]    = 0x070d09,
    [PAL_GRASS]         = 0x1c3324,
    [PAL_CYAN]          = 0x6fd3e0,
    [PAL_EDGE]          = 0x05070a,
    [PAL_SKY_TOP_DUSK]  = 0x0b1636,
    [PAL_SKY_GLOW_DUSK] = 0x2a4872,
    [PAL_SKY_TOP_MOON]  = 0x0c1730,
    [PAL_SKY_GLOW_MOON] = 0x22365a,
    [PAL_SUN_NEAR]      = 0xb06a44,
    [PAL_SUN_FAR]       = 0x5a3c52,
    [PAL_HAZE]          = 0x5f7fb0,
    [PAL_HAZE_DUSK]     = 0x8a6a78,
    [PAL_RIM]           = 0x8fb3e0,
    [PAL_UNSAFE]        = 0xd0503c,
    [PAL_STAR_0]        = 0xd6e0ff,
    [PAL_STAR_1]        = 0xeef1ff,
    [PAL_STAR_2]        = 0xfff4e2,
    [PAL_STAR_3]        = 0xffe3bf,
    [PAL_STAR_4]        = 0xffd0a8,
    [PAL_FIGURE]        = 0x8a97a8,
    [PAL_POLE]          = 0x7b8794,
    [PAL_MOON_GLOW]     = 0xc9d3e6,
    [PAL_MOON_DARK]     = 0x1c2129,
    [PAL_MOON_LIT]      = 0xcfd4dc,
    [PAL_GUIDE]         = 0x9fb3cc,
    [PAL_GUIDE_POOR]    = 0xe8a51e,
    [PAL_WHITE]         = 0xffffff,
    [PAL_SHADOW]        = 0x000000,
    [PAL_RIG_WHITE]     = 0xe9e7e1,
    [PAL_RIG_BLACK]     = 0x22252b,
    [PAL_RIG_RED]       = 0xd8281e,
    [PAL_RIG_STEEL]     = 0xb4bac3,
    [PAL_RIG_BAND]      = 0xc9d1bd,
    [PAL_MOUTH]         = 0x06070a,
    [PAL_LENS]          = 0x121a26,
    [PAL_GLINT]         = 0xcfe3ff,
};

/* ---- the draw list ---------------------------------------------------------- */

typedef struct { int16_t x1, y1, x2, y2; } seg_px_t;
typedef struct { int16_t x, y; uint32_t color; } bead_px_t;
/* One telescope primitive. Line: points 0 and 1, w = width. Triangle: three
 * points, w 0. Dot: point 0, w = radius. tone indexes rig_tone[]. No padding. */
typedef struct { int16_t x[3], y[3]; uint8_t kind, w, tone, round; } rig_px_t;
/* One piece of the slew trail or of the sweep arc: w = width, a = opacity
 * 0..255. No padding. */
typedef struct { int16_t x1, y1, x2, y2; uint8_t w, a; } trail_px_t;
typedef struct { int16_t x[3], y[3]; } tri_px_t;
typedef struct { int16_t x1, x2, y; } col_px_t;   /* land rectangle x1..x2, y..floor_y-1 */
typedef struct { int16_t x1, y1, x2, y2; uint8_t w, pad; } grass_px_t;
/* Label centre x on the horizon row; rank 2 = N E S W, 1 = NE SE SW NW,
 * 0 = degrees; hw = half the label box width, px. No padding. */
typedef struct { int16_t x; uint16_t az; uint8_t rank, hw; } compass_px_t;
/* r2 = diameter in px (the radius in half pixels), a = opacity 0..255,
 * tint = 0..4 (PAL_STAR_0 + tint), halo = 1 for a soft halo. No padding. */
typedef struct { int16_t x, y; uint8_t r2, a, tint, halo; } star_px_t;

/* Built from scratch (memset 0, then field writes) on every projection, so the
 * hash over its bytes is deterministic. Panel coordinates relative to the
 * scene object, which sits at (0,0) of the page root. */
typedef struct {
    uint16_t  n_trav, n_fut, n_hz, n_beads, n_rig, n_beam, n_cols, n_grass, n_compass;
    seg_px_t  trav[SCN_SEG_MAX];
    seg_px_t  fut[SCN_SEG_MAX];
    seg_px_t  hz[SCN_HZ_MAX];
    col_px_t  cols[SCN_COL_MAX];
    grass_px_t grass[SCN_GRASS_N];
    compass_px_t compass[SCN_COMPASS_MAX];
    int16_t   floor_y;            /* first row of the yard floor (level horizon), 0..size */
    int16_t   glow_y;             /* row where the sky reaches its glow colour */
    seg_px_t  beam[SCN_BEAM_MAX];
    bead_px_t beads[SCN_BEADS_MAX];
    seg_px_t  flip;
    tri_px_t  tarrow, aarrow;
    int16_t   target_x, target_y;
    int16_t   rise_x, rise_y, set_x, set_y;
    int16_t   ring_x, ring_y, ring_end;
    int16_t   tarrow_x, tarrow_y;
    uint8_t   target_in, rise_in, rise_fill, set_in;
    uint8_t   flip_in, ring_in, tarrow_in, aarrow_in;
    uint32_t  ring_color;
    /* sky (features 1 and 24) */
    int16_t   sun_x;              /* Sun glow centre column, may be off panel */
    uint8_t   sky_tw_q;           /* twilight in 1/32 steps, 0 = full night */
    uint8_t   sky_ml_q;           /* Moon light on the sky in 1/16 steps */
    uint8_t   unsafe;
    /* sky decor (features 2 to 6) */
    uint8_t   fig_a;              /* star pattern line opacity */
    uint8_t   pole_in, pole_a;
    uint8_t   moon_in, moon_il_q; /* lit fraction in 1/32 steps */
    uint16_t  n_haze, n_figs, n_stars;
    int16_t   pole_x, pole_y;
    int16_t   moon_x, moon_y, moon_glow_r;
    int16_t   moon_lit[2][9][2];  /* [0] lit limb, [1] terminator; nine x, y pairs each */
    uint32_t  haze_rgb;           /* mapped haze colour */
    col_px_t  haze[SCN_COL_MAX];  /* x1..x2 with the skyline (or the level horizon) at y */
    seg_px_t  figs[SCN_FIG_MAX];
    star_px_t stars[SKY_STAR_N];
    /* land and compass (features 6, 7, 9, 24) */
    uint16_t  n_cols_mid, n_cols_near, n_compass_minor;
    uint8_t   rim_in, pad_land;
    int16_t   unsafe_x, unsafe_y, unsafe_w, unsafe_h;   /* box of the word, w 0 = not shown */
    int16_t   compass_minor[SCN_COMPASS_MAX];           /* x of the ticks between two labels */
    uint32_t  rim_rgb;            /* mapped colour of the lit rim on the far hills */
    col_px_t  cols_mid[SCN_COL_MAX];
    col_px_t  cols_near[SCN_COL_MAX];
    col_px_t  cols_cap[SCN_COL_MAX];   /* land above the skyline behind the camera: row 0 down to y */
    uint16_t  n_cols_cap;
    uint8_t   lite, pad_cap;   /* lite: this frame is a glide frame drawn thin (SCN_FLY_LITE) */
    /* path, target, pictures (features 11 to 16) */
    uint16_t  n_left;
    uint8_t   left_tick_in, guide_in;
    uint8_t   n_ring_pts;         /* 0 = round outline, else SCN_RING_N + 1 points */
    uint8_t   ring_w, ring_arc_n, bead_new_in;
    uint8_t   gather_r, gather_a, tgt_r, tgt_a;
    uint8_t   tgt_core_a, pulse_stage, pulse_r, pulse_a;   /* stage 0 none, 1 travelling, 2 arriving */
    uint8_t   flare_in, flare_r, flare_a, pad_tgt;
    int16_t   ring_tip_x, ring_tip_y;
    int16_t   pulse_x, pulse_y, flare_x, flare_y;
    uint32_t  ring_rgb, ring_tip_rgb, pulse_rgb, flare_rgb;   /* final colours */
    seg_px_t  left_tick;
    seg_px_t  left[SCN_LEFT_MAX];
    int16_t   ring_pts[SCN_RING_N + 1][2];
    /* telescope (features 17 and 18) */
    int16_t   shadow_x, shadow_y, shadow_rx, shadow_ry;   /* contact shadow: centre, half sizes */
    uint8_t   shadow_in, pad_rig[3];
    uint32_t  accent;             /* final colour of the beam and the mouth rim */
    rig_px_t  rig[SKY_RIG_PRIM_MAX];
    /* events (features 22 and 23) */
    uint16_t  n_trail, n_sweep;
    trail_px_t trail[SCN_TRAIL_MAX - 1];
    trail_px_t sweep[SCN_SWEEP_MAX];       /* the sweep arc as line pieces */
    int16_t   sky_span;                    /* rows of the sky gradient; stepped while the camera flies */
} dl_t;

/* ---- scene state (one PSRAM block) ------------------------------------------ */

enum { CAM_MODE_NONE = 0, CAM_MODE_HOME, CAM_MODE_WORK };

struct nina_sky_scene_s {
    lv_obj_t   *obj;
    lv_timer_t *timer;
    bool        timer_running;
    uint16_t    ticks_since_update; /* tick_cb runs since the last update(); stale-flag guard */
    int64_t     last_tick_us;
    int         instance;
    int         gb;
    nina_sky_scene_opts_t opts;
    sky_frame_t frame;
    int         size;

    /* copied inputs */
    bool    have_site;
    float   lat, lon;
    bool    have_lst;
    float   lst_h;
    bool    have_target;
    float   ra_h, dec_deg;
    int8_t  horizon[SKY_HORIZON_N];
    bool    horizon_valid;
    uint8_t pier;
    bool    connected, at_park, slewing, tracking, flipping, exposing;
    bool    pointing_valid;
    sky_v3_t aim_target;
    sky_v3_t aim_disp;
    bool    have_aim_disp;
    bool    aim_settled;
    int64_t nina_epoch, nina_mono_us;

    /* copied inputs of the night sky (sky_inputs_copy(), from update() only) */
    bool     unsafe;              /* safety monitor connected and reporting unsafe */
    float    rms_hist[SCN_RMS_HIST]; /* last non-zero guiding RMS readings, oldest first */
    uint8_t  rms_n;
    float    guide_good, guide_ok, hfr_good, hfr_ok;   /* this node's thresholds */
    float    rms_now, hfr_now;
    int      left_min;            /* minutes left on the target, -1 unknown */
    uint32_t accent;              /* final colour: the filter's, cyan when parked or unknown */
    bool     accent_filter;       /* accent is a filter colour, not the palette cyan */
    bool     have_bodies;         /* Sun and Moon coordinates below are valid */
    int64_t  bodies_epoch;        /* epoch of the last moon_compute() */
    float    bodies_lat, bodies_lon;
    float    sun_ra_h, sun_dec, moon_ra_h, moon_dec, moon_illum;
    sky_v3_t sun_w, moon_w;       /* world directions; zero without bodies, site or sidereal time */
    float    tw, ml, wash;        /* twilight, Moon light on the sky, Moon wash of the stars */
    sky_m3_t eq2w;                /* equatorial to world; valid with a site and sidereal time */
    /* The sky turn (sky_turn()): what the directions above were last made from. */
    bool     have_sky;            /* they are valid */
    bool     sky_turn_due;        /* turn at the next update() whatever the age (camera goal change) */
    bool     demo, sky_demo, sky_bodies;
    float    sky_lst_h, sky_lat, sky_lon;
    int64_t  sky_turn_us;
    sky_v3_t star_eq[SKY_STAR_N]; /* filled once at create */
    sky_v3_t star_w[SKY_STAR_N];  /* valid with a site and sidereal time */
    float    fig_ra[SKY_FIG_N], fig_dec[SKY_FIG_N];   /* figure centres, filled once at create */

    /* finish pulse: set in update() when a picture is logged, dropped on age */
    bool     pulse_on;
    int64_t  pulse_t0_us;
    uint32_t pulse_rgb;           /* the bead colour (brightness mapped, not Red Night mapped) */

    /* Slew trail, and the ease through the pole on a change of pier side with
     * its sweep arc. Written by update(), tick_cb() and pose_now(); every flag
     * ends on elapsed time alone. */
    struct { sky_v3_t dir; int64_t t_us; } trail[SCN_TRAIL_MAX];   /* oldest first */
    int      trail_n;
    bool     have_prev_east, prev_east;   /* pier side at the last update() with a live telescope */
    bool     easing;
    int64_t  ease_t0_us;
    sky_axis_t ease_from;         /* the drawn axis state when the ease began */
    sky_axis_t disp_axis;         /* the axis state drawn by the last projection */
    sky_pose_t disp_pose;         /* and its pose */
    bool     have_disp;           /* both are a live (not rest) pose */
    struct { float a0, acc, last, r; int64_t end_us; bool on; } sweep;   /* degrees, px */

    /* render probe (debug_mode only) */
    lv_display_t *disp;
    bool     probe_on;            /* debug_mode at the last update() */
    int64_t  probe_t0_us;         /* render start, 0 = not timing this render */
    int64_t  probe_sum_us;
    uint32_t probe_n, probe_max_us;

    /* derived */
    sky_path_t path;
    float    ha_now;
    sky_v3_t target_vec;

    /* bead log, ring of SCN_BEADS_MAX */
    struct { float ha; uint32_t color; } beads[SCN_BEADS_MAX];
    int     bead_head, bead_count;
    int     prev_done;
    bool    have_prev_done;
    char    prev_filter[sizeof(((nina_client_t *)0)->current_filter)];
    bool    prev_have_target;
    float   prev_ra, prev_dec;

    /* camera */
    sky_cam_pose_t cam_now, cam_from, cam_goal;
    int64_t fly_t0_us;
    bool    flying;
    uint8_t cam_mode;
    bool    cam_placeholder;     /* current goal is the no-site stand-in */
    bool    goal_pending;
    bool    prev_want_work;
    bool    prev_horizon_valid;
    bool    prev_have_site;
    bool    prev_ha_east;        /* ha_now >= 0 at the last update: the pose mirrors on a change */
    float   goal_lat;
    sky_v3_t fit_dirs[SKY_PATH_N + 2];
    sky_v3_t fit_pts[SKY_RIG_FIT_N];
    /* telescope scratch for project_rig() (kept here, not on the stack) */
    sky_rig_part_t parts[SKY_RIG_PART_N];
    sky_rig_prim_t prims[SKY_RIG_PRIM_MAX];

    /* horizon scratch for project() (kept here, not on the stack) */
    float   hz_x[SCN_HZ_PTS], hz_y[SCN_HZ_PTS];
    bool    hz_ok[SCN_HZ_PTS];
    float   col_top[SCN_COL_MAX];
    float   col_az[SCN_COL_MAX];   /* azimuth of each column top */
    int16_t span_hold;             /* sky gradient span held for the glide, 0 = none */
    int16_t span_last;             /* span of the last frame drawn with the camera still */
    float   col_cap[SCN_COL_MAX];  /* size - y of the skyline behind the camera, 1e9 = none */
    float   col_lay[SCN_COL_MAX];  /* tops of the hill layer being merged */

    /* exposure ring input */
    float    exp_frac;
    uint32_t exp_color;

    /* Sky and floor gradients as SCN_STRIP_W px wide RGB565 strips with a 2x2 ordered
     * dither, tiled across the panel by lv_draw_image: the 16-bit panel shows
     * a plain dark gradient as 40 px bands and LVGL 9 has no dither of its own.
     * Re-baked only when their key changes. The sky has SCN_SKY_LEVELS strips,
     * one per step of horizontal distance from the Sun's column (the warm glow
     * at twilight); at full night only level 0 is baked and drawn. */
    uint16_t sky_strips[SCN_SKY_LEVELS][SCN_STRIP_W * SCN_STRIP_MAX];
    lv_image_dsc_t sky_dsc[SCN_SKY_LEVELS];
    uint32_t sky_key[8];
    uint16_t strip_floor[SCN_STRIP_W * SCN_STRIP_MAX];
    lv_image_dsc_t strip_floor_dsc;
    uint32_t strip_floor_key[6];

    /* Moon glow as an A8 picture (alpha with an ordered dither), drawn in the
     * glow colour. Re-baked only when its radius or the lit fraction changes. */
    uint8_t  moon_glow[SCN_MOON_GLOW_MAX * SCN_MOON_GLOW_MAX];
    lv_image_dsc_t moon_glow_dsc;
    uint32_t moon_glow_key[2];

    /* output */
    bool     red_night;           /* Red Night at the last palette_build(); part of the sky key */
    uint32_t pal[PAL_ALL];
    uint32_t rig_tone[SKY_TONE_N]; /* mapped telescope tones, see palette_build() */
    dl_t     dl;
    uint32_t dl_hash;
    lv_area_t rig_box;
    nina_sky_anchor_t anch[NINA_SKY_ANCHOR_N];
    nina_sky_info_t info;
    nina_sky_scene_moved_cb_t moved_cb;
    void    *moved_user;
};

/* ---- forward declarations --------------------------------------------------- */

static void     draw_cb(lv_event_t *e);
static void     delete_cb(lv_event_t *e);
static void     cover_cb(lv_event_t *e);
static void     tick_cb(lv_timer_t *t);
static void     frame_init(nina_sky_scene_t *s);
static void     palette_build(nina_sky_scene_t *s);
static uint32_t pal_map(int gb, uint32_t raw);
static uint32_t pal_red_only(uint32_t raw);
static void     goal_compute(nina_sky_scene_t *s, bool want_work);
static void     project(nina_sky_scene_t *s);
static void     ring_fill(nina_sky_scene_t *s);
static void     project_ring(nina_sky_scene_t *s);
static void     project_left(nina_sky_scene_t *s, const sky_cam_t *cam, float now_u, float b);
static void     project_pulse(nina_sky_scene_t *s, int64_t now_us);
static void     project_rig(nina_sky_scene_t *s, const sky_cam_t *cam, const sky_pose_t *pose,
                            bool parked);
static int16_t  rig_xy(float v);
static sky_pose_t pose_now(nina_sky_scene_t *s, int64_t now_us, bool *parked);
static void     trail_step(nina_sky_scene_t *s, int64_t now_us);
static void     sweep_step(nina_sky_scene_t *s, const sky_cam_t *cam, int64_t now_us);
static void     project_events(nina_sky_scene_t *s, const sky_cam_t *cam, int64_t now_us);
static void     project_compass(nina_sky_scene_t *s, const sky_cam_t *cam);
static void     cols_merge(const nina_sky_scene_t *s, const float *tops, int ncol, int16_t floor_y,
                           int merge, bool flat, col_px_t *out, uint16_t *n);
static const lv_font_t *compass_font(int rank);
static void     project_haze(nina_sky_scene_t *s, int ncol);
static void     sky_turn(nina_sky_scene_t *s, int64_t now_us);
static void     project_figs(nina_sky_scene_t *s, const sky_cam_t *cam);
static void     project_stars(nina_sky_scene_t *s, const sky_cam_t *cam);
static void     project_pole_moon(nina_sky_scene_t *s, const sky_cam_t *cam);
static void     commit(nina_sky_scene_t *s, bool force);
static void     anchors_build(nina_sky_scene_t *s, nina_sky_anchor_t *out);
static void     timer_kick(nina_sky_scene_t *s);
static bool     anim_active(const nina_sky_scene_t *s);
static void     bodies_refresh(nina_sky_scene_t *s, int64_t epoch);
static void     sky_inputs_copy(nina_sky_scene_t *s, const nina_client_t *d,
                                const app_config_t *cfg, int instance);
static void     probe_start_cb(lv_event_t *e);
static void     probe_ready_cb(lv_event_t *e);
static void     probe_report(nina_sky_scene_t *s);
static uint32_t fnv1a(const void *data, size_t n);
static int16_t  to_px(float v);
static bool     clip_seg(float *x1, float *y1, float *x2, float *y2, float lo, float hi);
static bool     add_seg(const nina_sky_scene_t *s, seg_px_t *arr, uint16_t *n, int max,
                        float x1, float y1, float x2, float y2);
static bool     proj_dir(const sky_cam_t *cam, sky_v3_t dir, float *x, float *y);
static void     emit_arc(nina_sky_scene_t *s, const sky_cam_t *cam, float h0, float h1,
                         seg_px_t *out, uint16_t *n);
static void     arrow_tri(tri_px_t *t, float x, float y, float ang_deg);
static bool     in_box(const lv_area_t *b, int grow, int x, int y);
static float    path_dist(const dl_t *dl, float x, float y, float lim);

typedef struct {
    lv_layer_t *layer;
    int32_t     ox, oy;
} dctx_t;

static void draw_line(const dctx_t *c, int x1, int y1, int x2, int y2,
                      uint32_t color, int width, lv_opa_t opa, bool round);
static void draw_ring(const dctx_t *c, int cx, int cy, int r, uint32_t color, int width,
                      lv_opa_t opa, int start, int end, bool rounded);
static void draw_dot(const dctx_t *c, int cx, int cy, int r, uint32_t color, lv_opa_t opa);
static void draw_fill(const dctx_t *c, int x1, int y1, int x2, int y2, uint32_t color);
static void draw_tri(const dctx_t *c, const int16_t *xs, const int16_t *ys,
                     uint32_t color, lv_opa_t opa);
static void draw_dashes(const dctx_t *c, const seg_px_t *segs, int n, uint32_t color, int width,
                        lv_opa_t opa, float dash, float gap, int ymax);
static void draw_box(const dctx_t *c, int x1, int y1, int x2, int y2, uint32_t color,
                     lv_opa_t opa, bool round);
static void draw_tri3(const dctx_t *c, const int16_t *p0, const int16_t *p1, const int16_t *p2,
                      uint32_t color);
static uint16_t dither565(uint32_t rgb, int x, int y);
static void strip_row_put(uint16_t *buf, int y, uint32_t rgb);
static void strip_dsc_set(lv_image_dsc_t *dsc, const uint16_t *buf, int rows);
static void strip_bake(uint16_t *buf, lv_image_dsc_t *dsc, uint32_t *key, int rows, int span,
                       uint32_t c0, uint32_t c1, uint32_t tint, int tint_rows);
static void sky_strips_bake(nina_sky_scene_t *s, int rows, const dl_t *dl);
static void moon_glow_bake(nina_sky_scene_t *s, int r, int il_q);
static void draw_strip(lv_layer_t *layer, const lv_image_dsc_t *dsc, const lv_area_t *a);
static void draw_sky(nina_sky_scene_t *s, lv_layer_t *layer, const lv_area_t *oa);
static void draw_decor(nina_sky_scene_t *s, const dctx_t *c);
static void draw_land(const nina_sky_scene_t *s, const dctx_t *c);
static void draw_compass(const nina_sky_scene_t *s, const dctx_t *c);
static void draw_target_ring(const nina_sky_scene_t *s, const dctx_t *c);
static void draw_rig(const nina_sky_scene_t *s, const dctx_t *c);

static const char SCN_UNSAFE_TXT[] = "Unsafe";

/* ---- small helpers ---------------------------------------------------------- */

static uint32_t fnv1a(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int16_t to_px(float v)
{
    if (!(v > -4000.0f)) {
        v = -4000.0f;
    }
    if (v > 4000.0f) {
        v = 4000.0f;
    }
    return (int16_t)floorf(v + 0.5f);
}

/* Liang-Barsky against the square [lo, hi] on both axes. */
static bool clip_seg(float *x1, float *y1, float *x2, float *y2, float lo, float hi)
{
    const float dx = *x2 - *x1, dy = *y2 - *y1;
    const float p[4] = { -dx, dx, -dy, dy };
    const float q[4] = { *x1 - lo, hi - *x1, *y1 - lo, hi - *y1 };
    float t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 4; i++) {
        if (fabsf(p[i]) < 1e-6f) {
            if (q[i] < 0.0f) {
                return false;
            }
        } else {
            const float r = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (r > t1) {
                    return false;
                }
                if (r > t0) {
                    t0 = r;
                }
            } else {
                if (r < t0) {
                    return false;
                }
                if (r < t1) {
                    t1 = r;
                }
            }
        }
    }
    const float ax = *x1, ay = *y1;
    *x1 = ax + t0 * dx;
    *y1 = ay + t0 * dy;
    *x2 = ax + t1 * dx;
    *y2 = ay + t1 * dy;
    return true;
}

static bool add_seg(const nina_sky_scene_t *s, seg_px_t *arr, uint16_t *n, int max,
                    float x1, float y1, float x2, float y2)
{
    if ((int)*n >= max) {
        return false;
    }
    if (!clip_seg(&x1, &y1, &x2, &y2, -SCN_CLIP_PAD, (float)s->size + SCN_CLIP_PAD)) {
        return false;
    }
    seg_px_t *g = &arr[*n];
    g->x1 = to_px(x1);
    g->y1 = to_px(y1);
    g->x2 = to_px(x2);
    g->y2 = to_px(y2);
    (*n)++;
    return true;
}

static bool proj_dir(const sky_cam_t *cam, sky_v3_t dir, float *x, float *y)
{
    return sky_cam_screen(cam, sky_cam_dir(cam, dir), x, y);
}

static bool in_box(const lv_area_t *b, int grow, int x, int y)
{
    return x >= b->x1 - grow && x <= b->x2 + grow && y >= b->y1 - grow && y <= b->y2 + grow;
}

/* Compass label face by rank: N E S W largest, the degrees smallest. */
static const lv_font_t *compass_font(int rank)
{
    if (rank >= 2) {
        return &lv_font_montserrat_26;
    }
    if (rank == 1) {
        return &lv_font_montserrat_20;
    }
    return &lv_font_montserrat_18;
}

/* Filled arrow with its centre at (x, y), tip toward ang_deg (0 right, 90 down). */
static void arrow_tri(tri_px_t *t, float x, float y, float ang_deg)
{
    const float a = ang_deg * SKY_DEG2RAD;
    const float dx = cosf(a), dy = sinf(a);
    const float h = 0.5f * SCN_ARROW_LEN;
    const float bx = x - dx * h, by = y - dy * h;
    t->x[0] = to_px(x + dx * h);
    t->y[0] = to_px(y + dy * h);
    t->x[1] = to_px(bx - dy * SCN_ARROW_HALF);
    t->y[1] = to_px(by + dx * SCN_ARROW_HALF);
    t->x[2] = to_px(bx + dy * SCN_ARROW_HALF);
    t->y[2] = to_px(by - dx * SCN_ARROW_HALF);
}

static uint32_t pal_red_only(uint32_t raw)
{
    if (theme_is_red_night(current_theme)) {
        raw = theme_red_luma(raw);
    }
    return raw;
}

static uint32_t pal_map(int gb, uint32_t raw)
{
    return app_config_apply_brightness(pal_red_only(raw), gb);
}

static void palette_build(nina_sky_scene_t *s)
{
    for (int i = 0; i < PAL_N; i++) {
        s->pal[i] = pal_map(s->gb, PAL_RAW[i]);
    }
    s->pal[PAL_X_LAND_FAR] = pal_map(s->gb, sky_rgb_mix(PAL_RAW[PAL_LAND], PAL_RAW[PAL_SKY_GLOW], 0.30f));
    s->pal[PAL_X_LAND_MID] = pal_map(s->gb, sky_rgb_mix(PAL_RAW[PAL_LAND], PAL_RAW[PAL_SKY_GLOW], 0.16f));
    s->pal[PAL_X_LEFT] = pal_map(s->gb, sky_rgb_mix(PAL_RAW[PAL_AMBER], PAL_RAW[PAL_SKY_TOP], 0.5f));
    s->pal[PAL_X_LEFT_TICK] = pal_map(s->gb, sky_rgb_mix(PAL_RAW[PAL_AMBER], PAL_RAW[PAL_SKY_TOP], 0.3f));
    s->pal[PAL_X_GATHER] = pal_map(s->gb, sky_rgb_mix(PAL_RAW[PAL_AMBER], PAL_RAW[PAL_TARGET_C], 0.3f));
    s->red_night = theme_is_red_night(current_theme);

    /* Telescope tones: lit, middle and shade per rig colour (mixed raw, then
     * mapped), in sky_rig_colour_t order, then the fixed ones. In Red Night a
     * hue is only its brightness and the plain red would land on the black,
     * so the red is lifted first: at equal tone the order stays white, steel,
     * red, black. */
    static const struct { uint8_t slot; uint8_t to_white[3]; float k[3]; } RIG_TONES[SKY_RIGC_N] = {
        [SKY_RIGC_WHITE] = { PAL_RIG_WHITE, { 0, 0, 0 }, { 0.20f, 0.36f, 0.55f } },
        [SKY_RIGC_RED]   = { PAL_RIG_RED,   { 0, 0, 0 }, { 0.10f, 0.32f, 0.55f } },
        [SKY_RIGC_BLACK] = { PAL_RIG_BLACK, { 1, 1, 0 }, { 0.17f, 0.04f, 0.35f } },
        [SKY_RIGC_STEEL] = { PAL_RIG_STEEL, { 0, 0, 0 }, { 0.12f, 0.32f, 0.52f } },
        [SKY_RIGC_BAND]  = { PAL_RIG_BAND,  { 0, 0, 0 }, { 0.18f, 0.36f, 0.55f } },
    };
    for (int col = 0; col < SKY_RIGC_N; col++) {
        uint32_t raw = PAL_RAW[RIG_TONES[col].slot];
        if (s->red_night && col == SKY_RIGC_RED) {
            raw = sky_rgb_mix(raw, 0xffffffu, SCN_RED_NIGHT_LIFT);
        }
        for (int t = 0; t < 3; t++) {
            s->rig_tone[SKY_TONE(col, t)] =
                pal_map(s->gb, sky_rgb_mix(raw, RIG_TONES[col].to_white[t] ? 0xffffffu : 0x000000u,
                                           RIG_TONES[col].k[t]));
        }
    }
    s->rig_tone[SKY_TONE_EDGE] = s->pal[PAL_EDGE];
    s->rig_tone[SKY_TONE_MOUTH] = s->pal[PAL_MOUTH];
    s->rig_tone[SKY_TONE_LENS] = s->pal[PAL_LENS];
    s->rig_tone[SKY_TONE_GLINT] = s->pal[PAL_GLINT];
    s->rig_tone[SKY_TONE_CAP] = s->rig_tone[SKY_TONE(SKY_RIGC_BLACK, 1)];
    s->rig_tone[SKY_TONE_CAP_RIM] = pal_map(s->gb, sky_rgb_mix(PAL_RAW[PAL_RIG_WHITE], 0x000000u, 0.45f));
    /* The accent follows the filter and is set by update(); while it is the
     * palette cyan it must follow a brightness or theme change here. */
    if (!s->accent_filter) {
        s->accent = s->pal[PAL_CYAN];
    }
    s->rig_tone[SKY_TONE_ACCENT] = s->accent;
}

static void frame_init(nina_sky_scene_t *s)
{
    const int S = screen_size();
    const int band = s->opts.ground_band_top;
    sky_frame_t *f = &s->frame;
    memset(f, 0, sizeof(*f));
    s->size = S;
    if (SCREEN_ROUND) {
        int r = screen_safe_radius() - 8;
        if (r <= 0) {
            r = S / 2 - 8;
        }
        f->round = true;
        f->cx = (float)(S / 2);
        f->cy = (float)(S / 2);
        f->r = (float)r;
        f->y1 = (band > 0) ? (float)band : 0.0f;
        f->px = (float)(S / 2);
        f->py = (band > 0) ? 0.5f * ((float)(S / 2 - r) + (float)band) : (float)(S / 2);
    } else {
        f->round = false;
        f->x0 = 24.0f;
        f->y0 = 24.0f;
        f->x1 = (float)(S - 24);
        f->y1 = (band > 0) ? (float)band : (float)(S - 24);
        f->px = (float)(S / 2);
        f->py = 0.5f * (f->y0 + f->y1);
    }
}

/* ---- create / delete -------------------------------------------------------- */

nina_sky_scene_t *nina_sky_scene_create(lv_obj_t *parent, const nina_sky_scene_opts_t *opts,
                                        int instance)
{
    nina_sky_scene_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s) {
        return NULL;
    }
    s->instance = instance;
    s->gb = app_config_get()->color_brightness;
    if (opts) {
        s->opts = *opts;
    }
    frame_init(s);
    palette_build(s);

    s->exp_frac = -1.0f;
    s->aim_settled = true;
    s->cam_placeholder = true;
    s->goal_pending = true;
    s->cam_mode = CAM_MODE_NONE;
    s->cam_goal.yaw_deg = 0.0f;
    s->cam_goal.pitch_deg = 10.0f;
    s->cam_goal.focal_px = sky_frame_half(&s->frame);   /* half / tan(45 deg) */
    s->cam_now = s->cam_goal;
    s->cam_from = s->cam_goal;
    s->left_min = -1;
    for (int i = 0; i < SKY_STAR_N; i++) {
        s->star_eq[i] = sky_eq_vec(SKY_STARS[i].ra_h, SKY_STARS[i].dec_deg);
    }
    for (int i = 0; i < SKY_FIG_N; i++) {
        sky_fig_centre(i, &s->fig_ra[i], &s->fig_dec[i]);
    }

    s->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s->obj);
    lv_obj_set_size(s->obj, s->size, s->size);
    lv_obj_set_pos(s->obj, 0, 0);
    lv_obj_remove_flag(s->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s->obj, draw_cb, LV_EVENT_DRAW_MAIN_END, s);
    lv_obj_add_event_cb(s->obj, cover_cb, LV_EVENT_COVER_CHECK, s);
    lv_obj_add_event_cb(s->obj, delete_cb, LV_EVENT_DELETE, s);

    /* Render probe: removed in delete_cb() before the block is freed. */
    s->disp = lv_obj_get_display(s->obj);
    if (s->disp) {
        lv_display_add_event_cb(s->disp, probe_start_cb, LV_EVENT_RENDER_START, s);
        lv_display_add_event_cb(s->disp, probe_ready_cb, LV_EVENT_RENDER_READY, s);
    }

    s->timer = lv_timer_create(tick_cb, SCN_TICK_MS, s);
    if (s->timer) {
        lv_timer_pause(s->timer);
    }

    project(s);
    s->dl_hash = fnv1a(&s->dl, sizeof(s->dl));
    anchors_build(s, s->anch);
    return s;
}

/* The sky strips paint every pixel of the object on every draw, so nothing
 * behind it can show: saying so lets LVGL skip the page and screen fills
 * under the scene. The object has no style of its own, so the base class
 * answers "does not cover" first; a masked answer is left alone. */
static void cover_cb(lv_event_t *e)
{
    lv_cover_check_info_t *info = (lv_cover_check_info_t *)lv_event_get_param(e);
    const nina_sky_scene_t *s = (const nina_sky_scene_t *)lv_event_get_user_data(e);
    if (!info || !s || !s->obj || info->res == LV_COVER_RES_MASKED) {
        return;
    }
    lv_area_t oa;
    lv_obj_get_coords(s->obj, &oa);
    if (lv_area_is_in(info->area, &oa, 0)) {
        info->res = LV_COVER_RES_COVER;
    }
}

static void delete_cb(lv_event_t *e)
{
    nina_sky_scene_t *s = (nina_sky_scene_t *)lv_event_get_user_data(e);
    if (!s) {
        return;
    }
    if (s->timer) {
        lv_timer_delete(s->timer);
        s->timer = NULL;
    }
    if (s->disp) {
        (void)lv_display_remove_event_cb_with_user_data(s->disp, probe_start_cb, s);
        (void)lv_display_remove_event_cb_with_user_data(s->disp, probe_ready_cb, s);
        s->disp = NULL;
    }
    heap_caps_free(s);
}

/* ---- render probe ------------------------------------------------------------- */

static void probe_start_cb(lv_event_t *e)
{
    nina_sky_scene_t *s = (nina_sky_scene_t *)lv_event_get_user_data(e);
    if (s && s->probe_on && s->timer_running) {
        s->probe_t0_us = esp_timer_get_time();
    }
}

static void probe_ready_cb(lv_event_t *e)
{
    nina_sky_scene_t *s = (nina_sky_scene_t *)lv_event_get_user_data(e);
    if (!s || s->probe_t0_us == 0) {
        return;
    }
    int64_t dt = esp_timer_get_time() - s->probe_t0_us;
    s->probe_t0_us = 0;
    if (dt < 0) {
        dt = 0;
    }
    if (dt > 60000000) {
        dt = 60000000;
    }
    s->probe_n++;
    s->probe_sum_us += dt;
    if ((uint32_t)dt > s->probe_max_us) {
        s->probe_max_us = (uint32_t)dt;
    }
}

/* One log line for the timer run that just ended, then a clean slate. */
static void probe_report(nina_sky_scene_t *s)
{
    if (s->probe_n > 0) {
        const unsigned avg = (unsigned)(s->probe_sum_us / (int64_t)s->probe_n);
        const unsigned mx = (unsigned)s->probe_max_us;
        ESP_LOGI(TAG, "sky scene render: %u frames, avg %u.%u ms, max %u.%u ms",
                 (unsigned)s->probe_n, avg / 1000u, (avg % 1000u) / 100u,
                 mx / 1000u, (mx % 1000u) / 100u);
    }
    s->probe_n = 0;
    s->probe_sum_us = 0;
    s->probe_max_us = 0;
    s->probe_t0_us = 0;
}

lv_obj_t *nina_sky_scene_obj(const nina_sky_scene_t *s)
{
    return s ? s->obj : NULL;
}

void nina_sky_scene_set_moved_cb(nina_sky_scene_t *s, nina_sky_scene_moved_cb_t cb, void *user)
{
    if (!s) {
        return;
    }
    s->moved_cb = cb;
    s->moved_user = user;
}

const nina_sky_info_t *nina_sky_scene_info(const nina_sky_scene_t *s)
{
    return s ? &s->info : NULL;
}

/* ---- update ----------------------------------------------------------------- */

void nina_sky_scene_update(nina_sky_scene_t *s, const nina_client_t *d, int instance, int gb)
{
    if (!s || !d) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    s->instance = instance;
    s->ticks_since_update = 0;
    bool force = false;
    if (gb != s->gb) {
        s->gb = gb;
        palette_build(s);
        force = true;
    }

    /* Site (P9: fall back to the weather location). */
    const app_config_t *cfg = app_config_get();
    /* A value that is not finite is a missing value, here and for the
     * sidereal time, the target and the mount pointing below. */
    if (d->site_valid && isfinite(d->site_lat_deg) && isfinite(d->site_lon_deg)) {
        s->have_site = true;
        s->lat = d->site_lat_deg;
        s->lon = d->site_lon_deg;
    } else if ((cfg->weather_lat != 0.0f || cfg->weather_lon != 0.0f) &&
               isfinite(cfg->weather_lat) && isfinite(cfg->weather_lon)) {
        s->have_site = true;
        s->lat = cfg->weather_lat;
        s->lon = cfg->weather_lon;
    } else {
        s->have_site = false;
    }
    if (s->lat > 90.0f) {
        s->lat = 90.0f;
    }
    if (s->lat < -90.0f) {
        s->lat = -90.0f;
    }

    /* Clock pair for the words. */
    s->nina_epoch = d->nina_clock_epoch;
    s->nina_mono_us = d->nina_clock_mono_us;

    /* Sidereal time. */
    if (d->lst_valid && isfinite(d->lst_hours)) {
        s->have_lst = true;
        s->lst_h = sky_lst_advance(d->lst_hours, (float)(now_us - d->lst_mono_us) / 1e6f);
    } else if (s->have_site) {
        int64_t epoch = 0;
        if (d->nina_clock_epoch != 0) {
            epoch = nina_client_now_epoch(d);
        } else if ((int64_t)time(NULL) >= 1700000000) {
            epoch = (int64_t)time(NULL);
        }
        s->have_lst = (epoch != 0);
        if (s->have_lst) {
            s->lst_h = sky_lst_hours(epoch, s->lon);
        }
    } else {
        s->have_lst = false;
    }

    /* Target. */
    s->have_target = d->target_coord_valid && s->have_site && s->have_lst &&
                     isfinite(d->target_ra_h) && isfinite(d->target_dec_deg);
    if (s->have_target) {
        s->ra_h = d->target_ra_h;
        s->dec_deg = d->target_dec_deg;
        s->ha_now = sky_ha_deg(s->lst_h, s->ra_h);
        /* ponytail: J2000 coordinates used as if current epoch; precession is
         * under half a degree (about 2 px). Precess here if a wider zoom ever
         * makes it visible. */
        s->target_vec = sky_hadec_to_vec(s->ha_now, s->dec_deg, s->lat);
    }

    bool target_changed = (s->have_target != s->prev_have_target);
    if (!target_changed && s->have_target) {
        float dra = fabsf(s->ra_h - s->prev_ra);
        if (dra > 12.0f) {
            dra = 24.0f - dra;
        }
        target_changed = dra > 0.01f || fabsf(s->dec_deg - s->prev_dec) > 0.1f;
    }
    if (target_changed) {
        s->bead_head = 0;
        s->bead_count = 0;
        s->have_prev_done = false;
        s->pulse_on = false;
    }
    if (s->pulse_on && now_us - s->pulse_t0_us >= SCN_PULSE_END_US) {
        s->pulse_on = false;
    }
    s->prev_have_target = s->have_target;
    s->prev_ra = s->ra_h;
    s->prev_dec = s->dec_deg;

    /* Horizon. */
    s->horizon_valid = d->horizon_valid;
    memcpy(s->horizon, d->horizon_alt, sizeof(s->horizon));

    /* Path. */
    if (s->have_target) {
        sky_path_build(&s->path, s->dec_deg, s->lat, s->ha_now, s->horizon, s->horizon_valid);
    }

    /* Counts and beads. */
    int done = 0, total = 0;
    sky_plan_counts(&d->plan, d->exposure_count, d->exposure_iterations, &done, &total);
    if (s->have_target && s->have_prev_done && done > s->prev_done) {
        int add = done - s->prev_done;
        if (add > 4) {
            add = 4;
        }
        const uint32_t col = app_config_get_filter_color(s->prev_filter, instance);
        for (int i = 0; i < add; i++) {
            s->beads[s->bead_head].ha = s->ha_now;
            s->beads[s->bead_head].color = col;
            s->bead_head = (s->bead_head + 1) % SCN_BEADS_MAX;
            if (s->bead_count < SCN_BEADS_MAX) {
                s->bead_count++;
            }
        }
        /* Finish pulse: the light of the picture runs down the beam and the
         * new bead flares once. Ends on age alone (tick_cb(), and above). */
        s->pulse_on = true;
        s->pulse_t0_us = now_us;
        s->pulse_rgb = col;
    }
    s->prev_done = done;
    s->have_prev_done = true;
    if (d->is_exposing) {
        memcpy(s->prev_filter, d->current_filter, sizeof(s->prev_filter));
        s->prev_filter[sizeof(s->prev_filter) - 1] = '\0';
    }

    /* Mount. */
    s->connected = d->mount_connected;
    s->at_park = d->mount_at_park;
    s->slewing = d->mount_slewing && !d->mount_at_park;
    s->tracking = d->mount_tracking;
    s->pier = d->mount_pier_side;
    s->exposing = d->is_exposing;
    s->flipping = strcmp(d->meridian_flip, "FLIPPING") == 0;
    s->pointing_valid = d->mount_pointing_valid && isfinite(d->mount_alt_deg) &&
                        isfinite(d->mount_az_deg);
    if (s->pointing_valid) {
        s->aim_target = sky_altaz_to_vec(d->mount_alt_deg, d->mount_az_deg);
        if (!s->have_aim_disp) {
            s->aim_disp = s->aim_target;
            s->have_aim_disp = true;
        }
        s->aim_settled = sky_sep_deg(s->aim_disp, s->aim_target) < SCN_AIM_DONE;
    } else {
        s->have_aim_disp = false;
        s->aim_settled = true;
    }

    /* Change of pier side: the drawn tube eases through the pole from the pose
     * it shows now to the newly reported one. A second change while it runs
     * starts again from the eased state. The first reading after create, after
     * a park and after the pointing becomes valid only records the side. With
     * no live telescope there is no ease, no sweep and no trail. */
    if (!s->at_park && s->pointing_valid && s->have_aim_disp && s->have_site) {
        const bool east = sky_pier_is_east((sky_pier_t)s->pier, s->have_target ? s->ha_now : 1.0f);
        if (s->have_prev_east && east != s->prev_east && s->have_disp) {
            s->ease_from = s->disp_axis;
            s->ease_t0_us = now_us;
            s->easing = true;
            s->sweep.on = false;
        }
        s->prev_east = east;
        s->have_prev_east = true;
    } else {
        s->have_prev_east = false;
        s->have_disp = false;
        s->easing = false;
        s->sweep.on = false;
        s->trail_n = 0;
    }
    if (s->easing && now_us - s->ease_t0_us >= (int64_t)SKY_POLE_EASE_MS * 1000) {
        s->easing = false;
    }

    /* Night sky inputs: safety, guiding, HFR, time left, accent, Sun, Moon, stars. */
    sky_inputs_copy(s, d, cfg, instance);

    /* Info for the words. */
    nina_sky_info_t *in = &s->info;
    in->target_known = s->have_target;
    in->target_up = false;
    if (s->have_target) {
        float alt = 0.0f, az = 0.0f;
        sky_vec_to_altaz(s->target_vec, &alt, &az);
        in->target_up = alt > sky_horizon_alt(s->horizon, s->horizon_valid, az);
    }
    in->on_target = s->pointing_valid && s->tracking && !s->slewing && s->have_target &&
                    sky_sep_deg(s->aim_target, s->target_vec) <= SKY_ON_TARGET_DEG;
    in->flip_known = d->flip_valid && isfinite(d->flip_minutes) && d->flip_minutes < 720.0f;
    /* A flip that would happen with the target under the horizon never
     * happens: drop the mark and the flip words on both layouts. */
    if (in->flip_known && s->have_target &&
        sky_path_margin_(sky_hadec_to_vec(sky_ha_after_minutes(s->ha_now, d->flip_minutes),
                                          s->dec_deg, s->lat),
                         s->horizon, s->horizon_valid) <= 0.0f) {
        in->flip_known = false;
    }
    in->minutes_to_flip = in->flip_known ? d->flip_minutes : 0.0f;
    in->rise_known = s->have_target && !in->target_up && s->path.has_rise;
    in->hours_to_rise = in->rise_known ? sky_hours_until_ha(s->ha_now, s->path.rise_ha_deg) : 0.0f;
    in->done = done;
    in->total = total;
    in->activity = sky_activity(s->flipping, d->is_exposing, d->mount_at_park, d->mount_slewing,
                                d->mount_tracking, s->have_target, in->target_up, done, total);

    /* Camera goal. */
    const bool want_work = s->have_target && d->mount_connected && !d->mount_at_park;
    if (want_work != s->prev_want_work || target_changed ||
        s->horizon_valid != s->prev_horizon_valid || s->have_site != s->prev_have_site ||
        (s->have_site && fabsf(s->lat - s->goal_lat) > 0.1f)) {
        s->goal_pending = true;
    }
    /* A fit goes stale over a session: the target climbs out of a frame fitted
     * hours ago, and the mount pose mirrors when the hour angle crosses zero.
     * Re-fit at rest (never mid-fly, or the fly would restart every tick). */
    const bool ha_east = s->ha_now >= 0.0f;
    if (want_work && s->have_site && !s->flying && s->cam_mode == CAM_MODE_WORK) {
        sky_cam_t gc;
        float gx = 0.0f, gy = 0.0f;
        sky_cam_setup(&gc, &s->cam_goal, &s->frame);
        if (ha_east != s->prev_ha_east ||
            !proj_dir(&gc, s->target_vec, &gx, &gy) ||
            !sky_frame_contains(&s->frame, gx, gy, SKY_FIT_MARGIN_PX)) {
            s->goal_pending = true;
        }
    }
    s->prev_ha_east = ha_east;
    s->prev_want_work = want_work;
    s->prev_horizon_valid = s->horizon_valid;
    s->prev_have_site = s->have_site;
    if (s->goal_pending && in->activity != SKY_ACT_FLIPPING) {
        s->goal_pending = false;
        goal_compute(s, want_work);
    }

    sky_turn(s, now_us);
    commit(s, force);
    timer_kick(s);
}

/* The one moon_compute() call: Sun and Moon right ascension and declination,
 * refreshed at most once per SCN_BODIES_S (or at once when the site or the
 * clock jumps). The double maths inside it is the only double code this
 * module reaches. No valid epoch or no site: no bodies.
 * ponytail: the Moon is geocentric (up to 1 degree of parallax, about 10 px
 * at a tight zoom); make it topocentric if that ever shows. */
static void bodies_refresh(nina_sky_scene_t *s, int64_t epoch)
{
    if (epoch == 0 || !s->have_site) {
        s->have_bodies = false;
        return;
    }
    if (s->have_bodies && epoch >= s->bodies_epoch &&
        epoch - s->bodies_epoch < SCN_BODIES_S &&
        s->lat == s->bodies_lat && s->lon == s->bodies_lon) {
        return;
    }
    moon_state_t ms;
    memset(&ms, 0, sizeof(ms));
    moon_compute((time_t)epoch, s->lat, s->lon, &ms);
    s->sun_ra_h = ms.sun_ra_deg / 15.0f;
    s->sun_dec = ms.sun_dec_deg;
    s->moon_ra_h = ms.ra_deg / 15.0f;
    s->moon_dec = ms.dec_deg;
    s->moon_illum = ms.illum;
    s->bodies_epoch = epoch;
    s->bodies_lat = s->lat;
    s->bodies_lon = s->lon;
    s->have_bodies = true;
}

/* Everything the night sky features read, copied out of the poll data and the
 * config. Called from nina_sky_scene_update() only, after the site, the
 * sidereal time and the mount state are set. */
static void sky_inputs_copy(nina_sky_scene_t *s, const nina_client_t *d, const app_config_t *cfg,
                            int instance)
{
    threshold_config_t th;

    s->probe_on = cfg->debug_mode;
    s->unsafe = d->safety_connected && !d->safety_is_safe;

    /* Guiding and focus, with this node's thresholds. */
    s->rms_now = d->guider.rms_total;
    if (s->rms_now > 0.0f) {
        if (s->rms_n >= SCN_RMS_HIST) {
            memmove(&s->rms_hist[0], &s->rms_hist[1], (SCN_RMS_HIST - 1) * sizeof(s->rms_hist[0]));
            s->rms_n = SCN_RMS_HIST - 1;
        }
        s->rms_hist[s->rms_n++] = s->rms_now;
    }
    s->hfr_now = d->hfr;
    memset(&th, 0, sizeof(th));
    app_config_get_rms_threshold_config(instance, &th);
    s->guide_good = th.good_max;
    s->guide_ok = th.ok_max;
    memset(&th, 0, sizeof(th));
    app_config_get_hfr_threshold_config(instance, &th);
    s->hfr_good = th.good_max;
    s->hfr_ok = th.ok_max;

    s->left_min = sky_parse_minutes(d->target_time_remaining);

    /* Accent: the filter colour (already brightness mapped by app_config, so
     * only the Red Night step is applied here, as for the beads), cyan when
     * parked or with no filter name. */
    if (d->current_filter[0] != '\0' && !d->mount_at_park) {
        s->accent = pal_red_only(app_config_get_filter_color(d->current_filter, instance));
        s->accent_filter = true;
    } else {
        s->accent = s->pal[PAL_CYAN];
        s->accent_filter = false;
    }
    s->rig_tone[SKY_TONE_ACCENT] = s->accent;

    /* Sun and Moon coordinates. */
    int64_t epoch = 0;
    if (d->nina_clock_epoch != 0) {
        epoch = nina_client_now_epoch(d);
    } else if ((int64_t)time(NULL) >= 1700000000) {
        epoch = (int64_t)time(NULL);
    }
    bodies_refresh(s, epoch);

    s->demo = cfg->demo_mode;
}

/* Turn the sky: the world directions of the stars, the Sun and the Moon (and
 * with them the star patterns, the twilight and the Moon light) on the scene's
 * own sidereal clock, so they turn together. Demo's sidereal time is simulated
 * and unrelated to the real clock, so demo forces full night.
 * Done at most every SCN_SKY_TURN_S: turning on every poll moved some star by
 * a pixel each time and repainted the idle panel on nearly every poll. Done at
 * once on the first valid data, a site change, a sidereal clock that is not
 * where the last turn expects it, bodies or demo coming or going, and a camera
 * goal change. The target, the path and the telescope use the live sidereal
 * time; the star clearance tests in project_stars() work on the drawn
 * positions, so they hold whatever the lag. From update() only. */
static void sky_turn(nina_sky_scene_t *s, int64_t now_us)
{
    if (!s->have_site || !s->have_lst) {
        s->sun_w = sky_v3(0.0f, 0.0f, 0.0f);
        s->moon_w = sky_v3(0.0f, 0.0f, 0.0f);
        s->tw = 0.0f;
        s->ml = 0.0f;
        s->wash = 0.0f;
        s->have_sky = false;
        s->sky_turn_due = false;
        return;
    }
    bool due = !s->have_sky || s->sky_turn_due || s->lat != s->sky_lat || s->lon != s->sky_lon ||
               s->have_bodies != s->sky_bodies || s->demo != s->sky_demo;
    if (!due) {
        const float el = (float)(now_us - s->sky_turn_us) / 1e6f;
        if (!(el >= 0.0f) || el >= SCN_SKY_TURN_S) {
            due = true;
        } else {
            const float want = sky_lst_advance(s->sky_lst_h, el);
            due = fabsf(sky_wrap180((s->lst_h - want) * 15.0f)) > SCN_SKY_JUMP_DEG;
        }
    }
    if (!due) {
        return;
    }
    s->have_sky = true;
    s->sky_turn_due = false;
    s->sky_lst_h = s->lst_h;
    s->sky_lat = s->lat;
    s->sky_lon = s->lon;
    s->sky_bodies = s->have_bodies;
    s->sky_demo = s->demo;
    s->sky_turn_us = now_us;

    s->sun_w = sky_v3(0.0f, 0.0f, 0.0f);
    s->moon_w = sky_v3(0.0f, 0.0f, 0.0f);
    s->tw = 0.0f;
    s->ml = 0.0f;
    s->wash = 0.0f;
    s->eq2w = sky_eq_to_world(s->lst_h, s->lat);
    for (int i = 0; i < SKY_STAR_N; i++) {
        s->star_w[i] = sky_m3_apply(&s->eq2w, s->star_eq[i]);
    }
    if (s->have_bodies) {
        float alt = 0.0f;
        s->sun_w = sky_hadec_to_vec(sky_ha_deg(s->lst_h, s->sun_ra_h), s->sun_dec, s->lat);
        s->moon_w = sky_hadec_to_vec(sky_ha_deg(s->lst_h, s->moon_ra_h), s->moon_dec, s->lat);
        sky_vec_to_altaz(s->sun_w, &alt, (float *)0);
        s->tw = s->demo ? 0.0f : sky_twilight(alt);
        sky_vec_to_altaz(s->moon_w, &alt, (float *)0);
        s->ml = sky_moon_light(s->moon_illum, alt);
        s->wash = sky_moon_wash(s->moon_illum, alt);
    }
}

/* Re-decide the camera goal and start (or snap) the fly to it. */
static void goal_compute(nina_sky_scene_t *s, bool want_work)
{
    sky_cam_pose_t goal;
    const bool was_placeholder = s->cam_placeholder;
    if (!s->have_site) {
        goal.yaw_deg = 0.0f;
        goal.pitch_deg = 10.0f;
        goal.focal_px = sky_frame_half(&s->frame);   /* half / tan(45 deg) */
        s->cam_mode = CAM_MODE_NONE;
        s->cam_placeholder = true;
    } else {
        /* The row the level horizon may not sink below. Layout 9 has no
         * readout band, and with nothing holding the horizon a high target
         * over a flat outline pitched the camera until the ground and most of
         * the telescope left the frame: keep a strip of ground there too. */
        const float gbt = (s->opts.ground_band_top > 0)
                              ? (float)s->opts.ground_band_top
                              : (float)s->size * (s->frame.round ? 0.80f : 0.90f);
        if (want_work) {
            /* Frame every sample above the outline, not just the working run:
             * a jagged horizon can make that run a sliver of a few low samples
             * and the view would zoom onto it. */
            const float yaw = sky_cam_up_yaw(&s->path, s->dec_deg, s->lat);
            int n = sky_path_up_dirs(&s->path, s->fit_dirs, SKY_PATH_N);
            if (n < 0) {
                n = 0;
            }
            if (n > SKY_PATH_N) {
                n = SKY_PATH_N;
            }
            s->fit_dirs[n++] = s->target_vec;
            const sky_pose_t pose = sky_mount_pose(s->lat, s->target_vec,
                                                   s->ha_now >= 0.0f ? SKY_PIER_EAST : SKY_PIER_WEST,
                                                   s->ha_now);
            const int np = sky_rig_model_fit_points(&pose, SKY_RIG_SCALE, s->fit_pts);
            sky_cam_fit(&goal, yaw, &s->frame, s->fit_dirs, n, s->fit_pts, np,
                        SKY_PITCH_MAX_DEG, gbt);
            s->cam_mode = CAM_MODE_WORK;
        } else {
            const sky_pose_t rest = sky_mount_rest_pose(s->lat);
            const int np = sky_rig_model_fit_points(&rest, SKY_RIG_SCALE, s->fit_pts);
            s->fit_dirs[0] = sky_pole(s->lat);
            sky_cam_fit(&goal, sky_cam_home_yaw(s->lat), &s->frame, s->fit_dirs, 1,
                        s->fit_pts, np, SKY_PITCH_MAX_DEG, gbt);
            s->cam_mode = CAM_MODE_HOME;
        }
        s->cam_placeholder = false;
        s->goal_lat = s->lat;
    }
    s->cam_goal = goal;
    s->sky_turn_due = true;

    const float dyaw = fabsf(sky_wrap180(goal.yaw_deg - s->cam_now.yaw_deg));
    const float dpitch = fabsf(goal.pitch_deg - s->cam_now.pitch_deg);
    const float dfoc = fabsf(goal.focal_px - s->cam_now.focal_px);
    if (was_placeholder || (dyaw <= 1.0f && dpitch <= 1.0f && dfoc <= 0.02f * s->cam_now.focal_px)) {
        s->cam_now = goal;
        s->flying = false;
    } else {
        s->cam_from = s->cam_now;
        s->fly_t0_us = esp_timer_get_time();
        s->flying = true;
    }
}

/* The one timer predicate, used by timer_kick() and by the pause check at the
 * end of tick_cb(). Every term ends on its own: the fly and the aim settle on
 * a time constant, slewing and flipping are cleared by the stale guard, the
 * finish pulse is dropped at SCN_PULSE_END_US of age, the ease ends after
 * SKY_POLE_EASE_MS, the sweep SKY_SWEEP_FADE_S after the ease, and every
 * trail point is dropped at SKY_TRAIL_S of age. */
static bool anim_active(const nina_sky_scene_t *s)
{
    return s->flying || !s->aim_settled || s->slewing || s->flipping || s->pulse_on ||
           s->easing || s->sweep.on || s->trail_n > 0;
}

static void timer_kick(nina_sky_scene_t *s)
{
    if (!s->timer) {
        return;
    }
    if (anim_active(s) && !s->timer_running) {
        s->last_tick_us = esp_timer_get_time();
        lv_timer_resume(s->timer);
        s->timer_running = true;
    }
}

static void tick_cb(lv_timer_t *t)
{
    nina_sky_scene_t *s = (nina_sky_scene_t *)lv_timer_get_user_data(t);
    if (!s) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    float dt = (float)(now - s->last_tick_us) / 1e6f;
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    if (dt > 0.5f) {
        dt = 0.5f;
    }
    s->last_tick_us = now;

    if (s->flying) {
        const float k = (float)(now - s->fly_t0_us) / ((float)SKY_FLY_MS * 1000.0f);
        if (k >= 1.0f) {
            s->cam_now = s->cam_goal;
            s->flying = false;
        } else {
            s->cam_now = sky_cam_pose_lerp(s->cam_from, s->cam_goal, sky_smoothstep(k));
        }
    }
    if (s->pointing_valid && s->have_aim_disp && !s->aim_settled) {
        const float a = 1.0f - expf(-dt / SCN_AIM_TAU_S);
        const sky_v3_t step = sky_v3_scale(sky_v3_sub(s->aim_target, s->aim_disp), a);
        const sky_v3_t next = sky_v3_norm(sky_v3_add(s->aim_disp, step));
        if (sky_v3_len(next) > 0.5f) {
            s->aim_disp = next;
        }
        if (sky_sep_deg(s->aim_disp, s->aim_target) < SCN_AIM_DONE) {
            s->aim_disp = s->aim_target;
            s->aim_settled = true;
        }
    }

    if (s->pulse_on && now - s->pulse_t0_us >= SCN_PULSE_END_US) {
        s->pulse_on = false;
    }

    /* Events, before the projection so the last frame of each is a clean one.
     * They read the pose the previous projection drew. */
    if (s->easing && now - s->ease_t0_us >= (int64_t)SKY_POLE_EASE_MS * 1000) {
        s->easing = false;
    }
    {
        sky_cam_t cam;
        sky_cam_setup(&cam, &s->cam_now, &s->frame);
        trail_step(s, now);
        sweep_step(s, &cam, now);
    }

    commit(s, false);

    if (s->ticks_since_update < UINT16_MAX) {
        s->ticks_since_update++;
    }
    if (!s->flying && s->aim_settled && (s->slewing || s->flipping) &&
        s->ticks_since_update > SCN_STALE_TICKS) {
        s->slewing = false;
        s->flipping = false;
    }
    if (s->ticks_since_update > SCN_STALE_TICKS) {
        s->easing = false;
    }

    if (!anim_active(s)) {
        lv_timer_pause(t);
        s->timer_running = false;
        probe_report(s);
    }
}

/* ---- projection ------------------------------------------------------------- */

/* Path arc from HA h0 to h1 (unwrapped, h1 > h0) through every 5 degree step. */
static void emit_arc(nina_sky_scene_t *s, const sky_cam_t *cam, float h0, float h1,
                     seg_px_t *out, uint16_t *n)
{
    if (!(h1 > h0)) {
        return;
    }
    float h = h0;
    float px = 0.0f, py = 0.0f;
    bool pv = proj_dir(cam, sky_hadec_to_vec(h, s->dec_deg, s->lat), &px, &py);
    for (int guard = 0; guard < SCN_SEG_MAX && h < h1; guard++) {
        float hn = floorf(h / SKY_PATH_STEP_DEG) * SKY_PATH_STEP_DEG + SKY_PATH_STEP_DEG;
        if (hn > h1) {
            hn = h1;
        }
        float x = 0.0f, y = 0.0f;
        const bool v = proj_dir(cam, sky_hadec_to_vec(hn, s->dec_deg, s->lat), &x, &y);
        if (pv && v) {
            (void)add_seg(s, out, n, SCN_SEG_MAX, px, py, x, y);
        }
        h = hn;
        px = x;
        py = y;
        pv = v;
    }
}

static void project(nina_sky_scene_t *s)
{
    dl_t *dl = &s->dl;
    memset(dl, 0, sizeof(*dl));
    const sky_frame_t *fr = &s->frame;
    sky_cam_t cam;
    sky_cam_setup(&cam, &s->cam_now, fr);
    float x = 0.0f, y = 0.0f;
    const int64_t now_us = esp_timer_get_time();
    dl->lite = (SCN_FLY_LITE && s->flying) ? 1 : 0;

    /* Sky: twilight and Moon light in coarse steps (each step is one strip
     * re-bake), and the column the Sun's glow is centred on: the Sun's
     * azimuth projected at altitude 0, or the nearer panel side when the Sun
     * is far off the camera yaw. No bodies, site or sidereal time: full night. */
    dl->sky_ml_q = (uint8_t)(s->ml * 16.0f + 0.5f);
    dl->unsafe = s->unsafe ? 1 : 0;
    if (s->have_bodies && s->have_site && s->have_lst) {
        dl->sky_tw_q = (uint8_t)(s->tw * 32.0f + 0.5f);
    }
    if (dl->sky_tw_q > 0) {
        float saz = 0.0f;
        sky_vec_to_altaz(s->sun_w, NULL, &saz);
        const bool vis = proj_dir(&cam, sky_altaz_to_vec(0.0f, saz), &x, &y);
        dl->sun_x = to_px(sky_sun_side_x(vis, x, sky_wrap180(saz - s->cam_now.yaw_deg),
                                         (float)s->size));
    }

    /* Path, split at the target. */
    if (s->have_target && !s->path.never_up) {
        const sky_path_t *p = &s->path;
        float a, b, now_u;
        if (p->always_up) {
            a = s->ha_now - 180.0f;
            b = s->ha_now + 180.0f;
            now_u = s->ha_now;
        } else {
            const float rise = p->has_rise ? p->rise_ha_deg
                                           : -180.0f + SKY_PATH_STEP_DEG * (float)p->run_first;
            const float set = p->has_set ? p->set_ha_deg
                                         : -180.0f + SKY_PATH_STEP_DEG * (float)p->run_last;
            float span = sky_wrap360(set - rise);
            if (span <= 0.0f) {
                span = 360.0f;
            }
            a = rise;
            b = rise + span;
            now_u = a + sky_wrap360(s->ha_now - a);
        }
        if (now_u <= b) {
            emit_arc(s, &cam, a, now_u, dl->trav, &dl->n_trav);
            emit_arc(s, &cam, now_u, b, dl->fut, &dl->n_fut);
            project_left(s, &cam, now_u, b);
        } else {
            emit_arc(s, &cam, a, b, dl->fut, &dl->n_fut);
        }

        /* Rise and set marks. */
        if (p->has_rise &&
            proj_dir(&cam, sky_hadec_to_vec(p->rise_ha_deg, s->dec_deg, s->lat), &x, &y) &&
            sky_frame_contains(fr, x, y, 0.0f)) {
            dl->rise_in = 1;
            dl->rise_x = to_px(x);
            dl->rise_y = to_px(y);
            dl->rise_fill = s->info.target_up ? 0 : 1;
        }
        if (p->has_set &&
            proj_dir(&cam, sky_hadec_to_vec(p->set_ha_deg, s->dec_deg, s->lat), &x, &y) &&
            sky_frame_contains(fr, x, y, 0.0f)) {
            dl->set_in = 1;
            dl->set_x = to_px(x);
            dl->set_y = to_px(y);
        }
    }

    /* Beads. */
    if (s->have_target) {
        for (int i = 0; i < s->bead_count && dl->n_beads < SCN_BEADS_MAX; i++) {
            const int k = (s->bead_head - s->bead_count + i + SCN_BEADS_MAX) % SCN_BEADS_MAX;
            if (proj_dir(&cam, sky_hadec_to_vec(s->beads[k].ha, s->dec_deg, s->lat), &x, &y) &&
                sky_frame_contains(fr, x, y, 0.0f)) {
                bead_px_t *bp = &dl->beads[dl->n_beads++];
                bp->x = to_px(x);
                bp->y = to_px(y);
                bp->color = pal_red_only(s->beads[k].color);
                dl->bead_new_in = (i == s->bead_count - 1) ? 1 : 0;
            }
        }
    }

    /* Flip mark: a bar across the path at the flip hour angle. */
    if (s->have_target && s->info.flip_known) {
        const float hf = sky_ha_after_minutes(s->ha_now, s->info.minutes_to_flip);
        float x2 = 0.0f, y2 = 0.0f;
        if (proj_dir(&cam, sky_hadec_to_vec(hf, s->dec_deg, s->lat), &x, &y) &&
            sky_frame_contains(fr, x, y, 0.0f)) {
            float tx = 0.0f, ty = 1.0f;
            if (proj_dir(&cam, sky_hadec_to_vec(hf + 1.0f, s->dec_deg, s->lat), &x2, &y2)) {
                const float l = sqrtf((x2 - x) * (x2 - x) + (y2 - y) * (y2 - y));
                if (l > 1e-3f) {
                    tx = (x2 - x) / l;
                    ty = (y2 - y) / l;
                }
            }
            dl->flip_in = 1;
            dl->flip.x1 = to_px(x - ty * SCN_FLIP_HALF);
            dl->flip.y1 = to_px(y + tx * SCN_FLIP_HALF);
            dl->flip.x2 = to_px(x + ty * SCN_FLIP_HALF);
            dl->flip.y2 = to_px(y - tx * SCN_FLIP_HALF);
        }
    }

    /* Target marker or amber edge arrow. */
    if (s->have_target) {
        const sky_v3_t tc = sky_cam_dir(&cam, s->target_vec);
        float ang = 0.0f;
        if (sky_cam_screen(&cam, tc, &x, &y) && sky_frame_contains(fr, x, y, 0.0f)) {
            const float soft = sky_hfr_soft(s->hfr_now, s->hfr_good, s->hfr_ok);
            dl->target_in = 1;
            dl->target_x = to_px(x);
            dl->target_y = to_px(y);
            dl->tgt_r = (uint8_t)(4.0f + 3.0f * soft + 0.5f);
            dl->tgt_a = (uint8_t)((1.0f - 0.45f * soft) * 255.0f + 0.5f);
            dl->tgt_core_a = (soft < 0.5f) ? (uint8_t)((1.0f - 2.0f * soft) * 255.0f + 0.5f) : 0;
        } else if (sky_edge_arrow(fr, &cam, tc, SKY_FIT_MARGIN_PX, &x, &y, &ang)) {
            dl->tarrow_in = 1;
            dl->tarrow_x = to_px(x);
            dl->tarrow_y = to_px(y);
            arrow_tri(&dl->tarrow, x, y, ang);
        }
    }

    /* Horizon. The outline is the smooth monotone curve through the stored
     * samples, every SCN_HZ_STEP_DEG over +-90 degrees of the yaw, clamped to the
     * level horizon. The obstructions under it are pixel-aligned column
     * rectangles from the outline down to the level horizon row, so no two
     * anti-aliased edges ever meet (seam free on both panel families). A
     * land-coloured round line along the outline hides their stepped tops,
     * which stay within half a column of it. The yard floor is everything below
     * the level horizon row: altitude 0 projects to one screen row (no roll). */
    {
        const float yaw = s->cam_now.yaw_deg;
        const float sz = (float)s->size;
        int ncol = (s->size + SCN_COL_W - 1) / SCN_COL_W;
        if (ncol > SCN_COL_MAX) {
            ncol = SCN_COL_MAX;
        }
        for (int c = 0; c < ncol; c++) {
            s->col_top[c] = 1e9f;
            s->col_az[c] = 0.0f;
            s->col_cap[c] = 1e9f;
        }
        for (int i = 0; i < SCN_HZ_PTS; i++) {
            const float az = yaw - 180.0f + SCN_HZ_STEP_DEG * (float)i;
            float alt = sky_horizon_alt_smooth(s->horizon, s->horizon_valid, az);
            if (alt < 0.0f) {
                alt = 0.0f;
            }
            s->hz_ok[i] = proj_dir(&cam, sky_altaz_to_vec(alt, az), &s->hz_x[i], &s->hz_y[i]);
        }
        for (int i = 1; i < SCN_HZ_PTS; i++) {
            if (!s->hz_ok[i - 1] || !s->hz_ok[i]) {
                continue;
            }
            const float hx0 = s->hz_x[i - 1], hy0 = s->hz_y[i - 1];
            const float hx1 = s->hz_x[i], hy1 = s->hz_y[i];
            float ex1 = hx0, ey1 = hy0, ex2 = hx1, ey2 = hy1;
            if (dl->n_hz < SCN_HZ_MAX && clip_seg(&ex1, &ey1, &ex2, &ey2, -8.0f, sz + 8.0f)) {
                seg_px_t *g = &dl->hz[dl->n_hz++];
                g->x1 = to_px(ex1);
                g->y1 = to_px(ey1);
                g->x2 = to_px(ex2);
                g->y2 = to_px(ey2);
            }
            /* Lower each column top whose centre this segment spans, and keep
             * the azimuth it was lowered at (the hill layers key on it). */
            const float az0 = yaw - 180.0f + SCN_HZ_STEP_DEG * (float)(i - 1);
            const float hdx = hx1 - hx0;
            const float xl = (hdx >= 0.0f) ? hx0 : hx1;
            const float xr = (hdx >= 0.0f) ? hx1 : hx0;
            float c0f = ceilf((xl - 0.5f * (float)SCN_COL_W) / (float)SCN_COL_W);
            float c1f = floorf((xr - 0.5f * (float)SCN_COL_W) / (float)SCN_COL_W);
            if (c0f < 0.0f) {
                c0f = 0.0f;
            }
            if (c1f > (float)(ncol - 1)) {
                c1f = (float)(ncol - 1);
            }
            if (!(c1f >= c0f)) {
                continue;
            }
            const int c0 = (int)c0f, c1 = (int)c1f;
            /* With the camera pitched up, the skyline more than 90 degrees off
             * the yaw comes back into the frame above the zenith row, upside
             * down: its land is ABOVE it. It must not lower a column top (that
             * painted the open sky between the two crossings as land); it
             * becomes a cap from row 0 down to the line instead.
             * ponytail: a column the outline folds short of gets neither fill;
             * count crossings per column if a sliver of sky ever shows there. */
            const bool behind = fabsf(az0 + 0.5f * SCN_HZ_STEP_DEG - yaw) > 90.0f;
            for (int c = c0; c <= c1; c++) {
                const float xc = (float)(c * SCN_COL_W) + 0.5f * (float)SCN_COL_W;
                const float k = (fabsf(hdx) > 1e-6f) ? (xc - hx0) / hdx : 0.0f;
                const float yc = hy0 + (hy1 - hy0) * k;
                if (behind) {
                    if (sz - yc < s->col_cap[c]) {
                        s->col_cap[c] = sz - yc;   /* mirrored, so cols_merge() keeps the lowest line */
                    }
                    continue;
                }
                if (yc < s->col_top[c]) {
                    s->col_top[c] = yc;
                    s->col_az[c] = az0 + SCN_HZ_STEP_DEG * k;
                }
            }
        }

        /* The level horizon row. */
        float fx = 0.0f, fy = sz;
        (void)proj_dir(&cam, sky_altaz_to_vec(0.0f, yaw), &fx, &fy);
        if (!(fy > 0.0f)) {
            fy = 0.0f;
        }
        if (fy > sz) {
            fy = sz;
        }
        dl->floor_y = to_px(fy);

        /* Column tops to rectangles; neighbours within SCN_COL_MERGE px share
         * one. The far layer is the outline itself, and the sky reaches its
         * glow colour at that layer's deepest dip. */
        int16_t deep = -1;
        for (int c = 0; c < ncol; c++) {
            if (s->col_top[c] < 1e8f) {
                const int16_t tt = to_px((s->col_top[c] > 0.0f) ? s->col_top[c] : 0.0f);
                if (tt < dl->floor_y && tt > deep) {
                    deep = tt;
                }
            }
        }
        cols_merge(s, s->col_top, ncol, dl->floor_y, SCN_COL_MERGE, false, dl->cols, &dl->n_cols);
        cols_merge(s, s->col_cap, ncol, (int16_t)s->size, SCN_COL_MERGE, false, dl->cols_cap,
                   &dl->n_cols_cap);
        for (int i = 0; i < dl->n_cols_cap; i++) {
            dl->cols_cap[i].y = (int16_t)(s->size - dl->cols_cap[i].y);   /* back to a screen row */
        }
        dl->glow_y = (int16_t)((deep > 0) ? deep : dl->floor_y);
        /* The sky gradient spans the rows down to the glow row. That row moves
         * on every frame of a camera glide and each new span re-bakes every
         * strip, so it moves in steps while flying; the frame after the glide
         * carries the exact span (one re-bake, a change of under a level). */
        dl->sky_span = (int16_t)((dl->glow_y > 1) ? dl->glow_y : 1);
        /* A new span re-bakes every strip (24 at twilight, 10 to 25 ms), and
         * the glow row moves on most glide frames: hold the span the glide
         * started with and let the first still frame carry the exact one. */
        if (s->flying) {
            if (s->span_hold == 0) {
                s->span_hold = (s->span_last > 0) ? s->span_last
                    : (int16_t)(((dl->sky_span + SCN_FLY_SPAN_Q - 1) / SCN_FLY_SPAN_Q) * SCN_FLY_SPAN_Q);
            }
            dl->sky_span = s->span_hold;
        } else {
            s->span_hold = 0;
            s->span_last = dl->sky_span;
        }
        project_haze(s, ncol);

        /* Hills in layers: the middle and the near hills are scaled-down
         * copies of the outline, by a fraction that wanders with the azimuth
         * so the hills stay put while the camera turns. They never rise above
         * the outline or sink below the level horizon. A flat horizon has no
         * height, so no layers. */
        if (s->horizon_valid) {
            for (int lay = 1; lay <= 2; lay++) {
                for (int c = 0; c < ncol; c++) {
                    float v = s->col_top[c];
                    if (!(v < 1e8f)) {
                        s->col_lay[c] = 1e9f;
                        continue;
                    }
                    if (v < 0.0f) {
                        v = 0.0f;
                    }
                    if (v > fy) {
                        v = fy;
                    }
                    s->col_lay[c] = fy - (fy - v) * sky_hill_frac(s->col_az[c], lay);
                }
                if (lay == 1) {
                    cols_merge(s, s->col_lay, ncol, dl->floor_y, SCN_COL_MERGE, false,
                               dl->cols_mid, &dl->n_cols_mid);
                } else {
                    cols_merge(s, s->col_lay, ncol, dl->floor_y, SCN_COL_MERGE, false,
                               dl->cols_near, &dl->n_cols_near);
                }
            }
        }

        /* Lit rim along the outline, half way between the haze and the rim colour. */
        if (!(SCN_FLY_LITE && s->flying)) {
            dl->rim_in = 1;
            dl->rim_rgb = pal_map(s->gb,
                                  sky_rgb_mix(sky_haze_colour(s->tw, PAL_RAW[PAL_HAZE],
                                                              PAL_RAW[PAL_HAZE_DUSK]),
                                              PAL_RAW[PAL_RIM], 0.5f));
        }

        project_compass(s, &cam);
    }

    /* Grass: SCN_GRASS_N blades at fixed pseudo-random spots on the ground
     * (LCG, constant seed), projected like the telescope so they stay put
     * while the camera flies. Wider and taller when near.
     * ponytail: drawn before the telescope, so a blade in front of the pier
     * base is painted over by it; add a z-sort if it ever shows. */
    {
        uint32_t rnd = 0x9e3779b9u;
        const float inv24 = 1.0f / 16777216.0f;
        for (int i = 0; i < SCN_GRASS_N; i++) {
            rnd = rnd * 1664525u + 1013904223u;
            const float u = (float)(rnd >> 8) * inv24;
            rnd = rnd * 1664525u + 1013904223u;
            const float v = (float)(rnd >> 8) * inv24;
            rnd = rnd * 1664525u + 1013904223u;
            const float t = (float)(rnd >> 8) * inv24;
            const float rad = SCN_GRASS_R0 + (SCN_GRASS_R - SCN_GRASS_R0) * sqrtf(u);
            const float ang = v * 360.0f * SKY_DEG2RAD;
            const float bx = rad * sinf(ang), by = rad * cosf(ang);
            const float h = 0.06f + 0.10f * t;
            const float lean = 0.06f * (t - 0.5f);
            const sky_v3_t ca = sky_cam_point(&cam, sky_v3(bx, by, 0.0f));
            const sky_v3_t cb = sky_cam_point(&cam, sky_v3(bx + lean, by, h));
            float ax = 0.0f, ay = 0.0f, tx = 0.0f, ty = 0.0f;
            if (!sky_cam_screen(&cam, ca, &ax, &ay) || !sky_cam_screen(&cam, cb, &tx, &ty)) {
                continue;
            }
            if (ay < (float)dl->floor_y) {
                continue;
            }
            if (!clip_seg(&ax, &ay, &tx, &ty, 0.0f, (float)s->size)) {
                continue;
            }
            float w = 0.025f * cam.focal_px / ca.z;
            if (w < 1.0f) {
                w = 1.0f;
            }
            if (w > 3.0f) {
                w = 3.0f;
            }
            grass_px_t *g = &dl->grass[dl->n_grass++];
            g->x1 = to_px(ax);
            g->y1 = to_px(ay);
            g->x2 = to_px(tx);
            g->y2 = to_px(ty);
            g->w = (uint8_t)w;
        }
    }

    /* Telescope and beam (needs a site for its latitude). The drawn pose is
     * decided in pose_now(); the beam leaves the mouth of that pose along its
     * tube, so while the tube eases through the pole the beam, the aim arrow
     * and the finish pulse all follow it. */
    dl->accent = s->accent;
    if (s->have_site) {
        bool parked = false;
        const sky_pose_t pose = pose_now(s, now_us, &parked);
        project_rig(s, &cam, &pose, parked);

        if (!parked) {
            const sky_v3_t aim = pose.tube;
            const sky_v3_t front = sky_rig_model_front(&pose, SKY_RIG_SCALE);
            const sky_v3_t ac = sky_cam_dir(&cam, aim);
            float sx = 0.0f, sy = 0.0f;
            bool aim_in = false;
            if (sky_cam_screen(&cam, sky_cam_point(&cam, front), &sx, &sy)) {
                float ex = 0.0f, ey = 0.0f;
                if (sky_cam_screen(&cam, ac, &ex, &ey)) {
                    (void)add_seg(s, dl->beam, &dl->n_beam, SCN_BEAM_MAX, sx, sy, ex, ey);
                    aim_in = sky_frame_contains(fr, ex, ey, 0.0f);
                } else {
                    static const float KS[4] = { 1.0f, 3.0f, 10.0f, 30.0f };
                    float lx = sx, ly = sy;
                    for (int i = 0; i < 4; i++) {
                        const sky_v3_t pw = sky_v3_add(front, sky_v3_scale(aim, KS[i]));
                        if (!sky_cam_screen(&cam, sky_cam_point(&cam, pw), &ex, &ey)) {
                            break;
                        }
                        (void)add_seg(s, dl->beam, &dl->n_beam, SCN_BEAM_MAX, lx, ly, ex, ey);
                        lx = ex;
                        ly = ey;
                    }
                }
            }
            float ang = 0.0f;
            if (!aim_in && sky_edge_arrow(fr, &cam, ac, SKY_FIT_MARGIN_PX, &x, &y, &ang)) {
                dl->aarrow_in = 1;
                arrow_tri(&dl->aarrow, x, y, ang);
            }
        }
        project_events(s, &cam, now_us);
    } else {
        s->have_disp = false;
        memset(&s->rig_box, 0, sizeof(s->rig_box));
    }

    /* Sky decor last: it keeps clear of the target, the path and the
     * telescope box of this same projection. */
    project_figs(s, &cam);
    project_stars(s, &cam);
    project_pole_moon(s, &cam);

    project_pulse(s, now_us);

    /* The word Unsafe: top centre, lower on a round panel (below the amber
     * "Last update" badge). Its box is a mark for layout 9's words. */
    if (s->unsafe) {
        const lv_font_t *font = &lv_font_montserrat_22;
        lv_point_t tsz;
        lv_text_get_size(&tsz, SCN_UNSAFE_TXT, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        dl->unsafe_w = (int16_t)(tsz.x + 4);
        dl->unsafe_h = (int16_t)lv_font_get_line_height(font);
        dl->unsafe_x = (int16_t)(s->size / 2 - dl->unsafe_w / 2);
        dl->unsafe_y = (int16_t)((fr->round ? 104 : 44) * s->size / 720);
    }

    project_ring(s);
}

/* The pose to draw: the rest pose (@p parked true: at park, or no pointing),
 * the live pose of the displayed aim, or, for SKY_POLE_EASE_MS after a change
 * of pier side, the pose part way from the one drawn when it began to the live
 * one, through the pole. Stores the drawn axis state and pose, which the next
 * ease, the trail and the sweep start from. Needs a site. */
static sky_pose_t pose_now(nina_sky_scene_t *s, int64_t now_us, bool *parked)
{
    if (s->at_park || !s->pointing_valid || !s->have_aim_disp) {
        /* ponytail: the parked pose is drawn snapped, not animated from the last pointing. */
        *parked = true;
        s->have_disp = false;
        return sky_mount_rest_pose(s->lat);
    }
    *parked = false;
    sky_pose_t pose = sky_mount_pose(s->lat, s->aim_disp, (sky_pier_t)s->pier,
                                     s->have_target ? s->ha_now : 1.0f);
    sky_axis_t ax = sky_axis_from_pose(&pose, s->lat);
    if (s->easing) {
        const float k = (float)(now_us - s->ease_t0_us) / ((float)SKY_POLE_EASE_MS * 1000.0f);
        if (k >= 0.0f && k < 1.0f) {
            ax = sky_axis_lerp(s->ease_from, ax, sky_smoothstep(k));
            pose = sky_axis_pose(ax, s->lat);
        }
    }
    s->disp_axis = ax;
    s->disp_pose = pose;
    s->have_disp = true;
    return pose;
}

/* Slew trail: drop the points older than SKY_TRAIL_S, then, while the drawn
 * aim is moving (settling on a new pointing, or easing through the pole), add
 * it when it is SCN_TRAIL_MIN_DEG or more from the newest point. Tracking
 * drift never reaches that spacing. */
static void trail_step(nina_sky_scene_t *s, int64_t now_us)
{
    int keep = 0;
    for (int i = 0; i < s->trail_n && i < SCN_TRAIL_MAX; i++) {
        const int64_t age = now_us - s->trail[i].t_us;
        if (age >= 0 && age < (int64_t)(SKY_TRAIL_S * 1e6f)) {
            s->trail[keep++] = s->trail[i];
        }
    }
    s->trail_n = keep;
    if (!s->have_disp || (s->aim_settled && !s->easing)) {
        return;
    }
    const sky_v3_t dir = s->disp_pose.tube;
    if (s->trail_n > 0 && sky_sep_deg(s->trail[s->trail_n - 1].dir, dir) < SCN_TRAIL_MIN_DEG) {
        return;
    }
    /* The first point needs a real move ahead of it: the small catch-up steps
     * of ordinary tracking must not start a trail (and a timer run). */
    if (s->trail_n == 0 && !s->easing && sky_sep_deg(dir, s->aim_target) < SCN_TRAIL_MIN_DEG) {
        return;
    }
    if (s->trail_n >= SCN_TRAIL_MAX) {
        memmove(&s->trail[0], &s->trail[1], (SCN_TRAIL_MAX - 1) * sizeof(s->trail[0]));
        s->trail_n = SCN_TRAIL_MAX - 1;
    }
    s->trail[s->trail_n].dir = dir;
    s->trail[s->trail_n].t_us = now_us;
    s->trail_n++;
}

/* Sweep arc: while the tube eases, add up the screen angle the tube mouth
 * turns through about the mount head. When the ease is over the arc fades for
 * SKY_SWEEP_FADE_S and turns off. */
static void sweep_step(nina_sky_scene_t *s, const sky_cam_t *cam, int64_t now_us)
{
    if (!s->have_disp) {
        s->sweep.on = false;
        return;
    }
    if (!s->easing) {
        if (s->sweep.on) {
            if (s->sweep.end_us == 0) {
                s->sweep.end_us = now_us;
            }
            if (now_us - s->sweep.end_us >= (int64_t)(SKY_SWEEP_FADE_S * 1e6f)) {
                s->sweep.on = false;
            }
        }
        return;
    }
    float hx = 0.0f, hy = 0.0f, mx = 0.0f, my = 0.0f;
    const sky_v3_t mouth = sky_rig_model_front(&s->disp_pose, SKY_RIG_SCALE);
    if (!sky_cam_screen(cam, sky_cam_point(cam, sky_rig_model_head()), &hx, &hy) ||
        !sky_cam_screen(cam, sky_cam_point(cam, mouth), &mx, &my)) {
        return;
    }
    const float dx = mx - hx, dy = my - hy;
    const float r = sqrtf(dx * dx + dy * dy);
    if (r < 1.0f) {
        return;   /* the tube points at the eye: no angle to read */
    }
    const float a = atan2f(dy, dx) * SKY_RAD2DEG;
    if (!s->sweep.on || s->sweep.end_us != 0) {
        s->sweep.a0 = a;
        s->sweep.acc = 0.0f;
        s->sweep.end_us = 0;
        s->sweep.on = true;
    } else {
        s->sweep.acc += sky_wrap180(a - s->sweep.last);
        if (s->sweep.acc > SCN_SWEEP_MAX_DEG) {
            s->sweep.acc = SCN_SWEEP_MAX_DEG;
        }
        if (s->sweep.acc < -SCN_SWEEP_MAX_DEG) {
            s->sweep.acc = -SCN_SWEEP_MAX_DEG;
        }
    }
    s->sweep.last = a;
    s->sweep.r = r;
}

/* The slew trail and the sweep arc into the draw list. Trail: one piece
 * between two neighbouring points that are both on the panel, thinner and
 * fainter with the age of the newer one. Sweep: the arc about the mount head
 * from its start angle to where the tube mouth is now, as short flat-ended
 * lines (an arc primitive of this radius costs its whole bounding box), wider
 * and stronger toward the tube, clipped to the panel. */
static void project_events(nina_sky_scene_t *s, const sky_cam_t *cam, int64_t now_us)
{
    dl_t *dl = &s->dl;
    const float sz = (float)s->size;
    float px = 0.0f, py = 0.0f;
    bool pv = false;
    for (int i = 0; i < s->trail_n && i < SCN_TRAIL_MAX; i++) {
        float tx = 0.0f, ty = 0.0f;
        const bool v = proj_dir(cam, s->trail[i].dir, &tx, &ty) &&
                       tx >= 0.0f && tx <= sz && ty >= 0.0f && ty <= sz;
        float life = 1.0f - (float)(now_us - s->trail[i].t_us) / (SKY_TRAIL_S * 1e6f);
        if (life > 1.0f) {
            life = 1.0f;
        }
        if (pv && v && life > 0.0f && dl->n_trail < SCN_TRAIL_MAX - 1) {
            trail_px_t *t = &dl->trail[dl->n_trail++];
            t->x1 = to_px(px);
            t->y1 = to_px(py);
            t->x2 = to_px(tx);
            t->y2 = to_px(ty);
            t->w = (uint8_t)(1.0f + 6.0f * life + 0.5f);
            t->a = (uint8_t)(0.4f * life * life * 255.0f + 0.5f);
        }
        px = tx;
        py = ty;
        pv = v;
    }

    if (s->sweep.on && fabsf(s->sweep.acc) >= 2.0f) {
        float hx = 0.0f, hy = 0.0f;
        if (sky_cam_screen(cam, sky_cam_point(cam, sky_rig_model_head()), &hx, &hy)) {
            float fade = 1.0f;
            if (s->sweep.end_us != 0) {
                fade = 1.0f - (float)(now_us - s->sweep.end_us) / (SKY_SWEEP_FADE_S * 1e6f);
            }
            if (!(fade > 0.0f)) {
                fade = 0.0f;
            }
            if (fade > 1.0f) {
                fade = 1.0f;
            }
            float r = s->sweep.r + 34.0f;
            if (!(r > 90.0f)) {
                r = 90.0f;
            }
            if (r > 2000.0f) {
                r = 2000.0f;
            }
            int n = (int)ceilf(fabsf(s->sweep.acc) / SCN_SWEEP_STEP_DEG);
            if (n < 1) {
                n = 1;
            }
            if (n > SCN_SWEEP_MAX) {
                n = SCN_SWEEP_MAX;
            }
            float qx = hx + r * cosf(s->sweep.a0 * SKY_DEG2RAD);
            float qy = hy + r * sinf(s->sweep.a0 * SKY_DEG2RAD);
            for (int i = 0; i < n && dl->n_sweep < SCN_SWEEP_MAX; i++) {
                const float w = (float)(i + 1) / (float)n;
                const float a = (s->sweep.a0 + s->sweep.acc * w) * SKY_DEG2RAD;
                const float nx = hx + r * cosf(a), ny = hy + r * sinf(a);
                float x1 = qx, y1 = qy, x2 = nx, y2 = ny;
                qx = nx;
                qy = ny;
                if (!clip_seg(&x1, &y1, &x2, &y2, -8.0f, sz + 8.0f)) {
                    continue;
                }
                trail_px_t *t = &dl->sweep[dl->n_sweep++];
                t->x1 = to_px(x1);
                t->y1 = to_px(y1);
                t->x2 = to_px(x2);
                t->y2 = to_px(y2);
                t->w = (uint8_t)(2.0f + 3.0f * w + 0.5f);
                t->a = (uint8_t)(0.45f * w * fade * 255.0f + 0.5f);
            }
        }
    }
}

/* A tessellator coordinate as a draw list pixel. The tessellator already
 * clamps to +-SKY_RIG_XY_LIMIT; the clamp here keeps the int16_t conversion
 * safe whatever comes in. (to_px() would bend lines that leave the panel.) */
static int16_t rig_xy(float v)
{
    if (!(v > -SKY_RIG_XY_LIMIT)) {
        v = -SKY_RIG_XY_LIMIT;
    }
    if (v > SKY_RIG_XY_LIMIT) {
        v = SKY_RIG_XY_LIMIT;
    }
    return (int16_t)floorf(v + 0.5f);
}

/* The telescope in @p pose as integer primitives, its screen box, and the
 * contact shadow under the pier foot. The part list and the float primitives
 * live in the scene block. The rig is not clipped to the frame. */
static void project_rig(nina_sky_scene_t *s, const sky_cam_t *cam, const sky_pose_t *pose, bool parked)
{
    dl_t *dl = &s->dl;
    float box[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int np = sky_rig_parts(pose, SKY_RIG_SCALE, s->parts);
    int n = sky_rig_tessellate_lod(s->parts, np, pose, SKY_RIG_SCALE, cam, parked,
                                   SCN_FLY_LITE && s->flying, s->prims, SKY_RIG_PRIM_MAX, box);
    if (n > SKY_RIG_PRIM_MAX) {
        n = SKY_RIG_PRIM_MAX;
    }
    for (int i = 0; i < n && dl->n_rig < SKY_RIG_PRIM_MAX; i++) {
        const sky_rig_prim_t *p = &s->prims[i];
        rig_px_t *q = &dl->rig[dl->n_rig++];
        for (int k = 0; k < 3; k++) {
            q->x[k] = rig_xy(p->x[k]);
            q->y[k] = rig_xy(p->y[k]);
        }
        float w = 0.0f;
        if (p->kind != SKY_PRIM_TRI) {
            w = floorf(p->w + 0.5f);
            if (!(w > 1.0f)) {
                w = 1.0f;
            }
            if (w > (float)SCN_RIG_W_MAX) {
                w = (float)SCN_RIG_W_MAX;
            }
        }
        q->kind = p->kind;
        q->w = (uint8_t)w;
        q->tone = (p->tone < SKY_TONE_N) ? p->tone : (uint8_t)SKY_TONE_EDGE;
        q->round = p->round ? 1 : 0;
    }
    if (n > 0) {
        s->rig_box.x1 = to_px(box[0]);
        s->rig_box.y1 = to_px(box[1]);
        s->rig_box.x2 = to_px(box[2]);
        s->rig_box.y2 = to_px(box[3]);
    } else {
        memset(&s->rig_box, 0, sizeof(s->rig_box));
    }

    /* Contact shadow: a flat pill centred on the pier foot. */
    const sky_v3_t fc = sky_cam_point(cam, sky_v3(0.0f, 0.0f, 0.0f));
    float fx = 0.0f, fy = 0.0f;
    if (n > 0 && sky_cam_screen(cam, fc, &fx, &fy)) {
        float rx = SCN_SHADOW_K * 2.0f * s->parts[SKY_RIGP_PIER].r0 * cam->focal_px / fc.z;
        if (rx > (float)SCN_SHADOW_MAX) {
            rx = (float)SCN_SHADOW_MAX;
        }
        if (rx >= 4.0f) {
            dl->shadow_in = 1;
            dl->shadow_x = to_px(fx);
            dl->shadow_y = to_px(fy);
            dl->shadow_rx = to_px(rx);
            dl->shadow_ry = to_px(rx * SCN_SHADOW_FLAT);
        }
    }
}

/* Column tops to runs x1..x2 with top y: neighbours whose tops are within
 * @p merge px share one run. Land (@p flat false): a column whose top is at
 * or below the level horizon row has no land and ends the run. Haze (@p flat
 * true): such a column is a run at the level horizon row. Shared by the three
 * hill layers and the haze. */
static void cols_merge(const nina_sky_scene_t *s, const float *tops, int ncol, int16_t floor_y,
                       int merge, bool flat, col_px_t *out, uint16_t *n)
{
    int16_t rmin = 0, rmax = 0;
    bool in_run = false;
    for (int c = 0; c < ncol; c++) {
        int16_t tt = floor_y;
        if (tops[c] < 1e8f) {
            tt = to_px((tops[c] > 0.0f) ? tops[c] : 0.0f);
            if (tt > floor_y) {
                tt = floor_y;
            }
        }
        if (!flat && tt >= floor_y) {
            in_run = false;
            continue;
        }
        int cx2 = c * SCN_COL_W + SCN_COL_W - 1;
        if (cx2 > s->size - 1) {
            cx2 = s->size - 1;
        }
        if (in_run) {
            const int16_t lo = (int16_t)((tt < rmin) ? tt : rmin);
            const int16_t hi = (int16_t)((tt > rmax) ? tt : rmax);
            if (hi - lo <= merge) {
                col_px_t *run = &out[*n - 1];
                run->x2 = (int16_t)cx2;
                run->y = lo;
                rmin = lo;
                rmax = hi;
                continue;
            }
        }
        if (*n >= SCN_COL_MAX) {
            in_run = false;
            continue;
        }
        col_px_t *nr = &out[(*n)++];
        nr->x1 = (int16_t)(c * SCN_COL_W);
        nr->x2 = (int16_t)cx2;
        nr->y = tt;
        rmin = tt;
        rmax = tt;
        in_run = true;
    }
}

/* Compass marks on the level horizon row (a straight horizontal line for a
 * camera with no roll): the step follows the zoom so labels never crowd.
 * N E S W are the largest, the in-between letters smaller, the degrees
 * smallest. A label is kept only when its whole box lies on the glass (the
 * frame grown by 8 px) and the horizon row is not under the readout band.
 * Between two marks sits a minor tick. The text is measured here so the draw
 * pass only replays the list. */
static void project_compass(nina_sky_scene_t *s, const sky_cam_t *cam)
{
    dl_t *dl = &s->dl;
    const sky_frame_t *fr = &s->frame;
    const float sz = (float)s->size;
    const float fy = (float)dl->floor_y;
    const bool band = s->opts.ground_band_top > 0 && s->opts.ground_band_top < s->size;
    const int floor_end = band ? s->opts.ground_band_top - 1 : s->size - 1;
    if (dl->floor_y - 1 > floor_end) {
        return;
    }
    const int step = sky_compass_step(cam->focal_px * tanf(SKY_DEG2RAD), SCN_COMPASS_MIN_PX);
    for (int a = 0; a < 360; a += step) {
        float mx = 0.0f, my = 0.0f;
        if (dl->n_compass < SCN_COMPASS_MAX &&
            proj_dir(cam, sky_altaz_to_vec(0.0f, (float)a), &mx, &my) && mx >= 0.0f && mx <= sz) {
            char buf[16];
            int rank = 0;
            const char *txt = sky_compass_label(a, buf, sizeof(buf), &rank);
            const lv_font_t *font = compass_font(rank);
            lv_point_t tsz;
            lv_text_get_size(&tsz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int hw = ((int)tsz.x + 4) / 2;
            if (hw > 127) {
                hw = 127;
            }
            const float bx0 = mx - (float)hw, bx1 = mx + (float)hw;
            const float by1 = fy - 8.0f;
            const float by0 = by1 - (float)lv_font_get_line_height(font);
            if (sky_frame_contains(fr, bx0, by0, -8.0f) && sky_frame_contains(fr, bx1, by0, -8.0f) &&
                sky_frame_contains(fr, bx0, by1, -8.0f) && sky_frame_contains(fr, bx1, by1, -8.0f)) {
                compass_px_t *m = &dl->compass[dl->n_compass++];
                m->x = to_px(mx);
                m->az = (uint16_t)a;
                m->rank = (uint8_t)rank;
                m->hw = (uint8_t)hw;
            }
        }
        if (dl->n_compass_minor < SCN_COMPASS_MAX &&
            proj_dir(cam, sky_altaz_to_vec(0.0f, (float)a + 0.5f * (float)step), &mx, &my) &&
            mx >= 0.0f && mx <= sz && sky_frame_contains(fr, mx, fy - 2.0f, -8.0f)) {
            dl->compass_minor[dl->n_compass_minor++] = to_px(mx);
        }
    }
}

/* Time left on the target: the stretch of the path it will still travel
 * before the plan ends, as a solid line from the target with a tick across
 * the path at its end. Shown while the target is up, the mount is not parked
 * and the time is known. */
static void project_left(nina_sky_scene_t *s, const sky_cam_t *cam, float now_u, float b)
{
    dl_t *dl = &s->dl;
    if (!s->info.target_up || s->at_park || s->left_min <= 0) {
        return;
    }
    float span = (float)s->left_min / 60.0f * 15.0f * SKY_SIDEREAL_RATE;
    if (span > b - now_u) {
        span = b - now_u;
    }
    if (!(span > 0.0f)) {
        return;
    }
    const float end = now_u + span;
    emit_arc(s, cam, now_u, end, dl->left, &dl->n_left);
    float ex = 0.0f, ey = 0.0f, qx = 0.0f, qy = 0.0f;
    if (proj_dir(cam, sky_hadec_to_vec(end, s->dec_deg, s->lat), &ex, &ey) &&
        sky_frame_contains(&s->frame, ex, ey, 0.0f) &&
        proj_dir(cam, sky_hadec_to_vec(end - 0.5f, s->dec_deg, s->lat), &qx, &qy)) {
        const float dx = ex - qx, dy = ey - qy;
        const float l = sqrtf(dx * dx + dy * dy);
        if (l > 1e-3f) {
            dl->left_tick_in = 1;
            dl->left_tick.x1 = to_px(ex + dy / l * 7.0f);
            dl->left_tick.y1 = to_px(ey - dx / l * 7.0f);
            dl->left_tick.x2 = to_px(ex - dy / l * 7.0f);
            dl->left_tick.y2 = to_px(ey + dx / l * 7.0f);
        }
    }
}

/* The one ring round the target. Its outline is the guiding: round and pale
 * when good, wavy and brighter when poor (shape and brightness, so it reads
 * in Red Night too). Shown while the mount is on the target. The colour is
 * premixed against the sky and opaque, so overlapping line caps never show.
 * The poor fraction moves in eighths and the wave amplitude in half pixels,
 * so guiding noise does not repaint the panel on every poll. */
static void project_ring(nina_sky_scene_t *s)
{
    dl_t *dl = &s->dl;
    if (s->info.on_target && dl->target_in) {
        /* No reading now: a round quiet ring, whatever the history holds. */
        const int hist_n = (s->rms_now > 0.0f) ? (int)s->rms_n : 0;
        const sky_guide_t g = sky_guide_shape(s->rms_hist, hist_n, s->rms_now,
                                              s->guide_good, s->guide_ok);
        const float poor = floorf(g.poor * 8.0f + 0.5f) / 8.0f;
        dl->guide_in = 1;
        dl->ring_w = 2;
        dl->ring_rgb = pal_map(s->gb,
                               sky_rgb_mix(sky_rgb_mix(PAL_RAW[PAL_GUIDE], PAL_RAW[PAL_GUIDE_POOR], poor),
                                           PAL_RAW[PAL_SKY_TOP], 1.0f - (0.32f + 0.5f * poor)));
        if (g.amp_px >= 0.5f) {
            /* The wave's two phases are fixed per target. */
            const uint32_t key = (uint32_t)(int32_t)(s->ra_h * 1000.0f) * 2654435761u +
                                 (uint32_t)(int32_t)((s->dec_deg + 90.0f) * 100.0f);
            const float ph1 = sky_hash01(key) * 6.2831853f;
            const float ph2 = sky_hash01(key + 1u) * 6.2831853f;
            for (int i = 0; i <= SCN_RING_N; i++) {
                const float a = (-90.0f + 360.0f * (float)i / (float)SCN_RING_N) * SKY_DEG2RAD;
                const float r = sky_ring_radius((float)SCN_RING_R, g.amp_px, a, ph1, ph2);
                dl->ring_pts[i][0] = to_px((float)dl->target_x + cosf(a) * r);
                dl->ring_pts[i][1] = to_px((float)dl->target_y + sinf(a) * r);
            }
            dl->n_ring_pts = SCN_RING_N + 1;
        }
    }
    ring_fill(s);
}

/* The finish pulse, by age since the picture was logged. Travelling: a point
 * runs from the aim end of the beam back to the tube mouth. Arriving: a ring
 * opens at the mouth. Flare: a ring opens once round the newest bead. With no
 * beam (parked, or no pointing) only the flare is drawn. Every stage ends on
 * age alone. */
static void project_pulse(nina_sky_scene_t *s, int64_t now_us)
{
    dl_t *dl = &s->dl;
    if (!s->pulse_on) {
        return;
    }
    const float age = (float)(now_us - s->pulse_t0_us) / 1e6f;
    if (age < 0.0f) {
        return;
    }
    const uint32_t pc = sky_rgb_mix(pal_red_only(s->pulse_rgb), s->pal[PAL_WHITE], 0.25f);
    if (dl->n_beam > 0 && age < SKY_PULSE_S) {
        float total = 0.0f;
        for (int i = 0; i < dl->n_beam; i++) {
            const float dx = (float)(dl->beam[i].x2 - dl->beam[i].x1);
            const float dy = (float)(dl->beam[i].y2 - dl->beam[i].y1);
            total += sqrtf(dx * dx + dy * dy);
        }
        float want = sky_smoothstep(age / SKY_PULSE_S) * total;
        float px = (float)dl->beam[dl->n_beam - 1].x2, py = (float)dl->beam[dl->n_beam - 1].y2;
        for (int i = (int)dl->n_beam - 1; i >= 0; i--) {
            const seg_px_t *g = &dl->beam[i];
            const float dx = (float)(g->x1 - g->x2), dy = (float)(g->y1 - g->y2);
            const float len = sqrtf(dx * dx + dy * dy);
            if (want <= len || i == 0) {
                float u = 0.0f;
                if (len > 1e-3f) {
                    u = (want < len) ? want / len : 1.0f;
                }
                px = (float)g->x2 + dx * u;
                py = (float)g->y2 + dy * u;
                break;
            }
            want -= len;
        }
        dl->pulse_stage = 1;
        dl->pulse_x = to_px(px);
        dl->pulse_y = to_px(py);
        dl->pulse_r = 14;
        dl->pulse_a = OPA_PCT(8);
        dl->pulse_rgb = pc;
    } else if (dl->n_beam > 0 && age < SKY_PULSE_S + SKY_ARRIVE_S) {
        const float u = (age - SKY_PULSE_S) / SKY_ARRIVE_S;
        dl->pulse_stage = 2;
        dl->pulse_x = dl->beam[0].x1;
        dl->pulse_y = dl->beam[0].y1;
        dl->pulse_r = (uint8_t)(8.0f + 22.0f * u + 0.5f);
        dl->pulse_a = (uint8_t)(0.5f * (1.0f - u) * 255.0f + 0.5f);
        dl->pulse_rgb = pc;
    }
    if (dl->bead_new_in && dl->n_beads > 0 && age < SKY_FLARE_S) {
        const bead_px_t *bp = &dl->beads[dl->n_beads - 1];
        const float k = age / SKY_FLARE_S;
        dl->flare_in = 1;
        dl->flare_x = bp->x;
        dl->flare_y = bp->y;
        dl->flare_r = (uint8_t)(5.0f + 16.0f * k + 0.5f);
        dl->flare_a = (uint8_t)(0.6f * (1.0f - k) * 255.0f + 0.5f);
        dl->flare_rgb = bp->color;
    }
}

/* Haze runs: every column top (the skyline, or the level horizon row where
 * the land is flat), neighbours within SCN_HAZE_MERGE px sharing one run.
 * haze_rgb is always written: the floor uses it too. */
static void project_haze(nina_sky_scene_t *s, int ncol)
{
    dl_t *dl = &s->dl;
    dl->haze_rgb = pal_map(s->gb, sky_haze_colour(s->tw, PAL_RAW[PAL_HAZE], PAL_RAW[PAL_HAZE_DUSK]));
    if (SCN_FLY_LITE && s->flying) {
        return;
    }
    cols_merge(s, s->col_top, ncol, dl->floor_y, SCN_HAZE_MERGE, true, dl->haze, &dl->n_haze);
}

/* Star patterns near where the telescope works (the target, else the pole):
 * faint lines between the pattern's stars, each shortened by 8 px at both ends
 * so it never touches a star. Needs the star directions of the last update(). */
static void project_figs(nina_sky_scene_t *s, const sky_cam_t *cam)
{
    dl_t *dl = &s->dl;
    if (!s->have_site || !s->have_lst || (SCN_FLY_LITE && s->flying)) {
        return;
    }
    float fk = 1.0f - 1.2f * s->tw;
    if (fk < 0.0f) {
        fk = 0.0f;
    }
    fk *= 1.0f - 0.6f * s->wash;
    if (fk < 0.05f) {
        return;
    }
    dl->fig_a = (uint8_t)(0.15f * fk * 255.0f + 0.5f);
    const float fra = s->have_target ? s->ra_h : 0.0f;
    const float fdec = s->have_target ? s->dec_deg : ((s->lat >= 0.0f) ? 90.0f : -90.0f);
    const float tx = (float)dl->target_x, ty = (float)dl->target_y;
    const float max_len = 0.8f * (float)s->size;
    for (int r = 0; r < SKY_FIG_RUN_N; r++) {
        const sky_fig_run_t *run = &SKY_FIG_RUNS[r];
        if (run->fig >= SKY_FIG_N ||
            sky_radec_sep_deg(s->fig_ra[run->fig], s->fig_dec[run->fig], fra, fdec) > 32.0f) {
            continue;
        }
        float ax = 0.0f, ay = 0.0f;
        bool av = false;
        for (int j = 0; j < (int)run->n; j++) {
            float bx = 0.0f, by = 0.0f;
            const int si = SKY_FIG_IDX[run->first + j];
            const bool bvis = si < SKY_STAR_N && proj_dir(cam, s->star_w[si], &bx, &by);
            if (j > 0 && av && bvis) {
                const float dx = bx - ax, dy = by - ay;
                const float len = sqrtf(dx * dx + dy * dy);
                const float mx = 0.5f * (ax + bx) - tx, my = 0.5f * (ay + by) - ty;
                const bool by_target = dl->target_in && mx * mx + my * my < 60.0f * 60.0f;
                if (len > 20.0f && len < max_len && !by_target) {
                    const float ux = 8.0f * dx / len, uy = 8.0f * dy / len;
                    (void)add_seg(s, dl->figs, &dl->n_figs, SCN_FIG_MAX,
                                  ax + ux, ay + uy, bx - ux, by - uy);
                }
            }
            ax = bx;
            ay = by;
            av = bvis;
        }
    }
}

/* Least distance from (x, y) to the drawn path, px; segments whose box is
 * more than @p lim away are skipped, so anything past lim reads as far. */
static float path_dist(const dl_t *dl, float x, float y, float lim)
{
    float best = 1e9f;
    for (int k = 0; k < 2; k++) {
        const seg_px_t *g = (k == 0) ? dl->trav : dl->fut;
        const int n = (k == 0) ? dl->n_trav : dl->n_fut;
        for (int i = 0; i < n; i++) {
            const float ax = (float)g[i].x1, ay = (float)g[i].y1;
            const float bx = (float)g[i].x2, by = (float)g[i].y2;
            if (x < ((ax < bx) ? ax : bx) - lim || x > ((ax > bx) ? ax : bx) + lim ||
                y < ((ay < by) ? ay : by) - lim || y > ((ay > by) ? ay : by) + lim) {
                continue;
            }
            const float d = sky_seg_dist(x, y, ax, ay, bx, by);
            if (d < best) {
                best = d;
            }
        }
    }
    return best;
}

/* Stars by magnitude: fainter ones drop out in twilight and near a bright
 * Moon; none is drawn on the target, the path or the telescope. Not marks:
 * layout 9 may place words over them. */
static void project_stars(nina_sky_scene_t *s, const sky_cam_t *cam)
{
    dl_t *dl = &s->dl;
    if (!s->have_site || !s->have_lst) {
        return;
    }
    const sky_frame_t *fr = &s->frame;
    const bool lite = SCN_FLY_LITE && s->flying;
    const bool near_moon = s->have_bodies && s->wash > 0.0f;
    const bool words = (s->opts.ground_band_top == 0);   /* layout 9 */
    const float limit = sky_star_limit(s->tw, s->wash);
    const float tx = (float)dl->target_x, ty = (float)dl->target_y;
    for (int i = 0; i < SKY_STAR_N && dl->n_stars < SKY_STAR_N; i++) {
        const sky_star_t *st = &SKY_STARS[i];
        float px = 0.0f, py = 0.0f;
        if (st->mag > limit || (lite && st->mag > 3.0f)) {
            continue;
        }
        if (!proj_dir(cam, s->star_w[i], &px, &py) || !sky_frame_contains(fr, px, py, 0.0f) ||
            py > (float)dl->glow_y) {
            continue;
        }
        float al = sky_star_alpha(st->mag, limit);
        if (near_moon) {
            al *= sky_star_moon_fade(s->wash, sky_sep_deg(s->moon_w, s->star_w[i]));
        }
        if (dl->target_in) {
            const float td2 = (px - tx) * (px - tx) + (py - ty) * (py - ty);
            if (td2 < 38.0f * 38.0f) {
                continue;
            }
            if (words && td2 < 110.0f * 110.0f) {
                al *= 0.55f;
            }
        }
        if (dl->n_rig > 0 && px > (float)(s->rig_box.x1 - 10) && px < (float)(s->rig_box.x2 + 10) &&
            py > (float)(s->rig_box.y1 - 10) && py < (float)(s->rig_box.y2 + 10)) {
            continue;
        }
        const float pd = path_dist(dl, px, py, 14.0f);
        if (pd < 7.0f) {
            continue;
        }
        if (pd < 14.0f) {
            al *= 0.5f;
        }
        if (al < 0.06f) {
            continue;
        }
        star_px_t *q = &dl->stars[dl->n_stars++];
        q->x = to_px(px);
        q->y = to_px(py);
        q->r2 = (uint8_t)(2.0f * sky_star_radius(st->mag) + 0.5f);
        q->a = (uint8_t)(al * 255.0f + 0.5f);
        q->tint = (uint8_t)sky_star_tint(st->bv);
        q->halo = (st->mag < 1.2f && !lite) ? 1 : 0;
    }
}

/* Pole marker (the elevated pole) and the Moon with its lit side toward the Sun. */
static void project_pole_moon(nina_sky_scene_t *s, const sky_cam_t *cam)
{
    dl_t *dl = &s->dl;
    const sky_frame_t *fr = &s->frame;
    float x = 0.0f, y = 0.0f;
    if (!s->have_site) {
        return;
    }
    if (proj_dir(cam, sky_pole(s->lat), &x, &y) && sky_frame_contains(fr, x, y, 0.0f) &&
        y < (float)dl->glow_y &&
        !(dl->target_in &&
          fabsf(x - (float)dl->target_x) + fabsf(y - (float)dl->target_y) < 44.0f)) {
        dl->pole_in = 1;
        dl->pole_x = to_px(x);
        dl->pole_y = to_px(y);
        dl->pole_a = (uint8_t)(0.45f * (1.0f - 0.6f * s->tw) * 255.0f + 0.5f);
    }

    /* Moon: higher than -2 degrees (z of a unit vector), on the glass, and not
     * far below the lowest skyline. The land is drawn later and covers a Moon
     * behind a hill. */
    if (!s->have_bodies || !s->have_lst || s->moon_w.z <= -0.0349f) {
        return;
    }
    if (!proj_dir(cam, s->moon_w, &x, &y) || !sky_frame_contains(fr, x, y, 0.0f) ||
        y > (float)(dl->glow_y + 20)) {
        return;
    }
    float il = s->moon_illum;
    if (!(il > 0.0f)) {
        il = 0.0f;
    }
    if (il > 1.0f) {
        il = 1.0f;
    }
    dl->moon_in = 1;
    dl->moon_x = to_px(x);
    dl->moon_y = to_px(y);
    dl->moon_il_q = (uint8_t)(il * 32.0f + 0.5f);
    dl->moon_glow_r = (int16_t)((50.0f + 60.0f * il) * (float)s->size / 720.0f);

    /* Screen angle of the lit side: a point a little way from the Moon along
     * the great circle toward the Sun. */
    float ang = 0.0f;
    const sky_v3_t tv = sky_v3_sub(s->sun_w, sky_v3_scale(s->moon_w, sky_v3_dot(s->sun_w, s->moon_w)));
    if (sky_v3_len(tv) > 1e-4f) {
        float sx = 0.0f, sy = 0.0f;
        if (proj_dir(cam, sky_v3_add(s->moon_w, sky_v3_scale(sky_v3_norm(tv), 0.04f)), &sx, &sy)) {
            ang = atan2f(sy - y, sx - x);
        }
    }
    float limb[9][2], term[9][2];
    sky_moon_lit_strip(il, (float)SCN_MOON_R, ang, x, y, limb, term);
    for (int i = 0; i < 9; i++) {
        dl->moon_lit[0][i][0] = to_px(limb[i][0]);
        dl->moon_lit[0][i][1] = to_px(limb[i][1]);
        dl->moon_lit[1][i][0] = to_px(term[i][0]);
        dl->moon_lit[1][i][1] = to_px(term[i][1]);
    }
}

/* The exposure arc on the ring, its tip, and the light gathered inside the
 * ring. Runs from project_ring() and from nina_sky_scene_set_exposure(),
 * never from the draw pass; it rewrites every field it owns. Round ring: the
 * arc is a true arc and the tip sits at its whole-degree end. Wavy ring: the
 * arc is the first ring_arc_n outline segments plus a part of the next one,
 * ending at the tip. */
static void ring_fill(nina_sky_scene_t *s)
{
    dl_t *dl = &s->dl;
    const bool exposing = s->info.on_target && s->exposing && s->exp_frac >= 0.0f;
    dl->ring_in = 0;
    dl->ring_x = 0;
    dl->ring_y = 0;
    dl->ring_end = 0;
    dl->ring_color = 0;
    dl->ring_arc_n = 0;
    dl->ring_tip_x = 0;
    dl->ring_tip_y = 0;
    dl->ring_tip_rgb = 0;
    dl->gather_r = 0;
    dl->gather_a = 0;
    if (exposing && dl->target_in) {
        float f = s->exp_frac;
        if (f > 1.0f) {
            f = 1.0f;
        }
        dl->ring_in = 1;
        dl->ring_x = dl->target_x;
        dl->ring_y = dl->target_y;
        dl->ring_end = (int16_t)(f * 360.0f);
        dl->ring_color = s->exp_color;
        dl->ring_tip_rgb = sky_rgb_mix(s->exp_color, s->pal[PAL_WHITE], 0.5f);
        if (dl->n_ring_pts == SCN_RING_N + 1) {
            float fr = ((f > 0.003f) ? f : 0.003f) * (float)SCN_RING_N;
            int whole = (int)fr;
            if (whole >= SCN_RING_N) {
                whole = SCN_RING_N;
                fr = (float)SCN_RING_N;
            }
            dl->ring_arc_n = (uint8_t)whole;
            if (whole < SCN_RING_N) {
                const float u = fr - (float)whole;
                const float x0 = (float)dl->ring_pts[whole][0], y0 = (float)dl->ring_pts[whole][1];
                dl->ring_tip_x = to_px(x0 + ((float)dl->ring_pts[whole + 1][0] - x0) * u);
                dl->ring_tip_y = to_px(y0 + ((float)dl->ring_pts[whole + 1][1] - y0) * u);
            } else {
                dl->ring_tip_x = dl->ring_pts[SCN_RING_N][0];
                dl->ring_tip_y = dl->ring_pts[SCN_RING_N][1];
            }
        } else {
            const float a = ((float)dl->ring_end - 90.0f) * SKY_DEG2RAD;
            dl->ring_tip_x = to_px((float)dl->target_x + cosf(a) * (float)SCN_RING_R);
            dl->ring_tip_y = to_px((float)dl->target_y + sinf(a) * (float)SCN_RING_R);
        }
    }
    if (dl->target_in) {
        float age = -1.0f;
        if (s->pulse_on) {
            age = (float)(esp_timer_get_time() - s->pulse_t0_us) / 1e6f;
        }
        const float g = sky_gathered(exposing, s->exp_frac, age);
        dl->gather_r = (uint8_t)(10.0f + 9.0f * g + 0.5f);
        dl->gather_a = (uint8_t)((0.03f + 0.06f * g) * 255.0f + 0.5f);
    }
}

static void anchors_build(nina_sky_scene_t *s, nina_sky_anchor_t *out)
{
    const dl_t *dl = &s->dl;
    memset(out, 0, sizeof(nina_sky_anchor_t) * NINA_SKY_ANCHOR_N);

    nina_sky_anchor_t *t = &out[NINA_SKY_ANCHOR_TARGET];
    if (s->have_target && !s->info.target_up && dl->rise_in) {
        t->shown = true;
        t->x = dl->rise_x;
        t->y = dl->rise_y;
    } else if (dl->target_in) {
        t->shown = true;
        t->x = dl->target_x;
        t->y = dl->target_y;
    } else if (dl->tarrow_in) {
        t->shown = true;
        t->at_edge = true;
        t->x = dl->tarrow_x;
        t->y = dl->tarrow_y;
    }

    nina_sky_anchor_t *f = &out[NINA_SKY_ANCHOR_FLIP];
    if (dl->flip_in) {
        f->shown = true;
        f->x = (int16_t)((dl->flip.x1 + dl->flip.x2) / 2);
        f->y = (int16_t)((dl->flip.y1 + dl->flip.y2) / 2);
    }

    nina_sky_anchor_t *r = &out[NINA_SKY_ANCHOR_RIG];
    if (dl->n_rig > 0) {
        r->shown = true;
        r->x = (int16_t)((s->rig_box.x1 + s->rig_box.x2) / 2);
        r->y = (int16_t)((s->rig_box.y1 + s->rig_box.y2) / 2);
    }
}

/* Re-project, invalidate on a changed draw list, tell the layout when an
 * anchor moved. @p force invalidates regardless (palette change). */
static void commit(nina_sky_scene_t *s, bool force)
{
    project(s);
    const uint32_t h = fnv1a(&s->dl, sizeof(s->dl));
    if (force || h != s->dl_hash) {
        s->dl_hash = h;
        (void)lv_obj_invalidate(s->obj);
    }
    nina_sky_anchor_t a[NINA_SKY_ANCHOR_N];
    anchors_build(s, a);
    bool moved = false;
    for (int i = 0; i < NINA_SKY_ANCHOR_N; i++) {
        if (a[i].shown != s->anch[i].shown || a[i].at_edge != s->anch[i].at_edge ||
            a[i].x != s->anch[i].x || a[i].y != s->anch[i].y) {
            moved = true;
        }
        s->anch[i] = a[i];
    }
    if (moved && s->moved_cb) {
        s->moved_cb(s->moved_user);
    }
}

/* ---- exposure ring, theme ----------------------------------------------------- */

void nina_sky_scene_set_exposure(nina_sky_scene_t *s, float frac, uint32_t color)
{
    if (!s) {
        return;
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }
    s->exp_frac = (frac >= 0.0f) ? frac : -1.0f;
    s->exp_color = color;
    ring_fill(s);
    const uint32_t h = fnv1a(&s->dl, sizeof(s->dl));
    if (h != s->dl_hash) {
        s->dl_hash = h;
        (void)lv_obj_invalidate(s->obj);
    }
}

void nina_sky_scene_apply_theme(nina_sky_scene_t *s, int gb)
{
    if (!s) {
        return;
    }
    s->gb = gb;
    palette_build(s);
    commit(s, true);
}

/* ---- queries for the layouts -------------------------------------------------- */

bool nina_sky_scene_anchor(const nina_sky_scene_t *s, nina_sky_anchor_id_t id,
                           nina_sky_anchor_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!s || (int)id < 0 || (int)id >= NINA_SKY_ANCHOR_N) {
        return false;
    }
    *out = s->anch[id];
    return out->shown;
}

void nina_sky_scene_rig_box(const nina_sky_scene_t *s, lv_area_t *out)
{
    if (!out) {
        return;
    }
    if (!s) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s->rig_box;
}

bool nina_sky_scene_box_fits(const nina_sky_scene_t *s, const lv_area_t *box)
{
    if (!s || !box) {
        return false;
    }
    const sky_frame_t *fr = &s->frame;
    return sky_frame_contains(fr, (float)box->x1, (float)box->y1, 8.0f) &&
           sky_frame_contains(fr, (float)box->x2, (float)box->y1, 8.0f) &&
           sky_frame_contains(fr, (float)box->x1, (float)box->y2, 8.0f) &&
           sky_frame_contains(fr, (float)box->x2, (float)box->y2, 8.0f);
}

bool nina_sky_scene_box_hits_marks(const nina_sky_scene_t *s, const lv_area_t *box)
{
    if (!s || !box) {
        return false;
    }
    const dl_t *dl = &s->dl;
    for (int i = 0; i < dl->n_fut; i++) {
        if (in_box(box, 10, dl->fut[i].x1, dl->fut[i].y1) ||
            in_box(box, 10, dl->fut[i].x2, dl->fut[i].y2)) {
            return true;
        }
    }
    if (dl->target_in && in_box(box, SCN_RING_R + 4, dl->target_x, dl->target_y)) {
        return true;
    }
    if (dl->unsafe && dl->unsafe_w > 0 &&
        box->x1 <= dl->unsafe_x + dl->unsafe_w && box->x2 >= dl->unsafe_x &&
        box->y1 <= dl->unsafe_y + dl->unsafe_h && box->y2 >= dl->unsafe_y) {
        return true;
    }
    if (dl->flip_in && (in_box(box, 4, dl->flip.x1, dl->flip.y1) ||
                        in_box(box, 4, dl->flip.x2, dl->flip.y2))) {
        return true;
    }
    if (dl->moon_in && in_box(box, 16, dl->moon_x, dl->moon_y)) {
        return true;
    }
    for (int i = 0; i < dl->n_beam; i++) {
        const seg_px_t *g = &dl->beam[i];
        const float dx = (float)(g->x2 - g->x1), dy = (float)(g->y2 - g->y1);
        const float len = sqrtf(dx * dx + dy * dy);
        int steps = (int)(len / 16.0f);
        if (steps > 400) {
            steps = 400;
        }
        for (int k = 0; k <= steps; k++) {
            const float u = (steps > 0) ? (float)k / (float)steps : 0.0f;
            const int px = (int)((float)g->x1 + dx * u);
            const int py = (int)((float)g->y1 + dy * u);
            if (in_box(box, 0, px, py)) {
                return true;
            }
        }
    }
    return false;
}

bool nina_sky_scene_clock_after(const nina_sky_scene_t *s, float hours, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return false;
    }
    buf[0] = '\0';
    if (!s) {
        return false;
    }
    int64_t epoch = 0;
    if (s->nina_epoch != 0) {
        epoch = s->nina_epoch + (esp_timer_get_time() - s->nina_mono_us) / 1000000;
    } else if ((int64_t)time(NULL) >= 1700000000) {
        epoch = (int64_t)time(NULL);
    } else {
        return false;
    }
    if (!(hours > -48.0f)) {
        hours = -48.0f;
    }
    if (hours > 48.0f) {
        hours = 48.0f;
    }
    const time_t t = (time_t)(epoch + (int64_t)(hours * 3600.0f));
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) {
        return false;
    }
    int hh = tmv.tm_hour, mm = tmv.tm_min;
    if (hh < 0) {
        hh = 0;
    }
    if (hh > 99) {
        hh = 99;
    }
    if (mm < 0) {
        mm = 0;
    }
    if (mm > 99) {
        mm = 99;
    }
    snprintf(buf, n, "%02d:%02d", hh, mm);
    return true;
}

/* ---- draw callback: reads the draw list and the palette only ------------------ */

static void draw_line(const dctx_t *c, int x1, int y1, int x2, int y2,
                      uint32_t color, int width, lv_opa_t opa, bool round)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    d.opa = opa;
    d.round_start = round ? 1 : 0;
    d.round_end = round ? 1 : 0;
    d.p1.x = (lv_value_precise_t)(x1 + c->ox);
    d.p1.y = (lv_value_precise_t)(y1 + c->oy);
    d.p2.x = (lv_value_precise_t)(x2 + c->ox);
    d.p2.y = (lv_value_precise_t)(y2 + c->oy);
    lv_draw_line(c->layer, &d);
}

/* RGB565 of a 24-bit colour with a 2x2 ordered dither (Bayer thresholds
 * centred: 1/8, 5/8, 7/8, 3/8), picked by column and row parity. */
static uint16_t dither565(uint32_t rgb, int x, int y)
{
    static const float th[4] = { 0.125f, 0.625f, 0.875f, 0.375f };
    const float t = th[((y & 1) << 1) | (x & 1)];
    int r = (int)((float)((rgb >> 16) & 0xffu) * (31.0f / 255.0f) + t);
    int g = (int)((float)((rgb >> 8) & 0xffu) * (63.0f / 255.0f) + t);
    int b = (int)((float)(rgb & 0xffu) * (31.0f / 255.0f) + t);
    if (r > 31) {
        r = 31;
    }
    if (g > 63) {
        g = 63;
    }
    if (b > 31) {
        b = 31;
    }
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* A SCN_STRIP_W x @p rows strip whose row i is c0 blended toward c1 by min(i, span) /
 * span, then tinted toward @p tint by 8 percent on row 0 falling to nothing
 * at @p tint_rows (the haze glow on the floor; the colours are already
 * mapped, and both mapping steps are linear, so mixing them here matches
 * mixing raw), dithered. Skipped when the six-entry key is unchanged. */
static void strip_bake(uint16_t *buf, lv_image_dsc_t *dsc, uint32_t *key, int rows, int span,
                       uint32_t c0, uint32_t c1, uint32_t tint, int tint_rows)
{
    if (rows > SCN_STRIP_MAX) {
        rows = SCN_STRIP_MAX;
    }
    if (rows < 1) {
        rows = 1;
    }
    if (span < 1) {
        span = 1;
    }
    if (tint_rows < 0) {
        tint_rows = 0;
    }
    if (key[0] == (uint32_t)rows && key[1] == (uint32_t)span && key[2] == c0 && key[3] == c1 &&
        key[4] == tint && key[5] == (uint32_t)tint_rows && dsc->data != NULL) {
        return;
    }
    key[0] = (uint32_t)rows;
    key[1] = (uint32_t)span;
    key[2] = c0;
    key[3] = c1;
    key[4] = tint;
    key[5] = (uint32_t)tint_rows;
    const float r0 = (float)((c0 >> 16) & 0xffu), g0 = (float)((c0 >> 8) & 0xffu), b0 = (float)(c0 & 0xffu);
    const float r1 = (float)((c1 >> 16) & 0xffu), g1 = (float)((c1 >> 8) & 0xffu), b1 = (float)(c1 & 0xffu);
    for (int y = 0; y < rows; y++) {
        const float k = (y >= span) ? 1.0f : (float)y / (float)span;
        uint32_t rgb = ((uint32_t)(r0 + (r1 - r0) * k + 0.5f) << 16) |
                       ((uint32_t)(g0 + (g1 - g0) * k + 0.5f) << 8) |
                       (uint32_t)(b0 + (b1 - b0) * k + 0.5f);
        if (y < tint_rows) {
            rgb = sky_rgb_mix(rgb, tint, 0.08f * (float)(tint_rows - y) / (float)tint_rows);
        }
        strip_row_put(buf, y, rgb);
    }
    strip_dsc_set(dsc, buf, rows);
}

/* Row @p y of a strip: the two dither phases of @p rgb, repeated across the
 * strip's width (even, so every tile starts on phase 0). */
static void strip_row_put(uint16_t *buf, int y, uint32_t rgb)
{
    const uint16_t v0 = dither565(rgb, 0, y), v1 = dither565(rgb, 1, y);
    uint16_t *row = &buf[SCN_STRIP_W * y];
    for (int x = 0; x < SCN_STRIP_W; x += 2) {
        row[x] = v0;
        row[x + 1] = v1;
    }
}

/* Descriptor of a SCN_STRIP_W x @p rows RGB565 strip.
 * No image cache to drop: this build runs with LV_CACHE_DEF_SIZE 0, so a
 * raw RGB565 descriptor is read straight from here on every draw. */
static void strip_dsc_set(lv_image_dsc_t *dsc, const uint16_t *buf, int rows)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = SCN_STRIP_W;
    dsc->header.h = (uint32_t)rows;
    dsc->header.stride = SCN_STRIP_W * 2;
    dsc->data_size = (uint32_t)rows * (SCN_STRIP_W * 2u);
    dsc->data = (const uint8_t *)buf;
}

/* The sky strips. Row colour: a three-stop gradient from the top of the panel
 * to the glow row (the deepest dip of the skyline), the Sun-side glow mixed in
 * by its true radial distance from (Sun column, glow row), then the unsafe
 * tint rising behind the skyline, all mixed from raw colours and mapped last.
 * Rows below the glow row repeat it. Strip L stands for the columns whose
 * distance from the Sun's column is (L + 0.5) * R / SCN_SKY_LEVELS, so the
 * Sun column itself is not part of the key. At full night only strip 0 is
 * baked.
 * Draw pass exception, as for strip_bake(): this is reached from draw_cb()
 * and uses sqrtf, but only when the key changed; it reads the draw list, the
 * raw palette and the scene's brightness and Red Night flag, nothing else. */
static void sky_strips_bake(nina_sky_scene_t *s, int rows, const dl_t *dl)
{
    if (rows > SCN_STRIP_MAX) {
        rows = SCN_STRIP_MAX;
    }
    if (rows < 1) {
        rows = 1;
    }
    int span = dl->sky_span;
    if (span < 1) {
        span = 1;
    }
    const uint32_t key[8] = {
        (uint32_t)rows, (uint32_t)span, dl->sky_tw_q, dl->sky_ml_q, dl->unsafe,
        (uint32_t)s->gb, s->red_night ? 1u : 0u, (uint32_t)s->size,
    };
    if (memcmp(key, s->sky_key, sizeof(key)) == 0 && s->sky_dsc[0].data != NULL) {
        return;
    }
    memcpy(s->sky_key, key, sizeof(key));

    const float tw = (float)dl->sky_tw_q / 32.0f;
    const float ml = (float)dl->sky_ml_q / 16.0f;
    const sky_grad_t g = sky_colours(tw, ml, PAL_RAW[PAL_SKY_TOP], PAL_RAW[PAL_SKY_GLOW],
                                     PAL_RAW[PAL_SKY_TOP_DUSK], PAL_RAW[PAL_SKY_GLOW_DUSK],
                                     PAL_RAW[PAL_SKY_TOP_MOON], PAL_RAW[PAL_SKY_GLOW_MOON],
                                     sky_haze_colour(tw, PAL_RAW[PAL_HAZE], PAL_RAW[PAL_HAZE_DUSK]));
    const float R = 0.9f * (float)s->size;
    const int levels = (dl->sky_tw_q > 0) ? SCN_SKY_LEVELS : 1;
    int tint = 140 * s->size / 720;
    if (tint > span) {
        tint = span;
    }
    for (int lv = 0; lv < levels; lv++) {
        const float dx = ((float)lv + 0.5f) * R / (float)SCN_SKY_LEVELS;
        uint16_t *buf = s->sky_strips[lv];
        uint32_t rgb = 0;
        for (int y = 0; y < rows; y++) {
            if (y <= span) {
                const float k = (float)y / (float)span;
                rgb = (k < 0.55f) ? sky_rgb_mix(g.top, g.mid, k / 0.55f)
                                  : sky_rgb_mix(g.mid, g.low, (k - 0.55f) / 0.45f);
                if (dl->sky_tw_q > 0 && R > 1.0f) {
                    const float dy = (float)(span - y);
                    uint32_t gc = 0;
                    float ga = 0.0f;
                    sky_sun_glow(sqrtf(dx * dx + dy * dy) / R, tw, PAL_RAW[PAL_SUN_NEAR],
                                 PAL_RAW[PAL_SUN_FAR], &gc, &ga);
                    rgb = sky_rgb_mix(rgb, gc, ga);
                }
                if (dl->unsafe && tint > 0 && y > span - tint) {
                    rgb = sky_rgb_mix(rgb, PAL_RAW[PAL_UNSAFE],
                                      0.2f * (float)(y - (span - tint)) / (float)tint);
                }
                if (s->red_night) {
                    rgb = theme_red_luma(rgb);
                }
                rgb = app_config_apply_brightness(rgb, s->gb);
            }
            strip_row_put(buf, y, rgb);
        }
        strip_dsc_set(&s->sky_dsc[lv], buf, rows);
    }
}

/* The Moon glow picture: 2r x 2r of alpha, full strength (0.14 of the lit
 * fraction) out to the disc edge and falling to 0 at r, with an ordered
 * dither so the blend does not band on the 16-bit panel.
 * Draw pass exception, as for the strips: reached from draw_cb(), square
 * roots inside sky_glow_a8(), but only when the radius or the lit fraction
 * changed. */
static void moon_glow_bake(nina_sky_scene_t *s, int r, int il_q)
{
    if (r > SCN_MOON_GLOW_MAX / 2) {
        r = SCN_MOON_GLOW_MAX / 2;
    }
    if (r < 1) {
        r = 1;
    }
    if (s->moon_glow_key[0] == (uint32_t)r && s->moon_glow_key[1] == (uint32_t)il_q &&
        s->moon_glow_dsc.data != NULL) {
        return;
    }
    s->moon_glow_key[0] = (uint32_t)r;
    s->moon_glow_key[1] = (uint32_t)il_q;
    const int side = 2 * r;
    const float peak = 0.14f * (float)il_q / 32.0f;
    for (int y = 0; y < side; y++) {
        uint8_t *row = &s->moon_glow[y * side];
        for (int x = 0; x < side; x++) {
            row[x] = sky_glow_a8(x, y, side, (float)SCN_MOON_R, peak, SCN_GLOW_DITHER);
        }
    }
    lv_image_dsc_t *dsc = &s->moon_glow_dsc;
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_A8;
    dsc->header.w = (uint32_t)side;
    dsc->header.h = (uint32_t)side;
    dsc->header.stride = (uint32_t)side;
    dsc->data_size = (uint32_t)(side * side);
    dsc->data = s->moon_glow;
}

/* Tile the strip across @p a (the strip is as tall as the area). */
static void draw_strip(lv_layer_t *layer, const lv_image_dsc_t *dsc, const lv_area_t *a)
{
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = dsc;
    d.tile = 1;
    lv_draw_image(layer, &d, a);
}

/* The sky over the whole object. Full night: strip 0 tiled once. Twilight:
 * the columns are walked in pairs (a strip's width is even, so every run starts
 * on the dither's own phase), each pair takes the strip of its distance from
 * the Sun's column, and each run of equal level is one tiled image. The runs
 * abut and the last one ends on the object's right edge, so the drawn width
 * is always the object width. Integer maths only. */
static void draw_sky(nina_sky_scene_t *s, lv_layer_t *layer, const lv_area_t *oa)
{
    const dl_t *dl = &s->dl;
    const int rows = (int)(oa->y2 - oa->y1 + 1);
    const int w = (int)(oa->x2 - oa->x1 + 1);
    sky_strips_bake(s, rows, dl);
    lv_area_t a = *oa;
    a.y2 = a.y1 + (int32_t)s->sky_dsc[0].header.h - 1;
    if (dl->sky_tw_q == 0) {
        draw_strip(layer, &s->sky_dsc[0], &a);
        return;
    }
    int R = 9 * s->size / 10;
    if (R < 1) {
        R = 1;
    }
    int run_x = 0, run_lv = -1;
    for (int x = 0; x < w; x += 2) {
        int dx = x + 1 - (int)dl->sun_x;
        if (dx < 0) {
            dx = -dx;
        }
        int lv = dx * SCN_SKY_LEVELS / R;
        if (lv > SCN_SKY_LEVELS - 1) {
            lv = SCN_SKY_LEVELS - 1;
        }
        if (lv != run_lv) {
            if (run_lv >= 0) {
                a.x1 = oa->x1 + run_x;
                a.x2 = oa->x1 + x - 1;
                draw_strip(layer, &s->sky_dsc[run_lv], &a);
            }
            run_x = x;
            run_lv = lv;
        }
    }
    if (run_lv >= 0) {
        a.x1 = oa->x1 + run_x;
        a.x2 = oa->x2;
        draw_strip(layer, &s->sky_dsc[run_lv], &a);
    }
}

/* Dashes of @p dash px every @p dash + @p gap px along a run of connected
 * segments, the phase carried across joins so the pattern is even round the
 * arc. LVGL's own dash fields apply to horizontal and vertical lines only.
 * A dash wholly below row @p ymax is not drawn (it is under the readout band). */
static void draw_dashes(const dctx_t *c, const seg_px_t *segs, int n, uint32_t color, int width,
                        lv_opa_t opa, float dash, float gap, int ymax)
{
    const float period = dash + gap;
    float p = 0.0f;   /* position inside the period at the current point */
    for (int i = 0; i < n; i++) {
        const float x1 = (float)segs[i].x1, y1 = (float)segs[i].y1;
        const float dx = (float)segs[i].x2 - x1, dy = (float)segs[i].y2 - y1;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f) {
            continue;
        }
        const float ux = dx / len, uy = dy / len;
        float t = 0.0f;
        while (t < len) {
            const bool on = p < dash;
            float step = (on ? dash : period) - p;
            if (step > len - t) {
                step = len - t;
            }
            if (on) {
                const int dy1 = (int)lroundf(y1 + uy * t), dy2 = (int)lroundf(y1 + uy * (t + step));
                if (dy1 - width <= ymax || dy2 - width <= ymax) {
                    /* A round end is two more fills per dash and shows only on a wide one. */
                    draw_line(c, (int)lroundf(x1 + ux * t), dy1, (int)lroundf(x1 + ux * (t + step)), dy2,
                              color, width, opa, width > 2);
                }
            }
            t += step;
            p += step;
            if (p >= period) {
                p -= period;
            }
        }
    }
}

static void draw_ring(const dctx_t *c, int cx, int cy, int r, uint32_t color, int width,
                      lv_opa_t opa, int start, int end, bool rounded)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    d.radius = (uint16_t)r;
    d.center.x = cx + c->ox;
    d.center.y = cy + c->oy;
    d.start_angle = (lv_value_precise_t)start;
    d.end_angle = (lv_value_precise_t)end;
    d.opa = opa;
    d.rounded = rounded ? 1 : 0;
    lv_draw_arc(c->layer, &d);
}

static void draw_dot(const dctx_t *c, int cx, int cy, int r, uint32_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a;
    a.x1 = cx - r + c->ox;
    a.y1 = cy - r + c->oy;
    a.x2 = cx + r + c->ox;
    a.y2 = cy + r + c->oy;
    lv_draw_rect(c->layer, &d, &a);
}

/* Opaque pixel-aligned rectangle x1..x2, y1..y2 inclusive (no anti-aliased edge). */
static void draw_fill(const dctx_t *c, int x1, int y1, int x2, int y2, uint32_t color)
{
    if (x2 < x1 || y2 < y1) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a;
    a.x1 = x1 + c->ox;
    a.y1 = y1 + c->oy;
    a.x2 = x2 + c->ox;
    a.y2 = y2 + c->oy;
    lv_draw_rect(c->layer, &d, &a);
}

static void draw_tri(const dctx_t *c, const int16_t *xs, const int16_t *ys,
                     uint32_t color, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.opa = opa;
    for (int i = 0; i < 3; i++) {
        d.p[i].x = (lv_value_precise_t)(xs[i] + c->ox);
        d.p[i].y = (lv_value_precise_t)(ys[i] + c->oy);
    }
    lv_draw_triangle(c->layer, &d);
}

/* Rectangle x1..x2, y1..y2 inclusive at @p opa: pixel-aligned, or with fully
 * rounded corners (a disc when it is square). */
static void draw_box(const dctx_t *c, int x1, int y1, int x2, int y2, uint32_t color,
                     lv_opa_t opa, bool round)
{
    if (x2 < x1 || y2 < y1) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    if (round) {
        d.radius = LV_RADIUS_CIRCLE;
    }
    lv_area_t a;
    a.x1 = x1 + c->ox;
    a.y1 = y1 + c->oy;
    a.x2 = x2 + c->ox;
    a.y2 = y2 + c->oy;
    lv_draw_rect(c->layer, &d, &a);
}

/* Opaque triangle from three x, y pairs; one with no area is skipped. */
static void draw_tri3(const dctx_t *c, const int16_t *p0, const int16_t *p1, const int16_t *p2,
                      uint32_t color)
{
    const int32_t cross = (int32_t)(p1[0] - p0[0]) * (int32_t)(p2[1] - p0[1]) -
                          (int32_t)(p1[1] - p0[1]) * (int32_t)(p2[0] - p0[0]);
    if (cross == 0) {
        return;
    }
    const int16_t xs[3] = { p0[0], p1[0], p2[0] };
    const int16_t ys[3] = { p0[1], p1[1], p2[1] };
    draw_tri(c, xs, ys, color, LV_OPA_COVER);
}

/* Sky decor, right after the sky: haze over the skyline, star patterns,
 * stars, the pole marker, the Moon. */
static void draw_decor(nina_sky_scene_t *s, const dctx_t *c)
{
    const dl_t *dl = &s->dl;
    const uint32_t *P = s->pal;

    /* haze: three bands above each run, stronger toward the skyline (the look
     * of three stacked 5 percent fills, without the overdraw), each cut to the
     * rows that show: on the panel and above the readout band */
    const bool band = s->opts.ground_band_top > 0 && s->opts.ground_band_top < s->size;
    const int vis_end = band ? s->opts.ground_band_top - 1 : s->size - 1;
    static const int16_t haze_top[3] = { SCN_HAZE_H0, SCN_HAZE_H1, SCN_HAZE_H2 };
    static const int16_t haze_end[3] = { SCN_HAZE_H1 + 1, SCN_HAZE_H2 + 1, 1 };
    static const uint8_t haze_pct[3] = { 5, 10, 15 };
    for (int i = 0; i < dl->n_haze; i++) {
        const col_px_t *k = &dl->haze[i];
        for (int b = 0; b < 3; b++) {
            const int y1 = (k->y - haze_top[b] > 0) ? k->y - haze_top[b] : 0;
            const int y2 = (k->y - haze_end[b] < vis_end) ? k->y - haze_end[b] : vis_end;
            draw_box(c, k->x1, y1, k->x2, y2, dl->haze_rgb, OPA_PCT(haze_pct[b]), false);
        }
    }

    /* star patterns */
    for (int i = 0; i < dl->n_figs; i++) {
        const seg_px_t *g = &dl->figs[i];
        draw_line(c, g->x1, g->y1, g->x2, g->y2, P[PAL_FIGURE], 1, dl->fig_a, false);
    }

    /* stars: a 1 or 2 px square when small, a disc otherwise, a soft halo
     * (2.6 times as wide, a tenth as strong) round the brightest */
    for (int i = 0; i < dl->n_stars; i++) {
        const star_px_t *q = &dl->stars[i];
        const uint32_t col = P[PAL_STAR_0 + ((q->tint <= 4) ? q->tint : 4)];
        const int d = q->r2;
        if (q->halo) {
            const int hd = (d * 13 + 2) / 5;
            const int hx = q->x - hd / 2, hy = q->y - hd / 2;
            draw_box(c, hx, hy, hx + hd - 1, hy + hd - 1, col, (lv_opa_t)(q->a / 10), true);
        }
        const int bx = q->x - d / 2, by = q->y - d / 2;
        draw_box(c, bx, by, bx + d - 1, by + d - 1, col, q->a, d > 2);
    }

    /* pole marker: a small ring (centre line radius 9) with four ticks */
    if (dl->pole_in) {
        const int px = dl->pole_x, py = dl->pole_y;
        const uint32_t col = P[PAL_POLE];
        draw_ring(c, px, py, 10, col, 2, dl->pole_a, 0, 360, false);
        draw_line(c, px - 14, py, px - 11, py, col, 2, dl->pole_a, false);
        draw_line(c, px + 11, py, px + 14, py, col, 2, dl->pole_a, false);
        draw_line(c, px, py - 14, px, py - 11, col, 2, dl->pole_a, false);
        draw_line(c, px, py + 11, px, py + 14, col, 2, dl->pole_a, false);
    }

    /* Moon: the glow picture in the glow colour, the dark disc, then the lit
     * part as a strip of opaque triangles between the limb and the terminator
     * (the dark disc is the underlay at their seams) */
    if (dl->moon_in) {
        if (dl->moon_glow_r > 0 && dl->moon_il_q > 0) {
            moon_glow_bake(s, dl->moon_glow_r, dl->moon_il_q);
            const int side = (int)s->moon_glow_dsc.header.w;
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = &s->moon_glow_dsc;
            d.recolor = lv_color_hex(P[PAL_MOON_GLOW]);
            lv_area_t a;
            a.x1 = c->ox + dl->moon_x - side / 2;
            a.y1 = c->oy + dl->moon_y - side / 2;
            a.x2 = a.x1 + side - 1;
            a.y2 = a.y1 + side - 1;
            lv_draw_image(c->layer, &d, &a);
        }
        draw_dot(c, dl->moon_x, dl->moon_y, SCN_MOON_R, P[PAL_MOON_DARK], LV_OPA_COVER);
        for (int i = 0; i < 8; i++) {
            draw_tri3(c, dl->moon_lit[0][i], dl->moon_lit[1][i], dl->moon_lit[0][i + 1],
                      P[PAL_MOON_LIT]);
            draw_tri3(c, dl->moon_lit[1][i], dl->moon_lit[1][i + 1], dl->moon_lit[0][i + 1],
                      P[PAL_MOON_LIT]);
        }
    }
}

/* The land above the level horizon row: the far hills (the outline itself)
 * as column rectangles with their smoothed edge, then the middle and the
 * near hills in front, the faint lit rim along the outline, and, when the
 * safety monitor reports unsafe, a dashed line along it. */
static void draw_land(const nina_sky_scene_t *s, const dctx_t *c)
{
    const dl_t *dl = &s->dl;
    const uint32_t *P = s->pal;
    /* Nothing is drawn under the readout band: it is painted over the land. */
    const bool band = s->opts.ground_band_top > 0 && s->opts.ground_band_top < s->size;
    const int vis_end = band ? s->opts.ground_band_top - 1 : s->size - 1;
    const int land_end = (dl->floor_y - 1 < vis_end) ? dl->floor_y - 1 : vis_end;
    for (int i = 0; i < dl->n_cols; i++) {
        const col_px_t *k = &dl->cols[i];
        draw_fill(c, k->x1, k->y, k->x2, land_end, P[PAL_X_LAND_FAR]);
    }
    for (int i = 0; i < dl->n_cols_cap; i++) {
        const col_px_t *k = &dl->cols_cap[i];
        draw_fill(c, k->x1, 0, k->x2, (k->y < vis_end) ? k->y : vis_end, P[PAL_X_LAND_FAR]);
    }
    for (int i = 0; i < dl->n_hz; i++) {
        const seg_px_t *g = &dl->hz[i];
        if (g->y1 - SCN_EDGE_W > vis_end && g->y2 - SCN_EDGE_W > vis_end) {
            continue;
        }
        draw_line(c, g->x1, g->y1, g->x2, g->y2, P[PAL_X_LAND_FAR], SCN_EDGE_W, LV_OPA_COVER, true);
    }
    for (int i = 0; i < dl->n_cols_mid; i++) {
        const col_px_t *k = &dl->cols_mid[i];
        draw_fill(c, k->x1, k->y, k->x2, land_end, P[PAL_X_LAND_MID]);
    }
    for (int i = 0; i < dl->n_cols_near; i++) {
        const col_px_t *k = &dl->cols_near[i];
        draw_fill(c, k->x1, k->y, k->x2, land_end, P[PAL_LAND]);
    }
    if (dl->rim_in) {
        for (int i = 0; i < dl->n_hz; i++) {
            const seg_px_t *g = &dl->hz[i];
            if (g->y1 - 2 > vis_end && g->y2 - 2 > vis_end) {
                continue;
            }
            draw_line(c, g->x1, g->y1 - 1, g->x2, g->y2 - 1, dl->rim_rgb, 2, OPA_PCT(28), false);
        }
    }
    if (dl->unsafe) {
        draw_dashes(c, dl->hz, dl->n_hz, P[PAL_UNSAFE], 3, OPA_PCT(80), 10.0f, 8.0f, vis_end);
    }
}

/* Compass labels just above the ground row, each over a short tick on it,
 * and the minor ticks between them. project_compass() already dropped every
 * label that would leave the glass. */
static void draw_compass(const nina_sky_scene_t *s, const dctx_t *c)
{
    static const uint8_t label_pct[3] = { 40, 46, 66 };
    static const uint8_t tick_pct[3] = { 42, 42, 65 };
    static const uint8_t tick_h[3] = { 6, 6, 9 };
    const dl_t *dl = &s->dl;
    const uint32_t col = s->pal[PAL_FLIP_LIGHT];
    if (dl->n_compass > 0) {
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.color = lv_color_hex(col);
        ld.align = LV_TEXT_ALIGN_CENTER;
        /* The draw task runs after this call returns, so the text must be
         * copied: buf below is a stack local. */
        ld.text_local = 1;
        for (int i = 0; i < dl->n_compass; i++) {
            const compass_px_t *m = &dl->compass[i];
            const int rank = (m->rank <= 2) ? m->rank : 2;
            const lv_font_t *font = compass_font(rank);
            const int lh = lv_font_get_line_height(font);
            char buf[16];
            lv_area_t ta;
            ta.x1 = c->ox + m->x - m->hw;
            ta.x2 = c->ox + m->x + m->hw;
            ta.y1 = c->oy + dl->floor_y - 8 - lh;
            ta.y2 = c->oy + dl->floor_y - 8;
            ld.font = font;
            ld.opa = OPA_PCT(label_pct[rank]);
            ld.text = sky_compass_label(m->az, buf, sizeof(buf), NULL);
            lv_draw_label(c->layer, &ld, &ta);
            draw_line(c, m->x, dl->floor_y - tick_h[rank], m->x, dl->floor_y - 1, col, 2,
                      OPA_PCT(tick_pct[rank]), false);
        }
    }
    for (int i = 0; i < dl->n_compass_minor; i++) {
        const int x = dl->compass_minor[i];
        draw_line(c, x, dl->floor_y - 4, x, dl->floor_y - 1, col, 2, OPA_PCT(28), false);
    }
}

/* The one ring round the target (LVGL angles: 270 = twelve o'clock,
 * clockwise). Outline: one arc when the guiding is quiet, else the wavy
 * polyline in round-capped opaque lines. Exposure: a soft halo arc, the solid
 * arc (or the first outline segments when wavy) and a bright tip. All centre
 * lines sit on radius SCN_RING_R; lv_draw_arc takes the outer radius. */
static void draw_target_ring(const nina_sky_scene_t *s, const dctx_t *c)
{
    const dl_t *dl = &s->dl;
    const bool wavy = (dl->n_ring_pts == SCN_RING_N + 1);
    if (dl->guide_in) {
        if (!wavy) {
            draw_ring(c, dl->target_x, dl->target_y, SCN_RING_R + 1, dl->ring_rgb, dl->ring_w,
                      LV_OPA_COVER, 0, 360, false);
        } else {
            for (int i = 0; i < SCN_RING_N; i++) {
                draw_line(c, dl->ring_pts[i][0], dl->ring_pts[i][1], dl->ring_pts[i + 1][0],
                          dl->ring_pts[i + 1][1], dl->ring_rgb, dl->ring_w, LV_OPA_COVER, true);
            }
        }
    }
    if (!dl->ring_in) {
        return;
    }
    if (dl->ring_end > 0) {
        draw_ring(c, dl->ring_x, dl->ring_y, SCN_RING_R + 4, dl->ring_color, 9, OPA_PCT(13),
                  270, 270 + dl->ring_end, false);
    }
    if (!wavy) {
        if (dl->ring_end > 0) {
            draw_ring(c, dl->ring_x, dl->ring_y, SCN_RING_R + 2, dl->ring_color, 4, LV_OPA_COVER,
                      270, 270 + dl->ring_end, true);
        }
    } else {
        const int n = (dl->ring_arc_n < SCN_RING_N) ? dl->ring_arc_n : SCN_RING_N;
        for (int i = 0; i < n; i++) {
            draw_line(c, dl->ring_pts[i][0], dl->ring_pts[i][1], dl->ring_pts[i + 1][0],
                      dl->ring_pts[i + 1][1], dl->ring_color, 4, LV_OPA_COVER, true);
        }
        draw_line(c, dl->ring_pts[n][0], dl->ring_pts[n][1], dl->ring_tip_x, dl->ring_tip_y,
                  dl->ring_color, 4, LV_OPA_COVER, true);
    }
    draw_dot(c, dl->ring_tip_x, dl->ring_tip_y, 4, dl->ring_tip_rgb, LV_OPA_COVER);
}

/* The telescope: its contact shadow (two flat pills), then the primitive
 * list in the order the tessellator gave it, through the mapped tone table. */
static void draw_rig(const nina_sky_scene_t *s, const dctx_t *c)
{
    const dl_t *dl = &s->dl;
    if (dl->shadow_in) {
        const int x = dl->shadow_x, y = dl->shadow_y;
        const int rx = dl->shadow_rx, ry = dl->shadow_ry;
        const int ix = rx * 9 / 16, iy = ry * 9 / 16;
        draw_box(c, x - rx, y - ry, x + rx, y + ry, s->pal[PAL_SHADOW], OPA_PCT(22), true);
        draw_box(c, x - ix, y - iy, x + ix, y + iy, s->pal[PAL_SHADOW], OPA_PCT(38), true);
    }
    const int n = (dl->n_rig < SKY_RIG_PRIM_MAX) ? dl->n_rig : SKY_RIG_PRIM_MAX;
    for (int i = 0; i < n; i++) {
        const rig_px_t *q = &dl->rig[i];
        const uint32_t col = s->rig_tone[(q->tone < SKY_TONE_N) ? q->tone : SKY_TONE_EDGE];
        if (q->kind == SKY_PRIM_TRI) {
            const int16_t p0[2] = { q->x[0], q->y[0] };
            const int16_t p1[2] = { q->x[1], q->y[1] };
            const int16_t p2[2] = { q->x[2], q->y[2] };
            draw_tri3(c, p0, p1, p2, col);
        } else if (q->kind == SKY_PRIM_LINE) {
            draw_line(c, q->x[0], q->y[0], q->x[1], q->y[1], col, q->w, LV_OPA_COVER, q->round != 0);
        } else {
            draw_dot(c, q->x[0], q->y[0], q->w, col, LV_OPA_COVER);
        }
    }
}

static void draw_cb(lv_event_t *e)
{
    /* The strips and the Moon glow picture are the only things the draw pass
     * writes (their bake caches). */
    nina_sky_scene_t *ms = (nina_sky_scene_t *)lv_event_get_user_data(e);
    const nina_sky_scene_t *s = ms;
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!s || !layer) {
        return;
    }
    lv_area_t oa;
    lv_obj_get_coords(s->obj, &oa);
    dctx_t c = { layer, oa.x1, oa.y1 };
    const dl_t *dl = &s->dl;
    const uint32_t *P = s->pal;
    const int bottom = s->size;

    /* 1 sky: deep blue, lightening down to the deepest dip of the outline,
     * as dithered strips tiled over the whole object; then the sky decor */
    draw_sky(ms, layer, &oa);
    draw_decor(ms, &c);

    /* 2 travelled path: thick dashes (imaged) */
    draw_dashes(&c, dl->trav, dl->n_trav, P[PAL_AMBER], 5, LV_OPA_COVER, 9.0f, 9.0f, bottom);
    /* 3 path to come: thin dashes (not yet imaged) */
    draw_dashes(&c, dl->fut, dl->n_fut, P[PAL_AMBER], 2, LV_OPA_COVER, 9.0f, 9.0f, bottom);
    /* 3b time left on the target: a solid stretch over the dashes, with an
     * end tick across the path */
    for (int i = 0; i < dl->n_left; i++) {
        const seg_px_t *g = &dl->left[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, P[PAL_X_LEFT], 3, LV_OPA_COVER, true);
    }
    if (dl->left_tick_in) {
        draw_line(&c, dl->left_tick.x1, dl->left_tick.y1, dl->left_tick.x2, dl->left_tick.y2,
                  P[PAL_X_LEFT_TICK], 3, LV_OPA_COVER, true);
    }
    /* 4 beads, and one opening ring round the newest when a picture finishes */
    for (int i = 0; i < dl->n_beads; i++) {
        draw_dot(&c, dl->beads[i].x, dl->beads[i].y, 4, dl->beads[i].color, LV_OPA_COVER);
    }
    if (dl->flare_in) {
        draw_ring(&c, dl->flare_x, dl->flare_y, dl->flare_r, dl->flare_rgb, 2, dl->flare_a,
                  0, 360, false);
    }
    /* 5 rise and set */
    if (dl->rise_in) {
        if (dl->rise_fill) {
            draw_dot(&c, dl->rise_x, dl->rise_y, 7, P[PAL_AMBER], LV_OPA_COVER);
        } else {
            draw_ring(&c, dl->rise_x, dl->rise_y, 8, P[PAL_AMBER], 3, LV_OPA_COVER, 0, 360, false);
        }
    }
    if (dl->set_in) {
        draw_ring(&c, dl->set_x, dl->set_y, 8, P[PAL_AMBER], 3, LV_OPA_COVER, 0, 360, false);
    }
    /* 6 flip */
    if (dl->flip_in) {
        draw_line(&c, dl->flip.x1, dl->flip.y1, dl->flip.x2, dl->flip.y2,
                  P[PAL_FLIP_DARK], 9, LV_OPA_COVER, true);
        draw_line(&c, dl->flip.x1, dl->flip.y1, dl->flip.x2, dl->flip.y2,
                  P[PAL_FLIP_LIGHT], 4, LV_OPA_COVER, true);
    }
    /* 7 target: the light gathered so far as three stacked faint discs, then
     * the dot, which grows and dims as the stars swell (HFR) */
    if (dl->target_in) {
        if (dl->gather_a > 0) {
            for (int i = 1; i <= 3; i++) {
                draw_dot(&c, dl->target_x, dl->target_y, dl->gather_r * i / 3, P[PAL_X_GATHER],
                         dl->gather_a);
            }
        }
        draw_dot(&c, dl->target_x, dl->target_y, dl->tgt_r, P[PAL_AMBER], dl->tgt_a);
        if (dl->tgt_core_a > 0) {
            draw_dot(&c, dl->target_x, dl->target_y, 2, P[PAL_TARGET_C], dl->tgt_core_a);
        }
    }
    /* 8 land: the yard floor below the level horizon row, the obstructions
     * above it as column rectangles, their smoothed edge, the grass, then the
     * readout band (the floor's near colour, so it continues) */
    const bool band = s->opts.ground_band_top > 0 && s->opts.ground_band_top < bottom;
    const int floor_end = band ? s->opts.ground_band_top - 1 : bottom - 1;
    if (dl->floor_y <= floor_end) {
        const int rows = floor_end - dl->floor_y + 1;
        strip_bake(ms->strip_floor, &ms->strip_floor_dsc, ms->strip_floor_key, rows, rows - 1,
                   P[PAL_FLOOR_FAR], P[PAL_FLOOR_NEAR], dl->haze_rgb, SCN_FLOOR_GLOW_H);
        lv_area_t a = oa;
        a.y1 = oa.y1 + dl->floor_y;
        a.y2 = a.y1 + (int32_t)s->strip_floor_dsc.header.h - 1;
        draw_strip(layer, &s->strip_floor_dsc, &a);
    }
    draw_land(s, &c);
    for (int i = 0; i < dl->n_grass; i++) {
        const grass_px_t *g = &dl->grass[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, P[PAL_GRASS], g->w, LV_OPA_COVER, g->w > 2);
    }
    if (band) {
        draw_fill(&c, 0, s->opts.ground_band_top, bottom - 1, bottom - 1, P[PAL_FLOOR_NEAR]);
    }
    /* 8b compass letters and ticks on the ground row */
    draw_compass(s, &c);
    /* 8c sweep arc of a pier-side change, behind the telescope */
    for (int i = 0; i < dl->n_sweep && i < SCN_SWEEP_MAX; i++) {
        const trail_px_t *t = &dl->sweep[i];
        draw_line(&c, t->x1, t->y1, t->x2, t->y2, dl->accent, t->w, t->a, false);
    }
    /* 9 telescope: the contact shadow, then the solid model */
    draw_rig(s, &c);
    /* 10 slew trail, then the beam, in the filter accent */
    for (int i = 0; i < dl->n_trail && i < SCN_TRAIL_MAX - 1; i++) {
        const trail_px_t *t = &dl->trail[i];
        draw_line(&c, t->x1, t->y1, t->x2, t->y2, dl->accent, t->w, t->a, false);
    }
    /* The soft glow is a 7 px blended diagonal over the beam's whole box:
     * left out on a thin glide frame. */
    for (int i = 0; i < dl->n_beam && !dl->lite; i++) {
        const seg_px_t *g = &dl->beam[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, dl->accent, 7, OPA_PCT(12), true);
    }
    for (int i = 0; i < dl->n_beam; i++) {
        const seg_px_t *g = &dl->beam[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, dl->accent, 2, OPA_PCT(72), false);
    }
    /* 10b finish pulse: a soft point running down the beam, then a ring
     * opening at the tube mouth */
    if (dl->pulse_stage == 1) {
        for (int i = 3; i >= 1; i--) {
            draw_dot(&c, dl->pulse_x, dl->pulse_y, dl->pulse_r * i / 3, dl->pulse_rgb, dl->pulse_a);
        }
        draw_dot(&c, dl->pulse_x, dl->pulse_y, 4, dl->pulse_rgb, OPA_PCT(90));
    } else if (dl->pulse_stage == 2) {
        draw_ring(&c, dl->pulse_x, dl->pulse_y, dl->pulse_r, dl->pulse_rgb, 3, dl->pulse_a,
                  0, 360, false);
    }
    /* 11 the ring: guiding outline and exposure arc */
    draw_target_ring(s, &c);
    /* 12 edge arrows */
    if (dl->tarrow_in) {
        draw_tri(&c, dl->tarrow.x, dl->tarrow.y, P[PAL_AMBER], LV_OPA_COVER);
    }
    if (dl->aarrow_in) {
        draw_tri(&c, dl->aarrow.x, dl->aarrow.y, P[PAL_CYAN], LV_OPA_COVER);
    }
    /* 13 the word Unsafe, top centre */
    if (dl->unsafe && dl->unsafe_w > 0) {
        lv_draw_label_dsc_t ld;
        lv_area_t ta;
        lv_draw_label_dsc_init(&ld);
        ld.font = &lv_font_montserrat_22;
        ld.color = lv_color_hex(P[PAL_UNSAFE]);
        ld.opa = OPA_PCT(85);
        ld.align = LV_TEXT_ALIGN_CENTER;
        ld.text = SCN_UNSAFE_TXT;
        ld.text_local = 1;
        ta.x1 = c.ox + dl->unsafe_x;
        ta.x2 = ta.x1 + dl->unsafe_w;
        ta.y1 = c.oy + dl->unsafe_y;
        ta.y2 = ta.y1 + dl->unsafe_h;
        lv_draw_label(layer, &ld, &ta);
    }
}
