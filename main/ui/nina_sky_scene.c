/**
 * @file nina_sky_scene.c
 * @brief The sky scene renderer shared by NINA page layouts 8 and 9.
 *
 * Renderer choice (plan D1): ONE full-panel LVGL object whose
 * LV_EVENT_DRAW_MAIN_END callback issues lv_draw_rect / lv_draw_line /
 * lv_draw_arc / lv_draw_triangle, the ADS-B Sky Dome pattern (nina_adsb.c).
 * All trigonometry runs in nina_sky_scene_update() and in the animation tick,
 * which build a fixed-size draw list of integer panel coordinates; the draw
 * callback reads only that list and the palette.
 *
 * ponytail: one LVGL object redrawn from draw-event primitives; ceiling is
 * the frame rate during the 2.4 s fly-around (every tick repaints the full
 * panel). Upgrade path: render into an own RGB565 PSRAM buffer and show it as
 * an lv_image.
 *
 * Invalidate discipline (the panel is full refresh): the draw list is hashed
 * (FNV-1a 32) after every re-projection and the object is invalidated only
 * when the hash changed. The 33 ms timer runs only while the camera flies, the
 * displayed aim is still settling, or the mount slews or flips.
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
#include "esp_timer.h"

#include "app_config.h"
#include "display_defs.h"
#include "themes.h"
#include "ui_helpers.h"

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
    PAL_TUBE,
    PAL_TUBE_HI,
    PAL_MOUNT,
    PAL_WEIGHT,
    PAL_PIER,
    PAL_CAMERA,
    PAL_EDGE,          /* thin dark edge round every telescope part */
    PAL_N,
};

static const uint32_t PAL_RAW[PAL_N] = {
    0x05091a, 0x1d3458, 0xf4b860, 0x0b1019, 0xe9e6dc,
    0xfff3dc, 0x030507, 0x0e1a13, 0x070d09, 0x1c3324,
    0x6fd3e0, 0xc4cad3, 0xe4e8ee, 0x8a919b, 0x7a818b,
    0x4b525c, 0x3a4049, 0x05070a,
};

/* ---- the draw list ---------------------------------------------------------- */

typedef struct { int16_t x1, y1, x2, y2; } seg_px_t;
typedef struct { int16_t x, y; uint32_t color; } bead_px_t;
typedef struct { int16_t x1, y1, x2, y2, w; uint8_t part, pad; } cap_px_t;
typedef struct { int16_t x[3], y[3]; } tri_px_t;
typedef struct { int16_t x1, x2, y; } col_px_t;   /* land rectangle x1..x2, y..floor_y-1 */
typedef struct { int16_t x1, y1, x2, y2; uint8_t w, pad; } grass_px_t;
typedef struct { int16_t x; uint16_t az; } compass_px_t;   /* label centre x on the horizon row */

/* Built from scratch (memset 0, then field writes) on every projection, so the
 * hash over its bytes is deterministic. Panel coordinates relative to the
 * scene object, which sits at (0,0) of the page root. */
typedef struct {
    uint16_t  n_trav, n_fut, n_hz, n_beads, n_caps, n_beam, n_cols, n_grass, n_compass;
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
    cap_px_t  caps[SKY_RIG_CAPSULES];
    seg_px_t  flip;
    tri_px_t  tarrow, aarrow;
    int16_t   target_x, target_y;
    int16_t   rise_x, rise_y, set_x, set_y;
    int16_t   ring_x, ring_y, ring_end;
    int16_t   tarrow_x, tarrow_y;
    uint8_t   target_in, rise_in, rise_fill, set_in;
    uint8_t   flip_in, ring_in, tarrow_in, aarrow_in;
    uint32_t  ring_color;
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
    float   goal_lat;
    sky_v3_t fit_dirs[SKY_PATH_N + 2];
    sky_v3_t fit_pts[SKY_RIG_FIT_N];
    sky_capsule_t caps[SKY_RIG_CAPSULES];

    /* horizon scratch for project() (kept here, not on the stack) */
    float   hz_x[SCN_HZ_PTS], hz_y[SCN_HZ_PTS];
    bool    hz_ok[SCN_HZ_PTS];
    float   col_top[SCN_COL_MAX];

    /* exposure ring input */
    float    exp_frac;
    uint32_t exp_color;

    /* Sky and floor gradients as 2 px wide RGB565 strips with a 2x2 ordered
     * dither, tiled across the panel by lv_draw_image: the 16-bit panel shows
     * a plain dark gradient as 40 px bands and LVGL 9 has no dither of its own.
     * Re-baked only when the rows, the span or the colours change. */
    uint16_t strip_sky[2 * SCN_STRIP_MAX];
    uint16_t strip_floor[2 * SCN_STRIP_MAX];
    lv_image_dsc_t strip_sky_dsc;
    lv_image_dsc_t strip_floor_dsc;
    uint32_t strip_sky_key[4];
    uint32_t strip_floor_key[4];

    /* output */
    uint32_t pal[PAL_N];
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
static void     tick_cb(lv_timer_t *t);
static void     frame_init(nina_sky_scene_t *s);
static void     palette_build(nina_sky_scene_t *s);
static uint32_t pal_map(int gb, uint32_t raw);
static uint32_t pal_red_only(uint32_t raw);
static void     goal_compute(nina_sky_scene_t *s, bool want_work);
static void     project(nina_sky_scene_t *s);
static void     ring_fill(nina_sky_scene_t *s);
static void     commit(nina_sky_scene_t *s, bool force);
static void     anchors_build(nina_sky_scene_t *s, nina_sky_anchor_t *out);
static void     timer_kick(nina_sky_scene_t *s);
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
                        lv_opa_t opa, float dash, float gap);
static void strip_bake(uint16_t *buf, lv_image_dsc_t *dsc, uint32_t *key, int rows, int span,
                       uint32_t c0, uint32_t c1);
static void draw_strip(lv_layer_t *layer, const lv_image_dsc_t *dsc, const lv_area_t *a);

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

    s->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s->obj);
    lv_obj_set_size(s->obj, s->size, s->size);
    lv_obj_set_pos(s->obj, 0, 0);
    lv_obj_remove_flag(s->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s->obj, draw_cb, LV_EVENT_DRAW_MAIN_END, s);
    lv_obj_add_event_cb(s->obj, delete_cb, LV_EVENT_DELETE, s);

    s->timer = lv_timer_create(tick_cb, SCN_TICK_MS, s);
    if (s->timer) {
        lv_timer_pause(s->timer);
    }

    project(s);
    s->dl_hash = fnv1a(&s->dl, sizeof(s->dl));
    anchors_build(s, s->anch);
    return s;
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
    heap_caps_free(s);
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
    if (d->site_valid) {
        s->have_site = true;
        s->lat = d->site_lat_deg;
        s->lon = d->site_lon_deg;
    } else if (cfg->weather_lat != 0.0f || cfg->weather_lon != 0.0f) {
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
    if (d->lst_valid) {
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
    s->have_target = d->target_coord_valid && s->have_site && s->have_lst;
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
    s->pointing_valid = d->mount_pointing_valid;
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
    in->flip_known = d->flip_valid && d->flip_minutes < 720.0f;
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
    s->prev_want_work = want_work;
    s->prev_horizon_valid = s->horizon_valid;
    s->prev_have_site = s->have_site;
    if (s->goal_pending && in->activity != SKY_ACT_FLIPPING) {
        s->goal_pending = false;
        goal_compute(s, want_work);
    }

    commit(s, force);
    timer_kick(s);
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
        const float gbt = (float)s->opts.ground_band_top;
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
            const int np = sky_rig_fit_points(&pose, s->fit_pts);
            sky_cam_fit(&goal, yaw, &s->frame, s->fit_dirs, n, s->fit_pts, np,
                        SKY_PITCH_MAX_DEG, gbt);
            s->cam_mode = CAM_MODE_WORK;
        } else {
            const sky_pose_t rest = sky_mount_rest_pose(s->lat);
            const int np = sky_rig_fit_points(&rest, s->fit_pts);
            s->fit_dirs[0] = sky_pole(s->lat);
            sky_cam_fit(&goal, sky_cam_home_yaw(s->lat), &s->frame, s->fit_dirs, 1,
                        s->fit_pts, np, SKY_PITCH_MAX_DEG, gbt);
            s->cam_mode = CAM_MODE_HOME;
        }
        s->cam_placeholder = false;
        s->goal_lat = s->lat;
    }
    s->cam_goal = goal;

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

static void timer_kick(nina_sky_scene_t *s)
{
    if (!s->timer) {
        return;
    }
    const bool need = s->flying || !s->aim_settled || s->slewing || s->flipping;
    if (need && !s->timer_running) {
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

    commit(s, false);

    if (s->ticks_since_update < UINT16_MAX) {
        s->ticks_since_update++;
    }
    if (!s->flying && s->aim_settled && (s->slewing || s->flipping) &&
        s->ticks_since_update > 150) {
        s->slewing = false;
        s->flipping = false;
    }

    if (!s->flying && s->aim_settled && !s->slewing && !s->flipping) {
        lv_timer_pause(t);
        s->timer_running = false;
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
            dl->target_in = 1;
            dl->target_x = to_px(x);
            dl->target_y = to_px(y);
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
            /* Lower each column top whose centre this segment spans. */
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
            for (int c = c0; c <= c1; c++) {
                const float xc = (float)(c * SCN_COL_W) + 0.5f * (float)SCN_COL_W;
                const float k = (fabsf(hdx) > 1e-6f) ? (xc - hx0) / hdx : 0.0f;
                const float yc = hy0 + (hy1 - hy0) * k;
                if (yc < s->col_top[c]) {
                    s->col_top[c] = yc;
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

        /* Column tops to rectangles; neighbours within SCN_COL_MERGE px share one. */
        int16_t deep = -1, rmin = 0, rmax = 0;
        bool in_run = false;
        for (int c = 0; c < ncol; c++) {
            int16_t tt = dl->floor_y;
            if (s->col_top[c] < 1e8f) {
                float v = s->col_top[c];
                if (v < 0.0f) {
                    v = 0.0f;
                }
                if (v > fy) {
                    v = fy;
                }
                tt = to_px(v);
                if (tt > dl->floor_y) {
                    tt = dl->floor_y;
                }
            }
            if (tt >= dl->floor_y) {
                in_run = false;
                continue;
            }
            if (tt > deep) {
                deep = tt;
            }
            int cx2 = c * SCN_COL_W + SCN_COL_W - 1;
            if (cx2 > s->size - 1) {
                cx2 = s->size - 1;
            }
            if (in_run) {
                const int16_t lo = (int16_t)((tt < rmin) ? tt : rmin);
                const int16_t hi = (int16_t)((tt > rmax) ? tt : rmax);
                if (hi - lo <= SCN_COL_MERGE) {
                    col_px_t *run = &dl->cols[dl->n_cols - 1];
                    run->x2 = (int16_t)cx2;
                    run->y = lo;
                    rmin = lo;
                    rmax = hi;
                    continue;
                }
            }
            if (dl->n_cols >= SCN_COL_MAX) {
                in_run = false;
                continue;
            }
            col_px_t *nr = &dl->cols[dl->n_cols++];
            nr->x1 = (int16_t)(c * SCN_COL_W);
            nr->x2 = (int16_t)cx2;
            nr->y = tt;
            rmin = tt;
            rmax = tt;
            in_run = true;
        }
        dl->glow_y = (int16_t)((deep > 0) ? deep : dl->floor_y);

        /* Compass marks on the level horizon row (a straight horizontal line
         * for a camera with no roll): the step follows the zoom so labels
         * never crowd, the labels are drawn above floor_y. */
        {
            const int step = sky_compass_step(cam.focal_px * tanf(SKY_DEG2RAD), SCN_COMPASS_MIN_PX);
            for (int a = 0; a < 360 && dl->n_compass < SCN_COMPASS_MAX; a += step) {
                float mx = 0.0f, my = 0.0f;
                if (!proj_dir(&cam, sky_altaz_to_vec(0.0f, (float)a), &mx, &my)) {
                    continue;
                }
                if (mx < 0.0f || mx > sz) {
                    continue;
                }
                compass_px_t *m = &dl->compass[dl->n_compass++];
                m->x = to_px(mx);
                m->az = (uint16_t)a;
            }
        }
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

    /* Telescope and beam (needs a site for its latitude). */
    if (s->have_site) {
        sky_pose_t pose;
        const bool live = !s->at_park && s->pointing_valid && s->have_aim_disp;
        if (!live) {
            /* ponytail: the parked pose is drawn snapped, not animated from the last pointing. */
            pose = sky_mount_rest_pose(s->lat);
        } else {
            /* ponytail: a change of side of pier jumps the tube to the other side in
             * one frame; NINA's reported pointing carries the slew itself. */
            pose = sky_mount_pose(s->lat, s->aim_disp, (sky_pier_t)s->pier,
                                  s->have_target ? s->ha_now : 1.0f);
        }
        const int nc = sky_rig_capsules(&pose, s->caps);
        float zm[SKY_RIG_CAPSULES] = { 0 };
        int nk = 0;
        int16_t bx1 = INT16_MAX, by1 = INT16_MAX, bx2 = INT16_MIN, by2 = INT16_MIN;
        for (int i = 0; i < nc && i < SKY_RIG_CAPSULES; i++) {
            const sky_v3_t ca = sky_cam_point(&cam, s->caps[i].a);
            const sky_v3_t cb = sky_cam_point(&cam, s->caps[i].b);
            float ax = 0.0f, ay = 0.0f, bxf = 0.0f, byf = 0.0f;
            if (!sky_cam_screen(&cam, ca, &ax, &ay) || !sky_cam_screen(&cam, cb, &bxf, &byf)) {
                continue;
            }
            if (!clip_seg(&ax, &ay, &bxf, &byf, -SCN_CLIP_PAD, (float)s->size + SCN_CLIP_PAD)) {
                continue;
            }
            const float z = 0.5f * (ca.z + cb.z);
            float w = 2.0f * s->caps[i].r * cam.focal_px / z;
            if (w < 3.0f) {
                w = 3.0f;
            }
            if (w > 60.0f) {
                w = 60.0f;
            }
            cap_px_t c;
            c.x1 = to_px(ax);
            c.y1 = to_px(ay);
            c.x2 = to_px(bxf);
            c.y2 = to_px(byf);
            c.w = to_px(w);
            c.part = s->caps[i].part;
            c.pad = 0;
            /* insertion sort, far (large z) first */
            int j = nk;
            while (j > 0 && zm[j - 1] < z) {
                zm[j] = zm[j - 1];
                dl->caps[j] = dl->caps[j - 1];
                j--;
            }
            zm[j] = z;
            dl->caps[j].x1 = c.x1;
            dl->caps[j].y1 = c.y1;
            dl->caps[j].x2 = c.x2;
            dl->caps[j].y2 = c.y2;
            dl->caps[j].w = c.w;
            dl->caps[j].part = c.part;
            dl->caps[j].pad = 0;
            nk++;
            const int16_t hw = (int16_t)(c.w / 2);
            const int16_t mnx = (int16_t)((c.x1 < c.x2 ? c.x1 : c.x2) - hw);
            const int16_t mxx = (int16_t)((c.x1 > c.x2 ? c.x1 : c.x2) + hw);
            const int16_t mny = (int16_t)((c.y1 < c.y2 ? c.y1 : c.y2) - hw);
            const int16_t mxy = (int16_t)((c.y1 > c.y2 ? c.y1 : c.y2) + hw);
            if (mnx < bx1) {
                bx1 = mnx;
            }
            if (mny < by1) {
                by1 = mny;
            }
            if (mxx > bx2) {
                bx2 = mxx;
            }
            if (mxy > by2) {
                by2 = mxy;
            }
        }
        dl->n_caps = (uint16_t)nk;

        if (nk > 0) {
            s->rig_box.x1 = bx1;
            s->rig_box.y1 = by1;
            s->rig_box.x2 = bx2;
            s->rig_box.y2 = by2;
        } else {
            memset(&s->rig_box, 0, sizeof(s->rig_box));
        }

        if (live) {
            const sky_v3_t front = sky_rig_tube_front(&pose);
            const sky_v3_t ac = sky_cam_dir(&cam, s->aim_disp);
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
                        const sky_v3_t pw = sky_v3_add(front, sky_v3_scale(s->aim_disp, KS[i]));
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
    } else {
        memset(&s->rig_box, 0, sizeof(s->rig_box));
    }

    ring_fill(s);
}

static void ring_fill(nina_sky_scene_t *s)
{
    dl_t *dl = &s->dl;
    const bool show = s->info.on_target && s->exposing && s->exp_frac >= 0.0f && dl->target_in;
    if (show) {
        float f = s->exp_frac;
        if (f > 1.0f) {
            f = 1.0f;
        }
        dl->ring_in = 1;
        dl->ring_x = dl->target_x;
        dl->ring_y = dl->target_y;
        dl->ring_end = (int16_t)(f * 360.0f);
        dl->ring_color = s->exp_color;
    } else {
        dl->ring_in = 0;
        dl->ring_x = 0;
        dl->ring_y = 0;
        dl->ring_end = 0;
        dl->ring_color = 0;
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
    if (dl->n_caps > 0) {
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
    if (dl->target_in && in_box(box, SCN_RING_R, dl->target_x, dl->target_y)) {
        return true;
    }
    if (dl->flip_in && (in_box(box, 4, dl->flip.x1, dl->flip.y1) ||
                        in_box(box, 4, dl->flip.x2, dl->flip.y2))) {
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

/* A 2 x @p rows strip whose row i is c0 blended toward c1 by min(i, span) /
 * span, dithered. Skipped when the key (rows, span, c0, c1) is unchanged. */
static void strip_bake(uint16_t *buf, lv_image_dsc_t *dsc, uint32_t *key, int rows, int span,
                       uint32_t c0, uint32_t c1)
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
    if (key[0] == (uint32_t)rows && key[1] == (uint32_t)span && key[2] == c0 && key[3] == c1 &&
        dsc->data != NULL) {
        return;
    }
    key[0] = (uint32_t)rows;
    key[1] = (uint32_t)span;
    key[2] = c0;
    key[3] = c1;
    const float r0 = (float)((c0 >> 16) & 0xffu), g0 = (float)((c0 >> 8) & 0xffu), b0 = (float)(c0 & 0xffu);
    const float r1 = (float)((c1 >> 16) & 0xffu), g1 = (float)((c1 >> 8) & 0xffu), b1 = (float)(c1 & 0xffu);
    for (int y = 0; y < rows; y++) {
        const float k = (y >= span) ? 1.0f : (float)y / (float)span;
        const uint32_t rgb = ((uint32_t)(r0 + (r1 - r0) * k + 0.5f) << 16) |
                             ((uint32_t)(g0 + (g1 - g0) * k + 0.5f) << 8) |
                             (uint32_t)(b0 + (b1 - b0) * k + 0.5f);
        buf[2 * y]     = dither565(rgb, 0, y);
        buf[2 * y + 1] = dither565(rgb, 1, y);
    }
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = 2;
    dsc->header.h = (uint32_t)rows;
    dsc->header.stride = 4;
    dsc->data_size = (uint32_t)rows * 4u;
    dsc->data = (const uint8_t *)buf;
    /* No image cache to drop: this build runs with LV_CACHE_DEF_SIZE 0, so a
     * raw RGB565 descriptor is read straight from here on every draw. */
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

/* Dashes of @p dash px every @p dash + @p gap px along a run of connected
 * segments, the phase carried across joins so the pattern is even round the
 * arc. LVGL's own dash fields apply to horizontal and vertical lines only. */
static void draw_dashes(const dctx_t *c, const seg_px_t *segs, int n, uint32_t color, int width,
                        lv_opa_t opa, float dash, float gap)
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
                draw_line(c, (int)lroundf(x1 + ux * t), (int)lroundf(y1 + uy * t),
                          (int)lroundf(x1 + ux * (t + step)), (int)lroundf(y1 + uy * (t + step)),
                          color, width, opa, true);
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

static void draw_cb(lv_event_t *e)
{
    /* The strips are the one thing the draw pass writes (their bake cache). */
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
     * as a dithered strip tiled over the whole object */
    {
        const int rows = (int)(oa.y2 - oa.y1 + 1);
        strip_bake(ms->strip_sky, &ms->strip_sky_dsc, ms->strip_sky_key, rows,
                   (int)dl->glow_y, P[PAL_SKY_TOP], P[PAL_SKY_GLOW]);
        lv_area_t a = oa;
        a.y2 = a.y1 + (int32_t)s->strip_sky_dsc.header.h - 1;
        draw_strip(layer, &s->strip_sky_dsc, &a);
    }

    /* 2 travelled path: thick dashes (imaged) */
    draw_dashes(&c, dl->trav, dl->n_trav, P[PAL_AMBER], 5, LV_OPA_COVER, 9.0f, 9.0f);
    /* 3 path to come: thin dashes (not yet imaged) */
    draw_dashes(&c, dl->fut, dl->n_fut, P[PAL_AMBER], 2, LV_OPA_COVER, 9.0f, 9.0f);
    /* 4 beads */
    for (int i = 0; i < dl->n_beads; i++) {
        draw_dot(&c, dl->beads[i].x, dl->beads[i].y, 4, dl->beads[i].color, LV_OPA_COVER);
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
    /* 7 target */
    if (dl->target_in) {
        draw_dot(&c, dl->target_x, dl->target_y, 6, P[PAL_AMBER], LV_OPA_COVER);
        draw_dot(&c, dl->target_x, dl->target_y, 2, P[PAL_TARGET_C], LV_OPA_COVER);
    }
    /* 8 land: the yard floor below the level horizon row, the obstructions
     * above it as column rectangles, their smoothed edge, the grass, then the
     * readout band (the floor's near colour, so it continues) */
    const bool band = s->opts.ground_band_top > 0 && s->opts.ground_band_top < bottom;
    const int floor_end = band ? s->opts.ground_band_top - 1 : bottom - 1;
    if (dl->floor_y <= floor_end) {
        const int rows = floor_end - dl->floor_y + 1;
        strip_bake(ms->strip_floor, &ms->strip_floor_dsc, ms->strip_floor_key, rows, rows - 1,
                   P[PAL_FLOOR_FAR], P[PAL_FLOOR_NEAR]);
        lv_area_t a = oa;
        a.y1 = oa.y1 + dl->floor_y;
        a.y2 = a.y1 + (int32_t)s->strip_floor_dsc.header.h - 1;
        draw_strip(layer, &s->strip_floor_dsc, &a);
    }
    for (int i = 0; i < dl->n_cols; i++) {
        const col_px_t *k = &dl->cols[i];
        draw_fill(&c, k->x1, k->y, k->x2, dl->floor_y - 1, P[PAL_LAND]);
    }
    for (int i = 0; i < dl->n_hz; i++) {
        const seg_px_t *g = &dl->hz[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, P[PAL_LAND], SCN_EDGE_W, LV_OPA_COVER, true);
    }
    for (int i = 0; i < dl->n_grass; i++) {
        const grass_px_t *g = &dl->grass[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, P[PAL_GRASS], g->w, LV_OPA_COVER, true);
    }
    if (band) {
        draw_fill(&c, 0, s->opts.ground_band_top, bottom - 1, bottom - 1, P[PAL_FLOOR_NEAR]);
    }
    /* 8b compass labels just above the ground row, with a short tick on it.
     * Same face as the layouts' picture count (Montserrat 24). A label whose
     * box would leave the panel is dropped. */
    if (dl->n_compass > 0) {
        const lv_font_t *font = &lv_font_montserrat_24;
        const int lh = lv_font_get_line_height(font);
        const int ty = dl->floor_y - lh - 8;
        if (ty >= 0 && dl->floor_y - 1 <= floor_end) {
            lv_draw_label_dsc_t ld;
            lv_draw_label_dsc_init(&ld);
            ld.font = font;
            ld.color = lv_color_hex(P[PAL_FLIP_LIGHT]);
            ld.opa = OPA_PCT(60);
            ld.align = LV_TEXT_ALIGN_CENTER;
            /* The draw task runs after this call returns, so the text must be
             * copied: buf below is a stack local. */
            ld.text_local = 1;
            for (int i = 0; i < dl->n_compass; i++) {
                char buf[16];
                const char *txt = sky_compass_text(dl->compass[i].az, buf, sizeof(buf));
                lv_point_t tsz;
                lv_text_get_size(&tsz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                const int w = (int)tsz.x + 4;
                const int x = dl->compass[i].x;
                if (x - w / 2 < 0 || x + w / 2 > s->size) {
                    continue;
                }
                lv_area_t ta;
                ta.x1 = c.ox + x - w / 2;
                ta.x2 = c.ox + x + w / 2;
                ta.y1 = c.oy + ty;
                ta.y2 = c.oy + ty + lh;
                ld.text = txt;
                lv_draw_label(layer, &ld, &ta);
                draw_line(&c, x, dl->floor_y - 6, x, dl->floor_y - 1, P[PAL_FLIP_LIGHT], 2,
                          OPA_PCT(60), false);
            }
        }
    }
    /* 9 telescope, far to near: a thin dark edge, the part's grey, and a
     * highlight down the tube; cyan only on the ring at the tube's front */
    for (int i = 0; i < dl->n_caps; i++) {
        const cap_px_t *k = &dl->caps[i];
        uint32_t body;
        switch (k->part) {
        case SKY_PART_PIER:
            body = P[PAL_PIER];
            break;
        case SKY_PART_MOUNT:
        case SKY_PART_SHAFT:
            body = P[PAL_MOUNT];
            break;
        case SKY_PART_WEIGHT:
            body = P[PAL_WEIGHT];
            break;
        case SKY_PART_TUBE:
            body = P[PAL_TUBE];
            break;
        case SKY_PART_FRONT:
            body = P[PAL_TUBE];   /* grey cell with a cyan rim, see below */
            break;
        default:
            body = P[PAL_CAMERA];
            break;
        }
        /* Round ends everywhere but the counterweight, which is a short fat
         * cylinder on the shaft: flat ends read as a disc, round ends as a pill. */
        const bool rnd = (k->part != SKY_PART_WEIGHT);
        /* The tube front keeps the cyan as its outline only: seen end-on the
         * short capsule fills into a disc, and a solid cyan disc read as a blob. */
        const bool front = (k->part == SKY_PART_FRONT);
        draw_line(&c, k->x1, k->y1, k->x2, k->y2, front ? P[PAL_CYAN] : P[PAL_EDGE],
                  k->w + (front ? 5 : 3), LV_OPA_COVER, rnd);
        draw_line(&c, k->x1, k->y1, k->x2, k->y2, body, k->w, LV_OPA_COVER, rnd);
        if (k->part == SKY_PART_TUBE && k->w >= 8) {
            int hw = k->w / 4;
            if (hw < 2) {
                hw = 2;
            }
            draw_line(&c, k->x1, k->y1, k->x2, k->y2, P[PAL_TUBE_HI], hw, LV_OPA_COVER, true);
        }
    }
    /* 10 beam */
    for (int i = 0; i < dl->n_beam; i++) {
        const seg_px_t *g = &dl->beam[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, P[PAL_CYAN], 7, OPA_PCT(14), true);
    }
    for (int i = 0; i < dl->n_beam; i++) {
        const seg_px_t *g = &dl->beam[i];
        draw_line(&c, g->x1, g->y1, g->x2, g->y2, P[PAL_CYAN], 2, OPA_PCT(85), true);
    }
    /* 11 exposure ring (LVGL angles: 270 = twelve o'clock, clockwise) */
    if (dl->ring_in) {
        draw_ring(&c, dl->ring_x, dl->ring_y, SCN_RING_R, dl->ring_color, 4, OPA_PCT(25),
                  0, 360, false);
        if (dl->ring_end > 0) {
            draw_ring(&c, dl->ring_x, dl->ring_y, SCN_RING_R, dl->ring_color, 4, LV_OPA_COVER,
                      270, 270 + dl->ring_end, true);
        }
    }
    /* 12 edge arrows */
    if (dl->tarrow_in) {
        draw_tri(&c, dl->tarrow.x, dl->tarrow.y, P[PAL_AMBER], LV_OPA_COVER);
    }
    if (dl->aarrow_in) {
        draw_tri(&c, dl->aarrow.x, dl->aarrow.y, P[PAL_CYAN], LV_OPA_COVER);
    }
}
