#pragma once

/**
 * @file nina_image_page.h
 * @brief Image page spine: ONE implementation, SIX instances (GOES, Moon,
 *        Solar, Custom Image, Weather Radar, Clouds). Each instance is a first-class page
 *        with its own runtime index (PAGE_IDX_IMG_*), LVGL page, decoded frame,
 *        poller and gates. Rendering lives in ui/nina_image_page.c; the pollers
 *        live in image_page_poll.c and are built on the poll_task spine.
 *
 * Lifetime / gating (page-gated, OctoPrint pattern, retained frames):
 *   - active: the page is visible (registry ops show/hide, UI task).
 *   - warm:   the page is the NEXT slideshow stop (image_page_prefetch, arbiter).
 *   - poll_gate = active || warm: the poll_task spine gate.
 *   - The decoded frame is RETAINED on leave (manual swipe-back re-shows it
 *     instantly). Cap: at most IMAGE_PAGE_MAX_RESIDENT (3) frames across the six
 *     instances; active and warm instances are pinned, and when a commit would
 *     exceed the cap the least-recently-shown other instance's frame is evicted
 *     (image_page_evict_if_over_cap). image_page_disable() frees a frame outright.
 *   - At most one download+decode is in flight across the six pollers
 *     (s_fetch_gate in image_page_poll.c), matching today's single fetch task.
 *
 * Locks: LVGL display lock OUTSIDE, frame_mux INSIDE. Every image_page_*
 * function documented "display lock held" must be called with bsp_display_lock
 * held by the caller; none of them take it. image_page_commit_frame() and
 * image_page_set_error() take the display lock themselves (called from the
 * pollers) and therefore must NOT be called with it held.
 */

#include "lvgl.h"
#include "app_config.h"
#include "goes_client.h"          /* image_frame_t, image_fetch_* */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_SRC_GOES   = 0,
    IMG_SRC_MOON   = 1,
    IMG_SRC_SOLAR  = 2,
    IMG_SRC_CUSTOM = 3,
    IMG_SRC_RADAR  = 4,
    IMG_SRC_CLOUDS = 5,
    IMG_SRC_COUNT  = 6,
} image_src_t;

/* Sources whose frames live in the animation ring (index 0 = newest, played
 * oldest -> newest on an LVGL timer) instead of a single p->frame. Every ring
 * gate in nina_image_page.c / image_page_poll.c keys off this predicate, never
 * off a source id, so a further animated source is one enum value + hooks. */
static inline bool image_src_is_animated(image_src_t s)
{
    return s == IMG_SRC_RADAR || s == IMG_SRC_CLOUDS;
}

/* Sources whose fetch failure is rendered as the page caption (user-addressed
 * or outage-prone): Custom Image, Radar (site token) and Clouds (GIBS). */
static inline bool image_page_shows_error(image_src_t s)
{
    return s == IMG_SRC_CUSTOM || image_src_is_animated(s);
}

typedef struct image_page {
    /* identity: constant after image_page_init() */
    image_src_t       src;
    int               page_idx;        /* PAGE_IDX_IMG_GOES + src */
    const char       *name;            /* task/log name: "img_goes" .. "img_clouds" */

    /* decoded frame, owned here, guarded by frame_mux */
    SemaphoreHandle_t frame_mux;
    image_frame_t     frame;

    /* gates (cross-core atomics) */
    _Atomic bool      active;          /* page visible */
    _Atomic bool      warm;            /* next slideshow stop: keep frame, keep polling */
    _Atomic bool      poll_gate;       /* active || warm (poll_task spine gate) */
    _Atomic bool      manual_fetch;    /* next fetch shows the loading overlay + re-downloads */
    TaskHandle_t      task;            /* poller handle, NULL until spawned */
    portMUX_TYPE      spawn_mux;
    int64_t           last_shown_ms;   /* esp_timer ms of the last activate; eviction order (0 = never shown) */

    /* LVGL state (display lock held by the caller for every access) */
    lv_obj_t *root, *img_front, *img_back, *overlay_bar, *lbl_region, *lbl_timestamp;
    lv_obj_t *loading;                 /* animated pages: pulsing placeholder until the loop has a few frames; else NULL */
    lv_obj_t *lbl_moon_age, *lbl_moon_next, *lbl_moon_rise, *lbl_moon_set;   /* Moon instance only, else NULL */
    /* Round-only shape handles: the Moon page draws illumination as a rim arc
     * instead of a percentage, and the C2 tap hides it with the captions. The
     * page repaints both from the theme (moon_arc_apply_theme). NULL on square
     * and on the five non-Moon sources; every use is guarded. */
    lv_obj_t *moon_illum_arc;
    lv_obj_t *moon_illum_tick;
    /* Last text written to lbl_region [0] and lbl_timestamp [1]. lv_arclabel
     * has no text getter and its setter reallocates and invalidates on every
     * call, while the ring playback rewrites lbl_region every 400 ms with
     * unchanged text, so the dedupe compares against this shadow instead of
     * reading the widget. 64 bytes covers every caption source in the file
     * (label_copy[48], err_copy[48], ts[32], name[24], pct[16]). */
    char caption_shadow[2][64];
    /* On-screen picture geometry. 0 = scale the decoded picture to the panel
     * width and centre it vertically, which is what every source does on
     * square. The round Moon builder sets 432 / -12 so the disc leaves a black
     * annulus for the label chords. Absolute pixels at both round widths. */
    int fit_px;
    int fit_dy;
    lv_image_dsc_t dsc_a, dsc_b;
    bool      dsc_a_borrowed, dsc_b_borrowed, front_is_a, crossfade_active, force_redraw;
    int64_t   displayed_stamp_ms;      /* stamp of the frame currently on screen; 0 = none */
    uint16_t *moon_copy_buf[2];        /* Moon SW-scale fallback copies */
    size_t    moon_copy_cap[2];
} image_page_t;

#define IMAGE_PAGE_MAX_RESIDENT 3   /* decoded frames resident across the six instances */

/* ── Instances ── */
image_page_t *image_page_get(image_src_t src);
image_page_t *image_page_by_page_idx(int page_idx);          /* NULL if not an image page */

/* Create the six instances' identities + mutexes (spawn_pollers=false) or
 * additionally start a poller for every source enabled in config
 * (spawn_pollers=true). Idempotent. MUST be called once with false from
 * app_main() right after app_config_init() (before the web server and before
 * create_nina_dashboard(): both can reach image_page_get() from other tasks),
 * and again with true from data_update_task init on the normal path. */
void image_page_init(bool spawn_pollers);
void image_page_ensure_task_running(image_page_t *p);      /* spawn if enabled in config; idempotent */
void image_page_wake(image_page_t *p);                       /* xTaskNotifyGive the poller (config change) */

/* ── Lifecycle (page-gated) ── */
void image_page_set_active(image_page_t *p, bool active);   /* registry ops show/hide; display lock held */
void image_page_prefetch(int page_idx);                       /* warm exactly this page (or none for -1); no lock */
void image_page_disable(image_page_t *p);                     /* config disable: clear warm, drop frame; no lock */
void image_page_request_manual_fetch(image_page_t *p);        /* loading overlay + fresh download on next poll */

/* ── Config accessors (per source) ── */
bool     image_page_config_enabled(const app_config_t *c, image_src_t src);
bool     image_page_config_overlay(const app_config_t *c, image_src_t src);
bool     image_page_config_crop(const app_config_t *c, image_src_t src);
uint32_t image_page_interval_ms(image_page_t *p);            /* live config, clamped; Moon: 3000 while the clock is invalid */
void     image_page_label(image_page_t *p, char *out, size_t sz);   /* region name / "Moon" / band label / "Custom" / "Radar <token>" / "Cloud Cover <channel>" */
/* Radar site token for THIS fetch, resolved fresh every time and never
 * persisted (a poll task must not write config): an explicit radar_token wins,
 * else the WSR-88D site nearest the configured weather location, else the
 * national mosaic "CONUS". @p sz should be at least 16. */
void     image_page_radar_token(const app_config_t *c, char *out, size_t sz);
bool     image_page_get_error(image_page_t *p, char *out, size_t sz); /* true if a non-empty error_msg was copied */

/* ── Frame commit from the poller (NOT under the display lock) ── */
/* Takes ownership of fresh->buf (moved into p->frame under frame_mux, the
 * previous buffer freed), then, if the page is active, takes the display lock
 * and renders. A frame that lands after the gate closed (page left / un-warmed
 * mid-fetch) is still STORED, never dropped: it is retained so a swipe-back
 * re-shows it instantly, and the resident cap (enforced right after every
 * commit) evicts it if the budget is exceeded. The buffer is freed only when
 * frame_mux cannot be taken. force=true bypasses the newer-stamp gate and
 * swaps instantly (Moon). */
void image_page_commit_frame(image_page_t *p, image_frame_t *fresh, bool force);
void image_page_set_error(image_page_t *p, const char *msg);   /* stores error_msg; Custom/Radar/Clouds render it as the caption */

/* ── Animation ring (image_src_is_animated: Weather Radar, Clouds) ──
 * An animated page stores up to N decoded stills (index 0 = newest; N =
 * radar_frames / clouds_frames) and plays them oldest -> newest on an LVGL
 * timer, instead of keeping a single frame in p->frame (which stays NULL for
 * these sources). One ring per instance. A full ring is 6-10 MB, so it is freed
 * whenever the page is deactivated or disabled and rebuilt by the poller's
 * backfill on the next activation. Guarded by the display lock alone; see the
 * ring block in nina_image_page.c. Every function is a no-op (or returns 0 /
 * false) for a single-frame source, so callers need no source test. */
/* Insert a decoded frame, taking ownership of fresh->buf. Takes the display
 * lock itself, so it must NOT be called with the lock held.
 *
 * @p stamp is the frame's own time (UTC seconds) or 0 when unknown. A stamped
 * frame is placed by time (newest first) whatever @p at_head says; a stamp the
 * ring already holds REPLACES that slot when the bytes differ (GIBS serves the
 * newest frames partially at first) and is dropped as a duplicate when they
 * match. For an unstamped frame at_head=true is the steady-state newest-frame
 * push (evicts the oldest when full); at_head=false appends an older backfill
 * frame at the tail (ignored when full). A frame that hashes equal to the
 * entry it would sit next to is dropped.
 *
 * @p gen is the ring generation the fetch was issued under, from
 * image_page_ring_gen(). A frame whose generation no longer matches is FREED
 * and dropped: this is the single point that keeps frames from a region the
 * user has already left out of the ring. Producers must read the generation
 * BEFORE the token/times they fetch with (see radar_frame_is_stale in radar_play.h).
 *
 * @p src / @p src_len are the frame's COMPRESSED bytes from
 * image_fetch_custom_retain() (may be NULL/0). The ring retains them so a crop,
 * dark-mode or Red Night change can re-derive the pixels locally instead of
 * re-downloading the whole ring; they also serve as the dedupe key.
 *
 * Ownership of BOTH fresh->buf and src is taken on every path, accepted or
 * rejected: the caller must not free either after this returns.
 *
 * Calls nav_arbiter_notify_content_ready() when a frame lands on the visible
 * page, so the slideshow dwell starts once the picture is up (same contract as
 * image_page_commit_frame). */
void image_page_ring_add(image_page_t *p, image_frame_t *fresh, bool at_head, uint32_t gen,
                         uint8_t *src, size_t src_len, uint32_t stamp);
/* Re-derive every resident frame from its retained compressed bytes under the
 * CURRENT bake (radar crop / dark mode) and theme (Red Night). No network.
 * Request from any task (sets a flag + wakes the poller); the work runs on the
 * page's poll task, one frame at a time, taking the display lock only for the
 * pointer swaps. */
void image_page_ring_request_retransform(image_page_t *p);
void image_page_ring_retransform_if_requested(image_page_t *p);   /* the page's poll task only */
void image_page_ring_reset(image_page_t *p);        /* free the ring + bump the generation; display lock HELD by the caller */
/* Supersede the ring: bumps the generation UNCONDITIONALLY (so no in-flight frame
 * from the old settings can enter), then frees it. Takes the display lock; if that
 * times out the teardown is deferred to image_page_ring_reset_if_requested(). */
void image_page_ring_invalidate(image_page_t *p);
void image_page_ring_reset_if_requested(image_page_t *p);          /* the page's poll task only */
uint32_t image_page_ring_gen(image_page_t *p);      /* current ring generation; read BEFORE resolving the token/times */
int  image_page_ring_count(image_page_t *p);        /* resident frames (lock-free) */
int  image_page_ring_capacity(image_page_t *p);     /* radar_frames / clouds_frames, clamped 1..RADAR_RING_MAX */
bool image_page_ring_backfill_take(image_page_t *p);   /* consume the pending-backfill request */
bool image_page_ring_has_stamp(image_page_t *p, uint32_t stamp);   /* lock-free; a held stamp needs no re-download */
/* Run @p fn on the pixels of the resident frame nearest in time to @p stamp
 * (a different stamp preferred over the same one; current bake only) with the
 * display lock held for the duration, so the slot cannot be freed under it.
 * Returns fn's result, or false when the ring has no usable neighbour. */
bool image_page_ring_with_neighbour(image_page_t *p, uint32_t stamp,
                                    bool (*fn)(const uint16_t *ref, int w, int h, void *arg),
                                    void *arg);
/* Re-judge the HEAD slot (index 0, the newest) with @p fn — the one frame the
 * insert gate cannot judge, because every page entry frees the ring and the head
 * lands in it with no neighbour. @p fn is handed the NEIGHBOUR's pixels and, as
 * its `arg`, an image_frame_t view of the head slot, so the caller reuses its
 * insert-time adapter unchanged; the display lock is held for the duration.
 * Returns the head's stamp when fn says the head is bad (feed it to
 * image_page_ring_drop_stamp()), 0 otherwise. */
uint32_t image_page_ring_judge_head(image_page_t *p,
                                    bool (*fn)(const uint16_t *ref, int w, int h, void *arg));
/* Free the slot carrying @p stamp so the next newest fetch re-downloads it and
 * the same-stamp path in _add installs the completed frame. Takes the display
 * lock itself; the page's poll task only. Compacts the slots above it down and
 * moves the playback cursor with them. false when @p stamp is 0, no slot carries
 * it, or the ring holds one frame or none. Does NOT re-arm the backfill. */
bool image_page_ring_drop_stamp(image_page_t *p, uint32_t stamp);
/* Free the OLDEST resident frame (pixels + retained source) to give the next
 * decode room. Takes the display lock itself; the page's poll task only, same as
 * image_page_ring_add(). Returns false — freeing nothing — when the ring holds
 * one frame or none: the newest must survive so the page never goes blank.
 * Does NOT re-arm the backfill (it assumes an empty ring); a shrunk ring stays
 * shorter until the next page activation rebuilds it. */
bool image_page_ring_drop_oldest(image_page_t *p);
/* Enforce IMAGE_PAGE_MAX_RESIDENT: while more than the cap are resident, free the
 * frame of the least-recently-shown instance that is neither active nor warm.
 * Called by image_page_commit_frame after every commit; no display lock. */
void image_page_evict_if_over_cap(void);

/* ── LVGL page (display lock held by the caller) ── */
lv_obj_t *image_page_create(image_page_t *p, lv_obj_t *parent);
/* On-screen width the decoded picture is scaled to, and the vertical offset of
 * its centre from the panel centre. The PPA fit and the software fallback in
 * nina_image_page.c and the Moon poller in image_page_poll.c all size their
 * work from these, so the picture is never scaled back up to the panel. */
int image_page_fit_px(const image_page_t *p);
int image_page_fit_dy(const image_page_t *p);
void image_page_render_frame(image_page_t *p);      /* push p->frame if newer than displayed (or forced) */
void image_page_force_redraw(image_page_t *p);
bool image_page_has_image(image_page_t *p);
void image_page_release_lvgl(image_page_t *p);      /* drop the page's LVGL copies (page leave) */
void image_page_set_overlay_visible(image_page_t *p, bool visible);
void image_page_apply_theme(image_page_t *p);
/* Moon drag frames (see the former nina_image_display_show_scaled/show_borrowed
 * contracts): no-ops unless the page is active. */
void image_page_show_scaled(image_page_t *p, const uint16_t *buf, int w, int h);
void image_page_show_borrowed(image_page_t *p, const uint16_t *buf, int w, int h);

/* ── Live config apply (defined in nina_image_page.c; called by the image
 *    config POST handler, the control registry and config_trigger_side_effects) ── */
void image_page_config_apply_live(const app_config_t *prev, const app_config_t *cur, bool force_fetch);

/* ── Pollers (image_page_poll.c) ── */
void image_page_poll_init(void);                    /* creates the shared fetch gate; called by image_page_init */
void image_page_poll_task(void *arg);               /* arg = image_page_t*; runs the poll_task spine */

/* ── Moon helpers (image_page_poll.c; were in tasks.c) ── */
extern _Atomic bool moon_anim_request;
void moon_caption(char *name_out, size_t name_sz, char *pct_out, size_t pct_sz);
void moon_overlay_info(char *age,  size_t age_sz, char *next, size_t next_sz,
                       char *rise, size_t rise_sz, char *set,  size_t set_sz);

#ifdef __cplusplus
}
#endif
