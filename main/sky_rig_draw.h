/* Tessellation of the rig part list into flat draw primitives for the sky scene layouts. */
#pragma once

/**
 * @file sky_rig_draw.h
 * @brief Turns the part list of sky_rig_model.h and a camera into a flat,
 *        painter-ordered list of lines, triangles and dots (the "Solid model"
 *        telescope of the sky scene).
 *
 * Header-only, C99, `static inline`, float only. No ESP-IDF, FreeRTOS or LVGL
 * include. Host-tested by test/host/test_sky_rig_draw.c.
 *
 * All trigonometry happens here, in the caller's project() step. The renderer
 * converts each primitive to integer pixels and replays the list; it clamps
 * line widths itself.
 *
 * Drawing rules (no primitive shares an edge with another, so the software
 * renderer's anti-aliased edges leave no seam):
 *   cylinder body  a fat flat-ended line in the centre tone, plus a quarter
 *                  width line along each side (3 strips) or one half width
 *                  line (2 strips) where that strip's tone differs
 *   tapered body   (end widths differ by more than 2 px and by more than 5
 *                  percent) two triangles plus a 2 px line along their shared
 *                  diagonal
 *   round end      an overlapping triangle fan (sky_rig_fan())
 *   box face       two triangles plus the diagonal line
 */

#include "sky_rig_model.h"

#define SKY_RIG_PRIM_MAX  288       /* the renderer's list capacity */
#define SKY_RIG_FAN_PTS   16        /* most points any circle here is cut into */
#define SKY_RIG_XY_LIMIT  16000.0f  /* emitted coordinates are clamped to +- this (int16 safe) */

typedef enum {
    SKY_PRIM_LINE = 0,
    SKY_PRIM_TRI,
    SKY_PRIM_DOT
} sky_prim_kind_t;

/* Tone index: three per rig colour (0 lit, 1 middle, 2 shade), then the fixed ones. */
enum {
    SKY_TONE_EDGE = SKY_RIGC_N * 3,
    SKY_TONE_MOUTH,
    SKY_TONE_LENS,
    SKY_TONE_GLINT,
    SKY_TONE_CAP,
    SKY_TONE_CAP_RIM,
    SKY_TONE_ACCENT,
    SKY_TONE_N
};
#define SKY_TONE(colour, tone) ((colour) * 3 + (tone))

/** Line: points 0 and 1, `w` width. Triangle: three points, `w` 0. Dot:
 *  point 0, `w` radius. Unused points are 0. `round` = round line ends. */
typedef struct {
    float   x[3], y[3];
    float   w;
    uint8_t kind, tone, round, pad;
} sky_rig_prim_t;

/** A 3D circle on screen, as the small-circle approximation: centre (x, y),
 *  half axes rx >= ry, the long axis turned rot_rad from screen x. `facing` is
 *  the cosine between the circle's normal and the direction to the eye. */
typedef struct {
    bool  vis;
    float x, y, rx, ry, rot_rad, facing;
} sky_ellipse_t;

/* ---- output sink ------------------------------------------------------------ */

typedef struct {
    sky_rig_prim_t *out;
    int             max, n;
    int             lod;    /* 0 = full detail, 1 and 2 = fewer points per round end */
    bool            full;   /* a primitive was dropped for want of room */
    float           box[4];
} sky_rig_sink_t_;

/* How many points a round end of screen radius @p rpx is cut into. */
static inline int sky_rig_steps_(const sky_rig_sink_t_ *s, float rpx)
{
    if (s->lod >= 2) return 8;
    return (s->lod == 1 || rpx < 25.0f) ? 12 : SKY_RIG_FAN_PTS;
}

/* Append one primitive. Dropped when the list is full or a value is not
 * finite. `np` = how many of x[], y[] are read. */
static inline void sky_rig_put_(sky_rig_sink_t_ *s, uint8_t kind, const float *x, const float *y, int np,
                                float w, uint8_t tone, uint8_t round)
{
    if (!isfinite(w) || w < 0.0f) return;
    if (kind != SKY_PRIM_TRI && w <= 0.0f) return;
    for (int i = 0; i < np; i++) {
        if (!isfinite(x[i]) || !isfinite(y[i])) return;
    }
    if (!s->out || s->n >= s->max) {
        s->full = true;
        return;
    }
    if (w > SKY_RIG_XY_LIMIT) w = SKY_RIG_XY_LIMIT;
    sky_rig_prim_t *p = &s->out[s->n];
    const float grow = (kind == SKY_PRIM_LINE) ? 0.5f * w : w;
    for (int i = 0; i < 3; i++) {
        float px = 0.0f, py = 0.0f;
        if (i < np) {
            px = fminf(fmaxf(x[i], -SKY_RIG_XY_LIMIT), SKY_RIG_XY_LIMIT);
            py = fminf(fmaxf(y[i], -SKY_RIG_XY_LIMIT), SKY_RIG_XY_LIMIT);
            if (s->n == 0 && i == 0) {
                s->box[0] = px - grow;
                s->box[1] = py - grow;
                s->box[2] = px + grow;
                s->box[3] = py + grow;
            } else {
                s->box[0] = fminf(s->box[0], px - grow);
                s->box[1] = fminf(s->box[1], py - grow);
                s->box[2] = fmaxf(s->box[2], px + grow);
                s->box[3] = fmaxf(s->box[3], py + grow);
            }
        }
        p->x[i] = px;
        p->y[i] = py;
    }
    p->w = w;
    p->kind = kind;
    p->tone = tone;
    p->round = round;
    p->pad = 0;
    s->n++;
}

static inline void sky_rig_line_(sky_rig_sink_t_ *s, float x0, float y0, float x1, float y1, float w,
                                 uint8_t tone, uint8_t round)
{
    const float x[2] = { x0, x1 }, y[2] = { y0, y1 };
    sky_rig_put_(s, SKY_PRIM_LINE, x, y, 2, w, tone, round);
}

static inline void sky_rig_tri_(sky_rig_sink_t_ *s, float x0, float y0, float x1, float y1, float x2, float y2,
                                uint8_t tone)
{
    const float x[3] = { x0, x1, x2 }, y[3] = { y0, y1, y2 };
    sky_rig_put_(s, SKY_PRIM_TRI, x, y, 3, 0.0f, tone, 0);
}

/* A four-sided fill, corners in order round the outline: two triangles plus a
 * 2 px line along the diagonal they share. */
static inline void sky_rig_quad_(sky_rig_sink_t_ *s, const float *x, const float *y, uint8_t tone)
{
    sky_rig_tri_(s, x[0], y[0], x[1], y[1], x[2], y[2], tone);
    sky_rig_tri_(s, x[0], y[0], x[2], y[2], x[3], y[3], tone);
    sky_rig_line_(s, x[0], y[0], x[2], y[2], 2.0f, tone, 0);
}

static inline void sky_rig_fan_(sky_rig_sink_t_ *s, const float *xs, const float *ys, int n, float cx, float cy,
                                uint8_t tone)
{
    if (n < 3) return;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n, k = (i + 2) % n;
        sky_rig_tri_(s, cx, cy, xs[i], ys[i], 0.5f * (xs[j] + xs[k]), 0.5f * (ys[j] + ys[k]), tone);
    }
}

/* ---- public pieces ---------------------------------------------------------- */

/** Screen position and camera depth of world point @p p. False (x, y
 *  untouched) when the point is nearer than SKY_NEAR_Z. */
static inline bool sky_rig_px_(const sky_cam_t *cam, sky_v3_t p, float *x, float *y, float *z)
{
    const sky_v3_t c = sky_cam_point(cam, p);
    if (z) *z = c.z;
    return sky_cam_screen(cam, c, x, y);
}

/** A unit vector across @p u. */
static inline sky_v3_t sky_rig_perp_(sky_v3_t u)
{
    const sky_v3_t ref = (fabsf(u.z) < 0.9f) ? sky_v3(0.0f, 0.0f, 1.0f) : sky_v3(1.0f, 0.0f, 0.0f);
    return sky_v3_norm(sky_v3_cross(u, ref));
}

/**
 * Project the circle of radius @p r (metres) about @p centre with normal
 * @p normal as @p steps screen points (xs[], ys[] hold at least @p steps) and,
 * when @p e is not NULL, as an ellipse. Returns false when steps < 3, the
 * centre or any point is behind the camera; outputs are then not usable.
 */
static inline bool sky_circle_project(const sky_cam_t *cam, sky_v3_t centre, sky_v3_t normal, float r, int steps,
                                      float *xs, float *ys, sky_ellipse_t *e)
{
    const sky_v3_t n = sky_v3_norm(normal);
    const sky_v3_t u = sky_rig_perp_(n);
    const sky_v3_t w = sky_v3_cross(n, u);
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    bool vis = sky_rig_px_(cam, centre, &cx, &cy, &cz);
    if (steps < 3) vis = false;
    for (int i = 0; i < steps && vis; i++) {
        const float a = 6.2831853f * (float)i / (float)steps;
        const sky_v3_t p = sky_v3_add(centre, sky_v3_add(sky_v3_scale(u, r * cosf(a)), sky_v3_scale(w, r * sinf(a))));
        float px = 0.0f, py = 0.0f;
        if (!sky_rig_px_(cam, p, &px, &py, NULL)) vis = false;
        xs[i] = px;
        ys[i] = py;
    }
    if (e) {
        e->vis = vis;
        e->x = cx;
        e->y = cy;
        e->rx = 0.0f;
        e->ry = 0.0f;
        e->rot_rad = 0.0f;
        e->facing = sky_v3_dot(n, sky_v3_norm(sky_v3_sub(cam->pos, centre)));
        if (vis) {
            float qx = 0.0f, qy = 0.0f;
            e->rx = r * cam->focal_px / cz;
            e->ry = e->rx * fabsf(e->facing);
            if (sky_rig_px_(cam, sky_v3_add(centre, sky_v3_scale(n, r)), &qx, &qy, NULL)) {
                const float dx = qx - cx, dy = qy - cy;
                if (dx * dx + dy * dy > 1e-6f) e->rot_rad = atan2f(dy, dx) + 1.5707963f;
            }
        }
    }
    return vis;
}

/** Tone of a surface with unit normal @p normal under unit @p light:
 *  0 lit (dot above 0.45), 1 middle (above -0.05), 2 shade. */
static inline int sky_rig_tone_of(sky_v3_t normal, sky_v3_t light)
{
    const float lam = sky_v3_dot(normal, light);
    if (lam > 0.45f) return 0;
    return (lam > -0.05f) ? 1 : 2;
}

/**
 * The overlapping fan that fills the closed outline xs[], ys[] of @p n points
 * (a convex ellipse) about (cx, cy): triangle i is the centre, point i, and
 * the point halfway between points i+1 and i+2, so each triangle overlaps the
 * next by half a step and no two share an edge. Writes at most @p max
 * triangles of tone @p tone to @p out; returns how many (n when they fit).
 */
static inline int sky_rig_fan(const float *xs, const float *ys, int n, float cx, float cy, uint8_t tone,
                              sky_rig_prim_t *out, int max)
{
    sky_rig_sink_t_ s = { out, max, 0, 0, false, { 0.0f, 0.0f, 0.0f, 0.0f } };
    sky_rig_fan_(&s, xs, ys, n, cx, cy, tone);
    return s.n;
}

/* ---- parts ------------------------------------------------------------------ */

/* The half of the circle round axis @p u at @p c that faces the eye:
 * vp points from the axis toward the eye, side = u x vp. */
static inline void sky_rig_frame_(const sky_cam_t *cam, sky_v3_t c, sky_v3_t u, sky_v3_t *side, sky_v3_t *vp)
{
    const sky_v3_t e = sky_v3_sub(cam->pos, c);
    sky_v3_t v = sky_v3_sub(e, sky_v3_scale(u, sky_v3_dot(e, u)));
    v = (sky_v3_len(v) < 1e-6f) ? sky_rig_perp_(u) : sky_v3_norm(v);
    *vp = v;
    *side = sky_v3_norm(sky_v3_cross(u, v));
}

static inline int sky_rig_find_(const sky_rig_part_t *parts, int n, int id)
{
    for (int i = 0; i < n; i++) {
        if (parts[i].id == id) return i;
    }
    return -1;
}

/* True when (px, py) lies inside the convex outline xs[], ys[]. */
static inline bool sky_rig_inside_(float px, float py, const float *xs, const float *ys, int n)
{
    int pos = 0, neg = 0;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const float c = (xs[j] - xs[i]) * (py - ys[i]) - (ys[j] - ys[i]) * (px - xs[i]);
        if (c > 0.0f) pos++;
        if (c < 0.0f) neg++;
    }
    return pos == 0 || neg == 0;
}

/* The dew shield mouth seen from the front: red rim, dark opening (the cap
 * when parked), the lens and its glint when the whole lens shows, and the
 * opening's outline in the accent. */
static inline void sky_rig_mouth_(sky_rig_sink_t_ *s, const sky_rig_part_t *pt, sky_v3_t u, const sky_cam_t *cam,
                                  sky_v3_t light, float shield_r, float tube_r, float scale, bool parked)
{
    float rx[SKY_RIG_FAN_PTS], ry[SKY_RIG_FAN_PTS], ix[SKY_RIG_FAN_PTS], iy[SKY_RIG_FAN_PTS];
    sky_ellipse_t re, ie;
    const int n = (s->lod >= 2) ? 12 : SKY_RIG_FAN_PTS, ln = (s->lod >= 2) ? 8 : 12;
    if (!sky_circle_project(cam, pt->b, u, pt->r1, n, rx, ry, &re)) return;
    sky_rig_fan_(s, rx, ry, n, re.x, re.y, (uint8_t)SKY_TONE(pt->colour, (sky_rig_tone_of(u, light) == 0) ? 0 : 1));
    if (!sky_circle_project(cam, pt->b, u, shield_r * 0.94f, n, ix, iy, &ie)) return;
    sky_rig_fan_(s, ix, iy, n, ie.x, ie.y, parked ? SKY_TONE_CAP : SKY_TONE_MOUTH);
    if (!parked) {
        float lx[12], ly[12];
        sky_ellipse_t le;
        const sky_v3_t lc = sky_v3_sub(pt->b, sky_v3_scale(u, 0.18f * scale));
        bool show = sky_circle_project(cam, lc, u, tube_r * 0.9f, ln, lx, ly, &le);
        for (int i = 0; i < ln && show; i++) {
            if (!sky_rig_inside_(lx[i], ly[i], ix, iy, n)) show = false;
        }
        if (show) {
            sky_rig_fan_(s, lx, ly, ln, le.x, le.y, SKY_TONE_LENS);
            const sky_v3_t off = sky_v3_norm(sky_v3_sub(light, sky_v3_scale(u, sky_v3_dot(light, u))));
            float gx = 0.0f, gy = 0.0f;
            if (sky_rig_px_(cam, sky_v3_add(lc, sky_v3_scale(off, tube_r * 0.45f)), &gx, &gy, NULL)) {
                sky_rig_put_(s, SKY_PRIM_DOT, &gx, &gy, 1, fmaxf(0.8f, 0.12f * le.rx), SKY_TONE_GLINT, 0);
            }
        }
    }
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        sky_rig_line_(s, ix[i], iy[i], ix[j], iy[j], 2.0f, parked ? SKY_TONE_CAP_RIM : SKY_TONE_ACCENT, 1);
    }
}

/* The mouth seen from behind: the near half of the front edge, 10 lines. */
static inline void sky_rig_far_rim_(sky_rig_sink_t_ *s, const sky_rig_part_t *pt, sky_v3_t u, const sky_cam_t *cam,
                                    bool parked)
{
    float xs[11], ys[11];
    sky_v3_t side, vp;
    sky_rig_frame_(cam, pt->b, u, &side, &vp);
    for (int j = 0; j <= 10; j++) {
        const float th = 3.14159265f * (float)j / 10.0f;
        const sky_v3_t nrm = sky_v3_add(sky_v3_scale(side, cosf(th)), sky_v3_scale(vp, sinf(th)));
        xs[j] = 0.0f;
        ys[j] = 0.0f;
        if (!sky_rig_px_(cam, sky_v3_add(pt->b, sky_v3_scale(nrm, pt->r1)), &xs[j], &ys[j], NULL)) return;
    }
    for (int j = 0; j < 10; j++) {
        sky_rig_line_(s, xs[j], ys[j], xs[j + 1], ys[j + 1], 2.0f, parked ? SKY_TONE_CAP_RIM : SKY_TONE_ACCENT, 1);
    }
}

/* True when the end of @p pt at @p c (radius @p r, outward normal @p nrm) is
 * covered by another part that starts on the same point, runs on outward along
 * the same axis and is at least as wide there. That part is nearer the eye
 * whenever this end faces it, so the end can never show. */
static inline bool sky_rig_end_hidden_(const sky_rig_part_t *parts, int n, const sky_rig_part_t *pt, sky_v3_t c,
                                       sky_v3_t nrm, float r)
{
    for (int i = 0; i < n; i++) {
        const sky_rig_part_t *o = &parts[i];
        if (o == pt || o->strips == 0) continue;
        const sky_v3_t da = sky_v3_sub(o->a, c), db = sky_v3_sub(o->b, c);
        const float tol = 0.02f * r;
        if (sky_v3_len(da) < tol && o->r0 >= r && sky_v3_dot(sky_v3_norm(db), nrm) > 0.999f) return true;
        if (sky_v3_len(db) < tol && o->r1 >= r && sky_v3_dot(sky_v3_norm(da), nrm) > 0.999f) return true;
    }
    return false;
}

/* One rod: the body in flat strips, then the end that faces the eye. */
static inline void sky_rig_cyl_(sky_rig_sink_t_ *s, const sky_rig_part_t *parts, int n, const sky_rig_part_t *pt,
                                const sky_cam_t *cam, sky_v3_t light, float scale, bool parked)
{
    const sky_v3_t axis = sky_v3_sub(pt->b, pt->a);
    if (sky_v3_len(axis) < 1e-6f) return;
    const sky_v3_t u = sky_v3_norm(axis);
    float ax = 0.0f, ay = 0.0f, az = 0.0f, bx = 0.0f, by = 0.0f, bz = 0.0f;
    if (!sky_rig_px_(cam, pt->a, &ax, &ay, &az) || !sky_rig_px_(cam, pt->b, &bx, &by, &bz)) return;
    const float wa = 2.0f * pt->r0 * cam->focal_px / az, wb = 2.0f * pt->r1 * cam->focal_px / bz;
    const float w = 0.5f * (wa + wb);
    const sky_v3_t mid = sky_v3_scale(sky_v3_add(pt->a, pt->b), 0.5f);
    sky_v3_t side, vp;
    sky_rig_frame_(cam, mid, u, &side, &vp);

    /* Strip tones, from the +side edge across to the -side edge. */
    int tone[3] = { 1, 1, 1 };
    if (pt->strips == 2) {
        tone[0] = sky_rig_tone_of(sky_v3_scale(sky_v3_add(vp, side), 0.70710678f), light);
        tone[1] = sky_rig_tone_of(sky_v3_scale(sky_v3_sub(vp, side), 0.70710678f), light);
    } else if (pt->strips >= 3) {
        tone[0] = sky_rig_tone_of(sky_v3_add(sky_v3_scale(side, 0.8660254f), sky_v3_scale(vp, 0.5f)), light);
        tone[1] = sky_rig_tone_of(vp, light);
        tone[2] = sky_rig_tone_of(sky_v3_add(sky_v3_scale(side, -0.8660254f), sky_v3_scale(vp, 0.5f)), light);
    }
    const int centre = (pt->strips >= 3) ? tone[1] : tone[0];

    const float dx = bx - ax, dy = by - ay;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len >= 0.5f) {
        const float nx = -dy / len, ny = dx / len;
        /* Which screen side of the line the +side edge falls on. */
        float mx = 0.0f, my = 0.0f, sx = 0.0f, sy = 0.0f, sgn = 1.0f;
        if (sky_rig_px_(cam, mid, &mx, &my, NULL) &&
            sky_rig_px_(cam, sky_v3_add(mid, sky_v3_scale(side, 0.5f * (pt->r0 + pt->r1))), &sx, &sy, NULL)) {
            if ((sx - mx) * nx + (sy - my) * ny < 0.0f) sgn = -1.0f;
        }
        if (fabsf(wa - wb) > fmaxf(2.0f, 0.05f * w) && w >= 8.0f) {
            const float qx[4] = { ax + nx * 0.5f * wa, ax - nx * 0.5f * wa, bx - nx * 0.5f * wb, bx + nx * 0.5f * wb };
            const float qy[4] = { ay + ny * 0.5f * wa, ay - ny * 0.5f * wa, by - ny * 0.5f * wb, by + ny * 0.5f * wb };
            sky_rig_quad_(s, qx, qy, (uint8_t)SKY_TONE(pt->colour, centre));
        } else {
            sky_rig_line_(s, ax, ay, bx, by, w, (uint8_t)SKY_TONE(pt->colour, centre), 0);
        }
        if (pt->strips >= 3) {
            for (int k = 0; k < 3; k += 2) {
                if (tone[k] == centre) continue;
                const float off = ((k == 0) ? sgn : -sgn) * 0.375f;
                sky_rig_line_(s, ax + nx * off * wa, ay + ny * off * wa, bx + nx * off * wb, by + ny * off * wb,
                              0.25f * w, (uint8_t)SKY_TONE(pt->colour, tone[k]), 0);
            }
        } else if (pt->strips == 2 && tone[1] != centre) {
            const float off = -sgn * 0.25f;
            sky_rig_line_(s, ax + nx * off * wa, ay + ny * off * wa, bx + nx * off * wb, by + ny * off * wb,
                          0.5f * w, (uint8_t)SKY_TONE(pt->colour, tone[1]), 0);
        }
    }

    /* The end you can see. */
    const bool front = sky_v3_dot(u, sky_v3_sub(cam->pos, pt->b)) > 0.0f;
    if (pt->id == SKY_RIGP_LIP) {
        if (front) {
            const int si = sky_rig_find_(parts, n, SKY_RIGP_SHIELD), ti = sky_rig_find_(parts, n, SKY_RIGP_TUBE);
            sky_rig_mouth_(s, pt, u, cam, light, (si >= 0) ? parts[si].r1 : pt->r1, (ti >= 0) ? parts[ti].r1 : pt->r1,
                           scale, parked);
        } else {
            sky_rig_far_rim_(s, pt, u, cam, parked);
        }
        return;
    }
    if (pt->over != SKY_RIG_NO_WRAP) return;
    const sky_v3_t c = front ? pt->b : pt->a;
    const sky_v3_t nrm = front ? u : sky_v3_scale(u, -1.0f);
    const float r = front ? pt->r1 : pt->r0;
    if (sky_v3_dot(nrm, sky_v3_sub(cam->pos, c)) <= 0.0f) return;
    const float rpx = r * cam->focal_px / (front ? bz : az);
    if (rpx < 3.0f) return;
    if (sky_rig_end_hidden_(parts, n, pt, c, nrm, r)) return;
    /* The shield's open front is drawn by the lip's mouth. */
    if (pt->id == SKY_RIGP_SHIELD && front && sky_rig_find_(parts, n, SKY_RIGP_LIP) >= 0) return;
    float xs[SKY_RIG_FAN_PTS], ys[SKY_RIG_FAN_PTS];
    sky_ellipse_t e;
    const int steps = sky_rig_steps_(s, rpx);
    if (!sky_circle_project(cam, c, nrm, r, steps, xs, ys, &e)) return;
    sky_rig_fan_(s, xs, ys, steps, e.x, e.y, (uint8_t)SKY_TONE(pt->colour, (sky_rig_tone_of(nrm, light) == 0) ? 0 : 1));
    if (pt->id == SKY_RIGP_CAMERA && !front) {   /* round camera back: dark centre */
        const int inner = (steps < 12) ? steps : 12;
        if (sky_circle_project(cam, c, nrm, r * 0.45f, inner, xs, ys, &e)) {
            sky_rig_fan_(s, xs, ys, inner, e.x, e.y, (uint8_t)SKY_TONE(SKY_RIGC_BLACK, 1));
        }
    }
}

/* The mount head: a box along the part's axis, its second axis the
 * declination axis. Every face toward the eye is one quad. */
static inline void sky_rig_box_(sky_rig_sink_t_ *s, const sky_rig_part_t *pt, const sky_pose_t *pose,
                                const sky_cam_t *cam, sky_v3_t light)
{
    const sky_v3_t axis = sky_v3_sub(pt->b, pt->a);
    const float hl = 0.5f * sky_v3_len(axis);
    if (hl < 1e-6f) return;
    const sky_v3_t u = sky_v3_norm(axis);
    sky_v3_t v = sky_v3_sub(pose->decl, sky_v3_scale(u, sky_v3_dot(pose->decl, u)));
    v = (sky_v3_len(v) < 1e-6f) ? sky_rig_perp_(u) : sky_v3_norm(v);
    const sky_v3_t ax[3] = { u, v, sky_v3_cross(u, v) };
    const float half[3] = { hl, pt->r0, pt->r0 };
    const sky_v3_t c = sky_v3_scale(sky_v3_add(pt->a, pt->b), 0.5f);
    for (int f = 0; f < 6; f++) {
        const int k = f / 2, i1 = (k + 1) % 3, i2 = (k + 2) % 3;
        const sky_v3_t fn = sky_v3_scale(ax[k], (f & 1) ? -1.0f : 1.0f);
        const sky_v3_t fc = sky_v3_add(c, sky_v3_scale(fn, half[k]));
        if (sky_v3_dot(fn, sky_v3_sub(cam->pos, fc)) <= 0.0f) continue;
        const sky_v3_t ea = sky_v3_scale(ax[i1], half[i1]), eb = sky_v3_scale(ax[i2], half[i2]);
        const sky_v3_t corner[4] = {
            sky_v3_add(sky_v3_add(fc, ea), eb), sky_v3_add(sky_v3_sub(fc, ea), eb),
            sky_v3_sub(sky_v3_sub(fc, ea), eb), sky_v3_sub(sky_v3_add(fc, ea), eb),
        };
        float qx[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, qy[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        bool ok = true;
        for (int j = 0; j < 4; j++) {
            if (!sky_rig_px_(cam, corner[j], &qx[j], &qy[j], NULL)) ok = false;
        }
        if (ok) sky_rig_quad_(s, qx, qy, (uint8_t)SKY_TONE(pt->colour, sky_rig_tone_of(fn, light)));
    }
}

/* ---- painter order ---------------------------------------------------------- */

/* Tube group classes: the rail sits on the mount side of the tube, the handle
 * on the far side, everything else is on the tube axis. */
static inline int sky_rig_class_(const sky_rig_part_t *pt)
{
    if (pt->id == SKY_RIGP_DOVETAIL) return 0;
    return (pt->id == SKY_RIGP_HANDLE) ? 2 : 1;
}

/* True when part @p i is drawn before part @p j: far to near by the mean
 * depth of the part each belongs to (itself, or the part it wraps), a wrap
 * just after its own part, wraps of one part far to near. */
static inline bool sky_rig_before_(int i, int j, const float *depth, const uint8_t *root)
{
    if (root[i] != root[j]) return depth[root[i]] > depth[root[j]];
    if (root[i] == i) return true;
    if (root[j] == j) return false;
    return depth[i] > depth[j];
}

/* Append to order[] every part of @p group (and tube class @p cls, -1 = any),
 * sorted. Returns the new count. */
static inline int sky_rig_seg_(const sky_rig_part_t *parts, int n, const float *depth, const uint8_t *root,
                               int group, int cls, uint8_t *order, int cnt)
{
    const int first = cnt;
    for (int i = 0; i < n && cnt < SKY_RIG_PART_N; i++) {
        if (parts[i].group != group) continue;
        if (cls >= 0 && sky_rig_class_(&parts[i]) != cls) continue;
        int at = cnt;
        while (at > first && sky_rig_before_(i, order[at - 1], depth, root)) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = (uint8_t)i;
        cnt++;
    }
    return cnt;
}

/* One whole telescope into the sink at the sink's level of detail. */
static inline void sky_rig_pass_(sky_rig_sink_t_ *s, const sky_rig_part_t *parts, int n, const sky_pose_t *pose,
                                 float scale, const sky_cam_t *cam, bool parked)
{
    float depth[SKY_RIG_PART_N];
    uint8_t root[SKY_RIG_PART_N], order[SKY_RIG_PART_N];
    int cnt = 0;
    const sky_v3_t head = sky_rig_model_head();
    const sky_v3_t light = sky_v3_norm(sky_v3_sub(sky_v3(0.0f, 0.0f, 1.0f), sky_v3_scale(cam->fwd, 0.35f)));

    /* Outline underlay: every part's dark line first, so only the outer
     * silhouette keeps an edge. */
    for (int i = 0; i < n; i++) {
        float ax = 0.0f, ay = 0.0f, az = 0.0f, bx = 0.0f, by = 0.0f, bz = 0.0f;
        const bool va = sky_rig_px_(cam, parts[i].a, &ax, &ay, &az);
        const bool vb = sky_rig_px_(cam, parts[i].b, &bx, &by, &bz);
        const int over = (parts[i].over == SKY_RIG_NO_WRAP) ? -1 : sky_rig_find_(parts, n, parts[i].over);
        depth[i] = 0.5f * (az + bz);
        root[i] = (uint8_t)((over >= 0) ? over : i);
        if (!va || !vb) continue;
        const float w = (parts[i].r0 / az + parts[i].r1 / bz) * cam->focal_px;
        if (w < 2.0f) continue;
        sky_rig_line_(s, ax, ay, bx, by, w + 3.0f, SKY_TONE_EDGE, 0);
    }

    const bool tube_near = sky_v3_dot(pose->decl, sky_v3_norm(sky_v3_sub(cam->pos, head))) > 0.0f;
    cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_PIER, -1, order, cnt);
    if (tube_near) {
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_MOUNT, -1, order, cnt);
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_TUBE, 0, order, cnt);
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_TUBE, 1, order, cnt);
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_TUBE, 2, order, cnt);
    } else {
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_TUBE, 2, order, cnt);
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_TUBE, 1, order, cnt);
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_TUBE, 0, order, cnt);
        cnt = sky_rig_seg_(parts, n, depth, root, SKY_RIGG_MOUNT, -1, order, cnt);
    }
    for (int i = 0; i < cnt; i++) {
        const sky_rig_part_t *pt = &parts[order[i]];
        if (pt->strips == 0) {
            sky_rig_box_(s, pt, pose, cam, light);
        } else {
            sky_rig_cyl_(s, parts, n, pt, cam, light, scale, parked);
        }
    }
}

/**
 * The rig as flat primitives, in drawing order: one SKY_TONE_EDGE outline line
 * per part first, then the pier, then the mount and the tube (whichever is
 * further from the eye first), each far to near.
 *
 * @param parts,n  from sky_rig_parts(); at most SKY_RIG_PART_N are read
 * @param pose     the pose the parts were built from
 * @param scale    the scale the parts were built with
 * @param parked   mouth capped, rim in the parked tone instead of the accent
 * @param out,max  primitive list and its capacity. A telescope that does not
 *                 fit is cut again with fewer points per round end (twice at
 *                 most); only when that still overflows are further
 *                 primitives dropped (end fans come last within a part)
 * @param box      x0, y0, x1, y1 over every emitted point, grown by half the
 *                 line width (the radius for a dot); all 0 when nothing was
 *                 emitted
 * @return how many primitives were written, 0..max. Every value is finite and
 *         every coordinate within +- SKY_RIG_XY_LIMIT.
 */
static inline int sky_rig_tessellate(const sky_rig_part_t *parts, int n, const sky_pose_t *pose, float scale,
                                     const sky_cam_t *cam, bool parked, sky_rig_prim_t *out, int max, float box[4])
{
    sky_rig_sink_t_ s = { out, max, 0, 0, false, { 0.0f, 0.0f, 0.0f, 0.0f } };
    if (n > SKY_RIG_PART_N) n = SKY_RIG_PART_N;
    if (parts && pose && cam && out && max > 0 && n > 0) {
        /* Full detail first; when that does not fit, again with fewer points
         * per round end, so the nearest parts (drawn last) are never the ones
         * dropped. */
        for (int lod = 0; lod < 3; lod++) {
            s.n = 0;
            s.lod = lod;
            s.full = false;
            sky_rig_pass_(&s, parts, n, pose, scale, cam, parked);
            if (!s.full) break;
        }
    }
    if (box) {
        for (int i = 0; i < 4; i++) box[i] = (s.n > 0) ? s.box[i] : 0.0f;
    }
    return s.n;
}
