/**
 * @file nina_subbar.c
 * @brief Segmented sub bar, block row and ring (see nina_subbar.h).
 *
 * The segment list comes from nina_plan_segments.h: one segment per Smart
 * Exposure per loop round of the running container, in execution order, as
 * wide as that item's iteration count. Row form: flex grow = weight with a
 * 4 px floor, 3 px radius, 3 px gap widening to 9 px at a round boundary.
 * Ring form: the same segments as lv_arc arcs on one circle, angular span
 * proportional to weight with about 4 px of arc as the floor and the same two
 * gaps. Done = the item's filter colour, running = 30 % base plus a fill,
 * remaining = a dim (LV_OPA_30) preview of that same item's filter colour;
 * the running segment's fill is full brightness, its unfilled part dim
 * (the collapsed per-round overflow segment stays the neutral colour it
 * always had), a disabled item = 0x161616 at 50 %.
 *
 * The objects are rebuilt only when the geometry fingerprint changes (segment
 * count, weights, item identity, round boundaries); everything else is a
 * repaint, and every colour write is compared against what the object already
 * carries so an unchanged value never invalidates this full-refresh panel.
 *
 * Every entry point runs with the LVGL display lock held by the caller.
 */

#include "nina_subbar.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "app_config.h"
#include "themes.h"
#include "ui_dial.h"
#include "ui_helpers.h"

#define SB_GAP_TIGHT      3
#define SB_GAP_WIDE       9
#define SB_BLOCK_RADIUS   3
#define SB_FILL_OPA       LV_OPA_COVER /* the running image's fill is as bright as a
                                        * done segment, so the colour does not step up
                                        * when the exposure ends (user, 2026-09-08) */
#define SB_DIM_OPA        LV_OPA_30   /* the running segment's unfilled part, same as
                                        * a REMAINING segment's dim preview */
#define SB_PLACEHOLDER_OPA LV_OPA_50
#define SB_ROUND_OPA      LV_OPA_70   /* a finished collapsed per-round segment */
#define SB_REMAIN_OPA     LV_OPA_30   /* dim preview of a REMAINING segment's own colour */
#define SB_REMAIN_COLOR   0x161616    /* PLACEHOLDER (disabled item) segments only */
#define SB_MIN_PX         4     /* narrowest a segment may draw: row px, or arc px */
#define SB_DEG_PER_RAD    57.29578f

/* The derived segment list. Every entry point runs with the LVGL display lock
 * held, so one scratch buffer serves every sub bar on the device; it is always
 * rebuilt for the sub bar in hand before it is read. PSRAM, allocated on first
 * use (about 3.4 KB), never freed. */
static nina_seg_t *s_segs;
static int         s_nsegs;

/* The per-widget bulk: the segment objects and the plan they were drawn from.
 * Every nina_subbar_t is embedded in a static page or card struct that its
 * owner memsets and re-creates on a rebuild, so a pointer kept inside the
 * widget would leak this block on every rebuild. It is keyed by the widget's
 * (stable) address instead: one PSRAM block per widget, allocated on the first
 * create, reused by every later one, never freed. */
struct nina_subbar_store {
    lv_obj_t   *blocks[NINA_SUBBAR_MAX_BLOCKS];
    nina_plan_t plan;
};
/* ponytail: 3 NINA pages + 3 Summary cards today; bump if a new owner appears */
#define SB_STORE_MAX 8
static struct {
    const nina_subbar_t      *key;
    struct nina_subbar_store *st;
} s_store[SB_STORE_MAX];

static struct nina_subbar_store *sb_store_get(const nina_subbar_t *sb) {
    int free_slot = -1;
    for (int i = 0; i < SB_STORE_MAX; i++) {
        if (s_store[i].key == sb) return s_store[i].st;
        if (!s_store[i].key && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    struct nina_subbar_store *st =
        heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_SPIRAM);
    if (!st) return NULL;
    s_store[free_slot].key = sb;
    s_store[free_slot].st  = st;
    return st;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static uint32_t sb_block_color(const char *filter, int instance_idx, int gb);
static uint32_t sb_seg_color(const nina_subbar_t *sb, const nina_seg_t *s);
static void sb_build_segs(nina_subbar_t *sb, float frac);
static uint32_t sb_fingerprint(void);
static bool sb_wide_gap_after(int i);
static void sb_rebuild_blocks(nina_subbar_t *sb);
static void sb_paint_blocks(nina_subbar_t *sb);
static void sb_place_fill(nina_subbar_t *sb, int idx, uint32_t color);
static void sb_ring_geom(const nina_subbar_t *sb, int i, int *a0, int *a1);
static void sb_style_block(const nina_subbar_t *sb, lv_obj_t *b, uint32_t color, lv_opa_t opa);
static void sb_ring_fill_angles(nina_subbar_t *sb, float frac);
static float sb_within(const nina_subbar_t *sb, float frac);
static void sb_apply_visibility(nina_subbar_t *sb);

/* The one writer of the container's HIDDEN flag, in both forms. Shown only
 * when the layout's view wants it AND the plan left something to draw (a plan
 * of a single image draws nothing: the exposure ring already tells that
 * story). */
static void sb_apply_visibility(nina_subbar_t *sb) {
    if (!sb->cont) return;
    bool show = sb->shown && sb->n_blocks > 0;
    if (show) lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(sb->cont, LV_OBJ_FLAG_HIDDEN);
}

/* Segment colour: that item's configured filter colour, clamped to the theme's
 * progress colour on Red Night so no non-red hue reaches the panel. */
static uint32_t sb_block_color(const char *filter, int instance_idx, int gb) {
    if (!current_theme) return app_config_apply_brightness(0x808080, gb);
    if (theme_is_red_night(current_theme)) {
        return app_config_apply_brightness(current_theme->progress_color, gb);
    }
    if (filter && filter[0] != '\0' && strcmp(filter, "--") != 0) {
        return app_config_get_filter_color(filter, instance_idx);
    }
    return app_config_apply_brightness(current_theme->progress_color, gb);
}

/* A collapsed per-round segment (item -1) stands for a whole round of mixed
 * filters, so it takes the theme's text colour instead of any one of them. */
static uint32_t sb_seg_color(const nina_subbar_t *sb, const nina_seg_t *s) {
    if (s->item >= 0 && s->item < sb->st->plan.n_items
        && s->item < NINA_PLAN_MAX_ITEMS) {
        return sb_block_color(sb->st->plan.items[s->item].filter,
                              sb->instance_idx, sb->gb);
    }
    if (!current_theme) return app_config_apply_brightness(0x808080, sb->gb);
    if (theme_is_red_night(current_theme)) {
        return app_config_apply_brightness(current_theme->progress_color, sb->gb);
    }
    return app_config_apply_brightness(current_theme->text_color, sb->gb);
}

/* Derive the segment list of THIS sub bar into the shared scratch. */
static void sb_build_segs(nina_subbar_t *sb, float frac) {
    if (!s_segs) {
        s_segs = heap_caps_malloc(NINA_SEG_MAX * sizeof(*s_segs), MALLOC_CAP_SPIRAM);
        if (!s_segs) {
            s_nsegs = 0;   /* nothing to draw: the widget stays hidden */
            return;
        }
    }
    if (sb->unknown) {
        /* No plan and no planned count: the sequence is stopped, finished or
         * not loaded, and the rule is that the ring hides then (it used to
         * draw a lone progress segment here, which read as a half-filled ring
         * on an idle page). Zero segments keeps the container hidden. */
        s_nsegs = 0;
        return;
    }
    s_nsegs = nina_plan_segments(&sb->st->plan, frac, s_segs, NINA_SEG_MAX);
}

/* Everything the drawn objects depend on: count, weights, item identity and
 * the round boundaries. State and colour are repaints, not rebuilds, so a sub
 * finishing never destroys the objects (and never drops the in-flight hold). */
static uint32_t sb_fingerprint(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < s_nsegs; i++) {
        uint32_t v = (uint32_t)(s_segs[i].item + 1) * 131u
                   + (uint32_t)s_segs[i].weight * 7u
                   + (s_segs[i].round_last ? 1u : 0u);
        h = (h ^ v) * 16777619u;
    }
    return h ^ (uint32_t)s_nsegs;
}

/* Wider gap after segment @p i. A round boundary takes it, but only when a
 * round is more than one segment: one item per round (and the collapsed
 * per-round list) would otherwise widen every joint, so those keep the
 * every-Nth rule the row has always used. */
static bool sb_wide_gap_after(int i) {
    if (i < 0 || i >= s_nsegs - 1) return false;
    if (s_nsegs > 1 && !s_segs[0].round_last) return s_segs[i].round_last;
    return (((i + 1) % NINA_SUBBAR_GROUP_EVERY) == 0);
}

/* ---- ring geometry ------------------------------------------------------ */

/* Angles of segment @p i, degrees clockwise from twelve o'clock. Spans are
 * proportional to weight; the joints take a tight gap, a round boundary the
 * wide one (three times it, mirroring the row's 3 px and 9 px), and no span is
 * narrower than about SB_MIN_PX of arc. The gaps sit at the joints only, so
 * the last segment ends exactly at the far side of the crown opening and the
 * ring's two ends mirror the rim arc's. */
static void sb_ring_geom(const nina_subbar_t *sb, int i, int *a0, int *a1) {
    *a0 = (int)(sb->ring_gap * 0.5f + 0.5f);
    *a1 = *a0 + 1;
    const int n = s_nsegs;
    if (n <= 0 || i < 0 || i >= n) return;

    const float span = (sb->ring_span > 1.0f) ? sb->ring_span : 1.0f;
    float gap = span / (float)n * 0.18f;
    if (gap > 5.0f) gap = 5.0f;

    /* Gap budget first: the spans share whatever is left. A very long list
     * would spend the whole ring on gaps, so they are capped at half of it. */
    float gaps = 0.0f;
    for (int k = 0; k < n - 1; k++) gaps += sb_wide_gap_after(k) ? gap * 3.0f : gap;
    if (gaps > span * 0.5f) {
        gap  *= (span * 0.5f) / gaps;
        gaps  = span * 0.5f;
    }
    const float draw = span - gaps;

    float total = 0.0f;
    for (int k = 0; k < n; k++) {
        total += (float)((s_segs[k].weight > 0) ? s_segs[k].weight : 1);
    }
    if (total <= 0.0f) total = (float)n;

    /* SB_MIN_PX of arc as a floor, then one scale pass in case the floors
     * together ask for more than the ring has left. */
    float min_deg = (sb->ring_radius > 0)
                    ? (SB_DEG_PER_RAD * (float)SB_MIN_PX / (float)sb->ring_radius)
                    : 1.0f;
    float need = 0.0f;
    for (int k = 0; k < n; k++) {
        float d = draw * (float)((s_segs[k].weight > 0) ? s_segs[k].weight : 1) / total;
        need += (d < min_deg) ? min_deg : d;
    }
    const float scale = (need > draw && need > 0.0f) ? (draw / need) : 1.0f;

    float start = sb->ring_gap * 0.5f;
    float w = 1.0f;
    for (int k = 0; k <= i; k++) {
        float d = draw * (float)((s_segs[k].weight > 0) ? s_segs[k].weight : 1) / total;
        if (d < min_deg) d = min_deg;
        d *= scale;
        if (k == i) {
            w = d;
            break;
        }
        start += d + (sb_wide_gap_after(k) ? gap * 3.0f : gap);
    }
    *a0 = (int)(start + 0.5f);
    *a1 = (int)(start + w + 0.5f);
    if (*a1 <= *a0) *a1 = *a0 + 1;
}

/* Stale cue, ring mode only: every arc opacity scaled to 40 %. The flex row
 * never sets sb->stale, so the square ledge keeps its shipped opacities. */
static lv_opa_t sb_stale_opa(const nina_subbar_t *sb, lv_opa_t opa) {
    if (!sb->stale) return opa;
    return (lv_opa_t)(((uint32_t)opa * LV_OPA_40) / LV_OPA_COVER);
}

/* The one place a segment's colour reaches pixels, in either mode. A style
 * write invalidates even when the value is unchanged and this panel is
 * full-refresh, so both values are compared first. */
static void sb_style_block(const nina_subbar_t *sb, lv_obj_t *b,
                           uint32_t color, lv_opa_t opa) {
    const lv_color_t c = lv_color_hex(color);
    const lv_opa_t o = sb->ring ? sb_stale_opa(sb, opa) : opa;
    if (sb->ring) {
        if (!lv_color_eq(lv_obj_get_style_arc_color(b, LV_PART_MAIN), c)) {
            lv_obj_set_style_arc_color(b, c, LV_PART_MAIN);
        }
        if (lv_obj_get_style_arc_opa(b, LV_PART_MAIN) != o) {
            lv_obj_set_style_arc_opa(b, o, LV_PART_MAIN);
        }
    } else {
        if (!lv_color_eq(lv_obj_get_style_bg_color(b, 0), c)) {
            lv_obj_set_style_bg_color(b, c, 0);
        }
        if (lv_obj_get_style_bg_opa(b, 0) != o) {
            lv_obj_set_style_bg_opa(b, o, 0);
        }
    }
}

/* Fraction of the running segment the fill covers: the whole images it has
 * already finished plus the live exposure fraction of the one in flight. */
static float sb_within(const nina_subbar_t *sb, float frac) {
    const int w = (sb->act_weight > 0) ? sb->act_weight : 1;
    float within = sb->act_base + frac / (float)w;
    if (within < 0.0f) within = 0.0f;
    if (within > 1.0f) within = 1.0f;
    return within;
}

/* Open the in-flight arc to @p frac of the running segment's sweep. */
static void sb_ring_fill_angles(nina_subbar_t *sb, float frac) {
    if (!sb->fill || sb->active_idx < 0) return;
    int span = (int)((float)(sb->act_a1 - sb->act_a0) * frac + 0.5f);
    if (span < 1) {
        lv_obj_add_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* lv_arc_set_rotation invalidates even for an unchanged value (the angle
     * setters and the flag calls compare first themselves). */
    const int32_t rot = (270 + sb->act_a0) % 360;
    if (lv_arc_get_rotation(sb->fill) != rot) lv_arc_set_rotation(sb->fill, rot);
    lv_arc_set_bg_angles(sb->fill, 0, span);
    lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
}

/* Row form: lv_obj_set_width invalidates even for an unchanged value, and the
 * 200 ms tick lands on the same whole percent most of the time. */
static void sb_fill_width_pct(nina_subbar_t *sb, int pct) {
    const int32_t w = LV_PCT(pct);
    if (lv_obj_get_style_width(sb->fill, 0) != w) lv_obj_set_width(sb->fill, w);
}

/* ── segment objects ─────────────────────────────────────────────────────── */

/* Realize exactly the objects the current segment list needs. An empty list
 * (the plan hides, or nothing has been polled yet) realizes none and the
 * container goes hidden. */
static void sb_rebuild_blocks(nina_subbar_t *sb) {
    if (!sb->cont) return;

    /* lv_obj_clean deletes the segments and, with them, the fill that lives
     * inside the running one. Drop the pointer before it dangles. */
    sb->fill = NULL;
    lv_obj_clean(sb->cont);
    for (int i = 0; i < NINA_SUBBAR_MAX_BLOCKS; i++) sb->st->blocks[i] = NULL;

    int n = s_nsegs;
    if (n > NINA_SUBBAR_MAX_BLOCKS) n = NINA_SUBBAR_MAX_BLOCKS;
    if (n < 0) n = 0;

    sb->n_blocks    = n;
    sb->active_idx  = -1;
    sb->act_base    = 0.0f;
    sb->act_weight  = 1;
    sb->ring_frac   = 0.0f;
    sb->last_frac   = 0.0f;
    sb->hold_within = 0.0f;
    sb_apply_visibility(sb);

    for (int i = 0; i < n; i++) {
        lv_obj_t *b;
        if (sb->ring) {
            int a0, a1;
            sb_ring_geom(sb, i, &a0, &a1);
            b = ui_dial_arc(sb->cont, sb->ring_radius, sb->ring_width, a0, a1);
        } else {
            int w = s_segs[i].weight;
            if (w < 1)   w = 1;
            if (w > 255) w = 255;      /* lv_obj_set_flex_grow takes a uint8_t */
            b = lv_obj_create(sb->cont);
            lv_obj_remove_style_all(b);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            /* Flex sizes a grown item from its MIN_WIDTH up, so that is where
             * the 4 px floor has to live; the width is the pre-layout size. */
            lv_obj_set_style_min_width(b, SB_MIN_PX, 0);
            lv_obj_set_width(b, SB_MIN_PX);
            lv_obj_set_flex_grow(b, (uint8_t)w);
            lv_obj_set_height(b, sb->block_h);
            lv_obj_set_style_radius(b, SB_BLOCK_RADIUS, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_clip_corner(b, true, 0);
            if (i < n - 1) {
                lv_obj_set_style_margin_right(
                    b, sb_wide_gap_after(i) ? SB_GAP_WIDE : SB_GAP_TIGHT, 0);
            }
        }
        sb->st->blocks[i] = b;
    }
}

static void sb_paint_blocks(nina_subbar_t *sb) {
    int n = (sb->n_blocks < s_nsegs) ? sb->n_blocks : s_nsegs;
    int active = -1;
    uint32_t act_color = 0;

    for (int i = 0; i < n; i++) {
        if (!sb->st->blocks[i]) continue;
        const nina_seg_t *s = &s_segs[i];
        uint32_t c = sb_seg_color(sb, s);
        lv_opa_t opa;
        switch (s->state) {
        case NINA_SEG_DONE:
            opa = (s->item < 0) ? SB_ROUND_OPA : LV_OPA_COVER;
            break;
        case NINA_SEG_ACTIVE:
            opa = SB_DIM_OPA;           /* dim base, the fill rides on top */
            if (active < 0) {
                active = i;
                act_color = c;
            }
            break;
        case NINA_SEG_PLACEHOLDER:
            c = SB_REMAIN_COLOR;
            opa = SB_PLACEHOLDER_OPA;   /* a disabled item never fills */
            break;
        case NINA_SEG_REMAINING:
        default:
            /* c is already this segment's own colour from sb_seg_color()
             * above (the item's filter colour, or the neutral colour for the
             * item -1 overflow segment): just dim it as a preview. */
            opa = SB_REMAIN_OPA;
            break;
        }
        sb_style_block(sb, sb->st->blocks[i], c, opa);
    }

    if (active >= 0) {
        sb->act_base   = s_segs[active].done_frac;
        sb->act_weight = (s_segs[active].weight > 0) ? s_segs[active].weight : 1;
        if (sb->ring) sb_ring_geom(sb, active, &sb->act_a0, &sb->act_a1);
    }
    sb_place_fill(sb, active, act_color);
}

/* Move the single fill object into segment @p idx (-1 hides it). */
static void sb_place_fill(nina_subbar_t *sb, int idx, uint32_t color) {
    if (idx < 0 || idx >= sb->n_blocks || !sb->st->blocks[idx]) {
        if (sb->fill) lv_obj_add_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
        sb->active_idx = -1;
        return;
    }
    const lv_color_t c = lv_color_hex(color);
    if (sb->ring) {
        if (!sb->fill || sb->active_idx != idx) {
            if (!sb->fill) {
                sb->fill = ui_dial_arc(sb->cont, sb->ring_radius,
                                       sb->ring_width, 0, 1);
            }
            sb->active_idx  = idx;
            sb->ring_frac   = sb->act_base;
            /* The segment identity changed: the hold has done its job. */
            sb->hold_within = 0.0f;
            sb->last_frac   = 0.0f;
        }
        const lv_opa_t o = sb_stale_opa(sb, SB_FILL_OPA);
        if (lv_obj_get_style_arc_opa(sb->fill, LV_PART_MAIN) != o) {
            lv_obj_set_style_arc_opa(sb->fill, o, LV_PART_MAIN);
        }
        if (!lv_color_eq(lv_obj_get_style_arc_color(sb->fill, LV_PART_MAIN), c)) {
            lv_obj_set_style_arc_color(sb->fill, c, LV_PART_MAIN);
        }
        if (sb->ring_frac < sb->act_base) sb->ring_frac = sb->act_base;
        sb_ring_fill_angles(sb, sb->ring_frac);
        return;
    }
    if (!sb->fill) {
        sb->fill = lv_obj_create(sb->st->blocks[idx]);
        lv_obj_remove_style_all(sb->fill);
        lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(sb->fill, SB_BLOCK_RADIUS, 0);
        lv_obj_set_style_bg_opa(sb->fill, SB_FILL_OPA, 0);
        lv_obj_set_size(sb->fill, 0, LV_PCT(100));
        lv_obj_align(sb->fill, LV_ALIGN_LEFT_MID, 0, 0);
        sb->active_idx  = idx;
        sb->hold_within = 0.0f;
        sb->last_frac   = 0.0f;
        sb_fill_width_pct(sb, (int)(sb->act_base * 100.0f + 0.5f));
    } else if (sb->active_idx != idx) {
        lv_obj_set_parent(sb->fill, sb->st->blocks[idx]);
        lv_obj_align(sb->fill, LV_ALIGN_LEFT_MID, 0, 0);
        sb_fill_width_pct(sb, (int)(sb->act_base * 100.0f + 0.5f));
        sb->active_idx = idx;
        /* The segment identity changed: the hold has done its job. */
        sb->hold_within = 0.0f;
        sb->last_frac   = 0.0f;
    }
    if (!lv_color_eq(lv_obj_get_style_bg_color(sb->fill, 0), c)) {
        lv_obj_set_style_bg_color(sb->fill, c, 0);
    }
    lv_obj_remove_flag(sb->fill, LV_OBJ_FLAG_HIDDEN);
}

/* ── public API ──────────────────────────────────────────────────────────── */

void nina_subbar_create(nina_subbar_t *sb, lv_obj_t *parent, int block_h) {
    if (!sb || !parent) return;
    memset(sb, 0, sizeof(*sb));
    sb->st = sb_store_get(sb);
    if (!sb->st) return;             /* no PSRAM: cont stays NULL, every entry point bails */
    memset(sb->st, 0, sizeof(*sb->st));   /* a rebuild's old object pointers are dead */
    sb->block_h    = (block_h > 0) ? block_h : 12;
    sb->active_idx = -1;
    sb->act_weight = 1;
    sb->shown      = true;
    sb->cached_target = -1;
    sb->cached_done   = -1;

    sb->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(sb->cont);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(sb->cont, LV_PCT(100));
    lv_obj_set_height(sb->cont, sb->block_h);
    lv_obj_set_style_pad_all(sb->cont, 0, 0);
    lv_obj_set_style_pad_gap(sb->cont, 0, 0);
    lv_obj_set_flex_flow(sb->cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb->cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    sb_build_segs(sb, 0.0f);
    sb_rebuild_blocks(sb);
    nina_subbar_apply_theme(sb);
}

void nina_subbar_create_ring(nina_subbar_t *sb, lv_obj_t *parent,
                             int radius, int width, int gap_deg) {
    if (!sb || !parent || radius <= 0 || width <= 0) return;
    memset(sb, 0, sizeof(*sb));
    sb->st = sb_store_get(sb);
    if (!sb->st) return;             /* no PSRAM: cont stays NULL, every entry point bails */
    memset(sb->st, 0, sizeof(*sb->st));   /* a rebuild's old object pointers are dead */
    sb->block_h    = width;
    sb->active_idx = -1;
    sb->act_weight = 1;
    sb->shown      = true;
    sb->cached_target = -1;
    sb->cached_done   = -1;
    sb->ring        = true;
    sb->ring_radius = radius;
    sb->ring_width  = width;
    sb->ring_gap    = (float)gap_deg;
    sb->ring_span   = 360.0f - (float)gap_deg;

    int side = 2 * radius + width;
    sb->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(sb->cont);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sb->cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sb->cont, side, side);
    lv_obj_center(sb->cont);
    lv_obj_set_style_pad_all(sb->cont, 0, 0);

    sb_build_segs(sb, 0.0f);
    sb_rebuild_blocks(sb);
    nina_subbar_apply_theme(sb);
}

void nina_subbar_ring_set_radius(nina_subbar_t *sb, int radius) {
    if (!sb || !sb->ring || !sb->cont || radius <= 0 || radius == sb->ring_radius) return;
    sb->ring_radius = radius;
    int side = 2 * radius + sb->ring_width;
    lv_obj_set_size(sb->cont, side, side);
    lv_obj_center(sb->cont);
    /* The segments are arcs sized from ring_radius at rebuild time: invalidate
     * the geometry cache so the next update rebuilds and repaints them. */
    sb->cached_target = -1;
}

/* Leaves the ratchet armed on purpose: the camera reports Idle between subs,
 * which is exactly when the dashboard fires this reset, and the hold has to
 * survive that gap to catch the next sub's restart on the same segment. */
void nina_subbar_reset_elapsed(nina_subbar_t *sb) {
    if (!sb || !sb->elapsed_cb) return;
    sb->elapsed_cb(sb->elapsed_ud, -1);
}

void nina_subbar_set_shown(nina_subbar_t *sb, bool shown) {
    if (!sb) return;
    sb->shown = shown;
    sb_apply_visibility(sb);
}

void nina_subbar_park(nina_subbar_t *sb) {
    if (!sb) return;
    sb->hold_within = 0.0f;
    sb->last_frac   = 0.0f;
    nina_subbar_set_progress(sb, 0.0f);
}

void nina_subbar_update(nina_subbar_t *sb, const nina_client_t *d,
                        int instance_idx, int gb) {
    if (!sb || !sb->cont || !d || !current_theme) return;

    const char *filter = (d->current_filter[0] != '\0') ? d->current_filter : "--";
    sb->instance_idx = instance_idx;
    sb->gb = gb;

    nina_plan_t *pl = &sb->st->plan;
    if (d->plan.n_items > 0) {
        memcpy(pl, &d->plan, sizeof(*pl));
        sb->unknown = false;
    } else {
        /* No plan: a source that fills only the legacy per-filter counts (demo
         * mode, or a container the parser found nothing in). One image per
         * round of the running filter reproduces exactly the block row this
         * widget drew before the plan existed. */
        int target = d->exposure_iterations;
        int done   = d->exposure_count;
        if (done < 0) done = 0;
        memset(pl, 0, sizeof(*pl));
        pl->n_items     = 1;
        pl->running_idx = 0;
        pl->items[0].iterations = 1;
        pl->items[0].enabled    = true;
        pl->items[0].running    = true;
        snprintf(pl->items[0].filter, sizeof(pl->items[0].filter), "%s", filter);
        sb->unknown = (target <= 0);
        if (sb->unknown) {
            pl->rounds       = 1;
            pl->total_images = 1;
        } else {
            pl->rounds       = target;
            pl->round_done   = (done < target) ? done : (target - 1);
            pl->total_images = target;
            /* All of them done: nothing is running, so the last segment reads
             * DONE (solid) and the fill hides, which is how this widget has
             * always drawn a finished target. */
            if (done >= target) {
                pl->items[0].completed = 1;
                pl->items[0].running   = false;
                pl->running_idx        = -1;
            }
        }
    }

    sb_build_segs(sb, sb->last_frac);
    uint32_t fp = sb_fingerprint();
    if (fp != sb->fp || sb->cached_target < 0) {
        sb_rebuild_blocks(sb);
        sb->fp = fp;
    }
    sb_paint_blocks(sb);

    sb->cached_target = pl->total_images;
    sb->cached_done   = (d->exposure_count > 0) ? d->exposure_count : 0;
    sb->cached_block_color = sb_block_color(filter, instance_idx, gb);
    snprintf(sb->cached_filter, sizeof(sb->cached_filter), "%s", filter);

    /* Elapsed seconds belong to nina_subbar_set_progress() (the 200 ms tick);
     * only the cached length is refreshed here. */
    sb->cached_total = d->exposure_total;
}

void nina_subbar_set_elapsed_cb(nina_subbar_t *sb, nina_subbar_elapsed_cb_t cb, void *ud) {
    if (!sb) return;
    sb->elapsed_cb = cb;
    sb->elapsed_ud = ud;
}

void nina_subbar_set_progress(nina_subbar_t *sb, float frac) {
    if (!sb || !sb->cont) return;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    if (sb->fill && sb->active_idx >= 0) {
        float within = sb_within(sb, frac);

        /* Two-tier lag: the done count arrives on the 15 s sequence poll while
         * frac arrives on the 2 s camera poll, so the next exposure starts
         * while the finished one still owns the fill. A near-complete fraction
         * collapsing is that boundary, so hold the fill where it was drawn
         * until the count catches up and moves it on. A drop from a low value
         * is a genuinely aborted sub and is drawn as is. The 0.9 floor leaves
         * room for the last 200 ms tick of a short sub. */
        if (sb->last_frac >= 0.9f && frac < sb->last_frac - 0.5f) {
            sb->hold_within = sb_within(sb, sb->last_frac);
            sb->hold_tick   = lv_tick_get();
        }
        /* The count lands within the 15 s tier or not at all: past
         * NINA_SUBBAR_HOLD_MS the restart is a retry of the same sub (abort,
         * reconnect) and the fill must follow it. */
        if (sb->hold_within > 0.0f
            && lv_tick_elaps(sb->hold_tick) > NINA_SUBBAR_HOLD_MS) {
            sb->hold_within = 0.0f;
        }
        if (sb->hold_within > 0.0f) {
            if (within < sb->hold_within) within = sb->hold_within;
            else                          sb->hold_within = 0.0f;
        }

        if (sb->ring) {
            sb->ring_frac = within;
            sb_ring_fill_angles(sb, within);
        } else {
            sb_fill_width_pct(sb, (int)(within * 100.0f + 0.5f));
        }
    }
    sb->last_frac = frac;

    if (sb->elapsed_cb && sb->cached_total > 0.0f) {
        int secs = (int)(frac * sb->cached_total + 0.5f);
        if (secs > 9999) secs = 9999;
        sb->elapsed_cb(sb->elapsed_ud, secs);
    }
}

void nina_subbar_set_stale(nina_subbar_t *sb, bool stale) {
    if (!sb || !sb->ring || sb->stale == stale) return;
    sb->stale = stale;
    sb_build_segs(sb, sb->last_frac);
    sb_paint_blocks(sb);
}

void nina_subbar_apply_theme(nina_subbar_t *sb) {
    if (!sb || !sb->cont || !current_theme) return;
    sb->gb = app_config_get()->color_brightness;

    /* Re-derive every segment from the cached plan so a theme or brightness
     * change repaints without waiting for the next poll. */
    sb->cached_block_color = sb_block_color(sb->cached_filter, sb->instance_idx, sb->gb);
    sb_build_segs(sb, sb->last_frac);
    sb_paint_blocks(sb);
}
