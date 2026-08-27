/**
 * @file nina_image_page.c
 * @brief Image page spine: rendering + lifecycle for the six image pages
 *        (GOES / Moon / Solar / Custom / Radar / Clouds). Ported from nina_image_display.c;
 *        every former module-level static is now a field of image_page_t.
 *
 * Threading: every LVGL call below runs under the display lock held by the
 * CALLER, except image_page_commit_frame() / image_page_set_error(), which
 * take it themselves (documented in the header). Never take the display lock
 * while holding p->frame_mux.
 */

#include "nina_image_page.h"
#include "nina_wait_overlay.h"
#include "nina_empty_state.h"
#include "nina_nav_arbiter.h"          /* nav_arbiter_notify_content_ready */
#include "nina_dashboard.h"            /* nina_dashboard_set_image_page_enabled (config apply, B5) */
#include "nina_dashboard_internal.h"   /* screen_size(), OUTER_PADDING, current_theme, PAGE_IDX_IMG_* */
#include "image_red_remap.h"
#include "image_fit.h"                 /* PPA fit: n/16 scale + symmetric trim */
#include "image_night_invert.h"        /* radar: invert the greyscale basemap only */
#include "jpeg_utils.h"                /* jpeg_decode_rgb565 (ring local re-transform) */
#include "moon_interaction.h"
#include "radar_sites.h"               /* radar_site_nearest (token resolution) */
#include "radar_play.h"                /* ring size, dedupe hash, playback cursor */
#include "clouds_wms.h"                /* clouds_channel_label (page caption) */
#include "app_config.h"
#include "display_defs.h"
#include "tasks.h"                     /* psram_task_ensure, psram_task_spawn */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>                     /* snprintf (radar caption) */
#include <string.h>
#include <time.h>

static const char *TAG = "image_page";

extern const lv_font_t lv_font_overpass_27;

/* The six instances. Identity fields are filled by image_page_init(). */
static image_page_t s_pages[IMG_SRC_COUNT];

/* Animation ring (Radar, Clouds) — implementation further down, forward-declared
 * here because the render/lifecycle functions above it dispatch into the ring
 * for the animated sources. All require the display lock held by the caller. */
static void ring_show_idx(image_page_t *p, int idx, bool fade);
static void ring_loading_sync(image_page_t *p);
static void ring_timer_sync(image_page_t *p);
static void ring_request_backfill(image_page_t *p);
static bool ring_red_mismatch(image_page_t *p);

static inline void set_label_if_changed(lv_obj_t *label, const char *text)
{
    if (!label) return;
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

static inline bool frame_lock(image_page_t *p, int timeout_ms)
{
    return p->frame_mux && xSemaphoreTake(p->frame_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void frame_unlock(image_page_t *p) { xSemaphoreGive(p->frame_mux); }

static void init_image_dsc(lv_image_dsc_t *dsc)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf    = LV_COLOR_FORMAT_RGB565;
}

/* Vertically centre the scaled image on the square panel. Every source is scaled
 * to the panel WIDTH (see the scale math at each call site), so a source that is
 * wider than it is tall falls short of 720 px high: the 600x392 CONUS radar tile
 * becomes 720x470 and, left at the create-time (0,0), dumped its whole ~250 px
 * of letterbox at the bottom of the screen. Split that gap evenly instead.
 *
 * Square sources (Moon, Solar, GOES full disc) scale to 720x720 and resolve to
 * y = 0, so this is a no-op for them. Only Y moves — X stays at its create-time
 * 0 — and only when the value actually changes, so a stream of same-shaped
 * frames never re-anchors the image (see the render_frame note on why the old
 * code refused a per-update lv_obj_set_pos). The comparison reads back the very
 * style property lv_obj_set_y() writes, not computed geometry, so it cannot go
 * stale before layout runs. Overlay labels are children of the page container,
 * not of the image, so they keep their screen-edge alignment. */
static void center_image_y(lv_obj_t *img, int h, uint16_t scale)
{
    int32_t scaled_h = ((int32_t)h * (int32_t)scale) / 256;
    int32_t y = ((int32_t)screen_size() - scaled_h) / 2;
    if (y < 0) y = 0;
    if (lv_obj_get_style_y(img, LV_PART_MAIN) != y) lv_obj_set_y(img, y);
}

/* Release a slot's descriptor: free the backing buffer ONLY if this page owns
 * it (not borrowed), then clear the descriptor and the borrowed flag. `is_a`
 * selects which slot's borrowed flag to clear. Borrowed buffers belong to the
 * moon-drag scratch and are freed there on page leave. */
static void release_dsc(image_page_t *p, lv_image_dsc_t *dsc, bool is_a)
{
    bool *borrowed = is_a ? &p->dsc_a_borrowed : &p->dsc_b_borrowed;
    if (dsc->data && !*borrowed) {
        heap_caps_free((void *)dsc->data);
    }
    dsc->data       = NULL;
    dsc->data_size  = 0;
    dsc->header.w   = 0;
    dsc->header.h   = 0;
    *borrowed       = false;
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* Runs when the fade-IN of the new image completes. Hides/clears the OLD
 * image (the one that just faded OUT, which is always p->img_front at callback
 * time), frees its buffer, flips the front/back designation, and recomputes
 * the img_front/img_back pointers from the container children. */
static void crossfade_done_cb(lv_anim_t *a)
{
    image_page_t *p = lv_anim_get_user_data(a);
    if (!p) return;

    /* p->img_front is the OLD image that just faded OUT. Its buffer is p->dsc_a
     * when front_is_a, else p->dsc_b. */
    lv_image_dsc_t *old_dsc = p->front_is_a ? &p->dsc_a : &p->dsc_b;

    /* Detach the source BEFORE freeing the buffer so LVGL never dereferences a
     * freed descriptor. release_dsc() skips the free for a borrowed buffer. */
    lv_obj_add_flag(p->img_front, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(p->img_front, NULL);
    release_dsc(p, old_dsc, p->front_is_a);

    p->front_is_a = !p->front_is_a;
    p->img_front  = p->front_is_a ? lv_obj_get_child(p->root, 0)
                                  : lv_obj_get_child(p->root, 1);
    p->img_back   = p->front_is_a ? lv_obj_get_child(p->root, 1)
                                  : lv_obj_get_child(p->root, 0);
    p->crossfade_active = false;
}

/* Cancel an in-flight settle->resting crossfade and finalize it immediately, so a
 * fresh moon-drag frame can take over instead of being dropped. Stops both fade
 * anims (so they never run their exec/completed callbacks against stale state),
 * then performs the SAME retirement crossfade_done_cb would do: the new frame
 * (img_back, faded partway in) is forced fully opaque and becomes the front; the
 * old frame (img_front, faded partway out) is hidden and its buffer retired via
 * release_dsc() (which skips borrowed buffers and frees owned ones — no double-free,
 * no leak). Leaves crossfade_active false with consistent front/back bookkeeping so
 * the next show_borrowed starts clean. Caller holds the display lock. */
static void cancel_moon_crossfade(image_page_t *p)
{
    /* Stop both anims BEFORE touching the descriptors so neither callback fires
     * against state we are about to flip. lv_anim_delete with the exec cb removes
     * the running fade and prevents crossfade_done_cb from running later. NOTE:
     * lv_anim_delete invokes only the anim's deleted_cb (we set none), NOT its
     * completed_cb, so crossfade_done_cb does NOT run here — the retirement below
     * is the single cleanup, with no double-cleanup. */
    lv_anim_delete(p->img_back,  anim_opa_cb);
    lv_anim_delete(p->img_front, anim_opa_cb);

    /* Promote the incoming frame to fully visible (it was mid fade-in). */
    lv_obj_set_style_opa(p->img_back, LV_OPA_COVER, 0);

    /* Retire the old front frame exactly as crossfade_done_cb does. */
    lv_image_dsc_t *old_dsc = p->front_is_a ? &p->dsc_a : &p->dsc_b;
    lv_obj_add_flag(p->img_front, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(p->img_front, NULL);
    release_dsc(p, old_dsc, p->front_is_a);   /* skips borrowed, frees owned */

    p->front_is_a = !p->front_is_a;
    p->img_front  = p->front_is_a ? lv_obj_get_child(p->root, 0)
                                  : lv_obj_get_child(p->root, 1);
    p->img_back   = p->front_is_a ? lv_obj_get_child(p->root, 1)
                                  : lv_obj_get_child(p->root, 0);
    p->crossfade_active = false;
}

/* Tap (short click) triggers the moon-cycle animation. Registered on the Moon
 * instance only, so no per-callback source test is needed. */
static void moon_tap_cb(lv_event_t *e)
{
    image_page_t *p = lv_event_get_user_data(e);
    if (!p) return;
    atomic_store(&moon_anim_request, true);
    image_page_wake(p);
}

/* Single-finger drag-to-rotate (Moon instance only). PRESSED/PRESSING/RELEASED
 * feed moon_interaction's accumulated yaw/pitch; the render (poll) task reads
 * that state and rebuilds the sphere. The render task is nudged on press/release
 * via image_page_wake() (the same notify moon_tap_cb uses). */
static bool moon_indev_point(lv_point_t *pt)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return false;
    lv_indev_get_point(indev, pt);
    return true;
}

static void moon_drag_pressed_cb(lv_event_t *e)
{
    image_page_t *p = lv_event_get_user_data(e);
    if (!p) return;
    lv_point_t pt;
    if (!moon_indev_point(&pt)) return;
    moon_drag_begin((float)pt.x, (float)pt.y);
    /* Wake the render loop to START the realtime drag frames. It then self-spins
     * (loops on moon_drag_settled()) for the whole gesture, so no per-move notify
     * is needed; RELEASED notifies again to ensure the eased snap-back finishes. */
    image_page_wake(p);
}

static void moon_drag_pressing_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t pt;
    if (!moon_indev_point(&pt)) return;
    moon_drag_move((float)pt.x, (float)pt.y);
    /* No wake here: the render loop self-spins for the whole drag (it loops on
     * moon_drag_settled() without waiting on notifications), so a per-move notify
     * is a dead no-op. PRESSED wakes the loop to start; RELEASED is what lets it
     * run the eased snap-back to completion. moon_drag_move() just updates the
     * shared target the spinning loop already reads each frame. */
}

static void moon_drag_released_cb(lv_event_t *e)
{
    image_page_t *p = lv_event_get_user_data(e);
    if (!p) return;
    moon_drag_end();
    image_page_wake(p);
}

/* Apply the "chip" style to a label: transparent background (no chip) with the
 * historic padding so label geometry/stacking math is unchanged.
 * Non-red themes use white text. Under the Red Night theme (gated by
 * theme_is_red_night) the text becomes the theme's red (text_color) so the page
 * emits only red shades or black. image_page_apply_theme() re-runs this on
 * theme change. */
static void apply_chip_style(lv_obj_t *lbl)
{
    const theme_t *th = current_theme;
    bool red = theme_is_red_night(th);
    lv_color_t txt = red ? lv_color_hex(th->text_color) : lv_color_white();
    lv_obj_set_style_bg_opa(lbl,     LV_OPA_TRANSP,    0);
    lv_obj_set_style_pad_left(lbl,   8,                0);
    lv_obj_set_style_pad_right(lbl,  8,                0);
    lv_obj_set_style_pad_top(lbl,    3,                0);
    lv_obj_set_style_pad_bottom(lbl, 3,                0);
    lv_obj_set_style_text_color(lbl, txt, 0);
}

/* ─────────────────── Cloud Cover location marker ───────────────────
 * The Clouds frames are fetched with a bbox centred exactly on
 * weather_lat/weather_lon (clouds_bbox() in clouds_wms.h), and every image on
 * this page is scaled to the panel WIDTH and vertically centred
 * (see center_image_y / swap_borrowed_buf), so the frame centre is ALWAYS the
 * screen centre and lv_obj_center() on the page container is the location.
 * Clouds never crops (image_page_config_crop) and has no flip/rotate, so no
 * transform can move it off centre.
 *
 * One instance only (the Clouds page is created once and never destroyed —
 * optional_page_set_enabled hides it instead), hence a single file static
 * rather than a field on every image_page_t. Created as the LAST child of the
 * page container so it draws above both crossfade images AND the overlay bar,
 * and it is never a target of the crossfade opacity anim, which runs on the two
 * lv_image objects only. Not CLICKABLE, so taps pass through to the page. */
static lv_obj_t *s_clouds_marker;   /* NULL until the Clouds page is created */

/* Colour the ring and the centre dot from the active theme. Red Night must emit
 * only red or black (same rule as apply_chip_style), so both parts take the
 * theme text colour there; other themes get an accent ring (progress_color)
 * with a white dot, which keeps the marker legible whatever the accent's
 * luminance. The 2 px black outline on both parts carries it over white cloud
 * tops as well as dark terrain. Display lock held by the caller. */
static void marker_apply_theme(void)
{
    if (!s_clouds_marker) return;
    const theme_t *th = current_theme;
    bool red = theme_is_red_night(th);
    lv_color_t ring = red ? lv_color_hex(th->text_color) : lv_color_hex(th->progress_color);
    lv_color_t dot  = red ? lv_color_hex(th->text_color) : lv_color_white();
    lv_obj_set_style_border_color(s_clouds_marker, ring, 0);
    lv_obj_t *centre = lv_obj_get_child(s_clouds_marker, 0);
    if (centre) lv_obj_set_style_bg_color(centre, dot, 0);
}

/* Build the marker (36 px ring + 8 px dot; 18/4 was a speck on the 720 px panel) centred on @p parent, hidden.
 * Display lock held by the caller (image_page_create runs under it). */
static void marker_create(lv_obj_t *parent)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, 36, 36);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 3, 0);
    lv_obj_set_style_border_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(m, 2, 0);
    lv_obj_set_style_outline_color(m, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(m, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                         LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(m);

    lv_obj_t *dot = lv_obj_create(m);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(dot, 2, 0);
    lv_obj_set_style_outline_color(dot, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                           LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_center(dot);

    s_clouds_marker = m;
    marker_apply_theme();
}

/* Show/hide the marker. No-op on every instance but Clouds (the pointer is NULL
 * until the Clouds page is built). Display lock held by the caller. */
static void marker_set_visible(bool visible)
{
    if (!s_clouds_marker) return;
    if (visible) lv_obj_clear_flag(s_clouds_marker, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_clouds_marker, LV_OBJ_FLAG_HIDDEN);
}

static void hide_moon_corner_labels(image_page_t *p);

/* Show the four moon corner labels and populate them from moon_overlay_info().
 * Called from every moon update path. Guards against NULL: only the Moon
 * instance creates these labels, so this is a no-op on the other three. */
static void update_moon_corner_labels(image_page_t *p)
{
    if (!p->lbl_moon_age) return;
    /* The corner labels track overlay visibility. overlay_bar's HIDDEN flag is the
     * single source of truth (set by the initial config check and by
     * image_page_set_overlay_visible). Without this guard the live drag paths
     * (show_scaled / show_borrowed) would re-show the labels on every frame even
     * while the overlay is turned off. */
    if (p->overlay_bar && lv_obj_has_flag(p->overlay_bar, LV_OBJ_FLAG_HIDDEN)) {
        hide_moon_corner_labels(p);
        return;
    }
    char age[20], next[20], rise[20], set[20];
    moon_overlay_info(age, sizeof(age), next, sizeof(next),
                      rise, sizeof(rise), set, sizeof(set));
    set_label_if_changed(p->lbl_moon_age,  age);
    set_label_if_changed(p->lbl_moon_next, next);
    set_label_if_changed(p->lbl_moon_rise, rise);
    set_label_if_changed(p->lbl_moon_set,  set);
    lv_obj_clear_flag(p->lbl_moon_age,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p->lbl_moon_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p->lbl_moon_rise, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p->lbl_moon_set,  LV_OBJ_FLAG_HIDDEN);
}

/* Hide the four moon corner labels (non-Moon instances and overlay-off state). */
static void hide_moon_corner_labels(image_page_t *p)
{
    if (p->lbl_moon_age)  lv_obj_add_flag(p->lbl_moon_age,  LV_OBJ_FLAG_HIDDEN);
    if (p->lbl_moon_next) lv_obj_add_flag(p->lbl_moon_next, LV_OBJ_FLAG_HIDDEN);
    if (p->lbl_moon_rise) lv_obj_add_flag(p->lbl_moon_rise, LV_OBJ_FLAG_HIDDEN);
    if (p->lbl_moon_set)  lv_obj_add_flag(p->lbl_moon_set,  LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *image_page_create(image_page_t *p, lv_obj_t *parent)
{
    lv_obj_t *page_container = lv_obj_create(parent);
    p->root = page_container;
    lv_obj_remove_style_all(page_container);
    lv_obj_set_size(page_container, screen_size(), screen_size());
    /* Negate the parent main_cont's OUTER_PADDING so the image fills edge-to-edge
     * (matches nina_spotify spotify_page_create). Without this the page renders
     * inset by OUTER_PADDING, leaving a black band on the top and left. */
    lv_obj_set_pos(page_container, -OUTER_PADDING, -OUTER_PADDING);
    lv_obj_set_style_bg_color(page_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(page_container, LV_OBJ_FLAG_CLICKABLE);

    /* Tap + single-finger drag-to-rotate belong to the Moon instance only, so
     * the callbacks are registered ONLY there (the other three pages never see
     * them and need no per-callback source test). SHORT_CLICKED (not CLICKED)
     * so the tap does not fire on the swipe gesture used for page navigation.
     * PRESSED snapshots the start point, PRESSING accumulates yaw/pitch,
     * RELEASED ends the drag. */
    if (p->src == IMG_SRC_MOON) {
        lv_obj_add_event_cb(page_container, moon_tap_cb, LV_EVENT_SHORT_CLICKED, p);
        lv_obj_add_event_cb(page_container, moon_drag_pressed_cb,  LV_EVENT_PRESSED,  p);
        lv_obj_add_event_cb(page_container, moon_drag_pressing_cb, LV_EVENT_PRESSING, p);
        lv_obj_add_event_cb(page_container, moon_drag_released_cb, LV_EVENT_RELEASED, p);
    }

    init_image_dsc(&p->dsc_a);
    init_image_dsc(&p->dsc_b);

    /* Child 0 = image A, child 1 = image B (the two stacked crossfade images);
     * overlay_bar (child 2) MUST be created after both images so the child-index
     * math in crossfade_done_cb / release_lvgl (children 0 and 1) stays valid.
     *
     * Each image uses pos + pivot (0,0) and no explicit object size, so the
     * source auto-sizes and lv_image_set_scale() (in render_frame) expands it
     * from the top-left to fill the panel — matching nina_spotify's album art.
     * Do NOT add LV_IMAGE_ALIGN_CENTER or a fixed 720x720 size: combined with
     * the scale transform they offset the image and leave a black border. */
    lv_obj_t *img_a = lv_image_create(page_container);
    lv_obj_set_pos(img_a, 0, 0);
    lv_image_set_pivot(img_a, 0, 0);
    lv_obj_add_flag(img_a, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *img_b = lv_image_create(page_container);
    lv_obj_set_pos(img_b, 0, 0);
    lv_image_set_pivot(img_b, 0, 0);
    lv_obj_add_flag(img_b, LV_OBJ_FLAG_HIDDEN);

    /* Drag-to-rotate must not also scroll/pan. page_container is non-scrollable,
     * but LVGL scroll-chains the drag up to the scrollable main_cont parent,
     * which then pans the image. Clear SCROLLABLE and the SCROLL_CHAIN_HOR/VER
     * flags on the container AND both image objects (the press target can resolve
     * to an image) so the gesture is consumed here and never bubbles to main_cont.
     * This only affects the image page objects, not main_cont or other pages, and
     * is independent of the LV_EVENT_GESTURE page-swipe on scr_dashboard. */
    lv_obj_clear_flag(page_container, LV_OBJ_FLAG_SCROLLABLE |
                                      LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                                      LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_clear_flag(img_a, LV_OBJ_FLAG_SCROLLABLE |
                             LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                             LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_clear_flag(img_b, LV_OBJ_FLAG_SCROLLABLE |
                             LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                             LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    p->img_front = img_a;
    p->img_back  = img_b;
    p->front_is_a = true;
    p->displayed_stamp_ms = 0;
    p->crossfade_active   = false;

    p->overlay_bar = lv_obj_create(page_container);
    lv_obj_remove_style_all(p->overlay_bar);
    lv_obj_set_size(p->overlay_bar, screen_size(), 68);
    lv_obj_align(p->overlay_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* No bar background: labels render text directly (apply_chip_style), so
     * overlay_bar is just an invisible positioning container for
     * lbl_region / lbl_timestamp. */
    lv_obj_set_style_bg_opa(p->overlay_bar, LV_OPA_TRANSP, 0);
    /* No edge padding — the bottom labels sit flush in the screen corners so they
     * stay clear of the inscribed moon disc. */
    lv_obj_set_style_pad_left(p->overlay_bar, 0, 0);
    lv_obj_set_style_pad_right(p->overlay_bar, 0, 0);
    lv_obj_clear_flag(p->overlay_bar, LV_OBJ_FLAG_SCROLLABLE);

    p->lbl_region = lv_label_create(p->overlay_bar);
    lv_obj_set_style_text_font(p->lbl_region, &lv_font_overpass_27, 0);
    lv_obj_align(p->lbl_region, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    apply_chip_style(p->lbl_region);   /* white text, transparent bg */
    lv_label_set_text(p->lbl_region, "");

    p->lbl_timestamp = lv_label_create(p->overlay_bar);
    lv_obj_set_style_text_font(p->lbl_timestamp, &lv_font_overpass_27, 0);
    lv_obj_align(p->lbl_timestamp, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    apply_chip_style(p->lbl_timestamp);
    lv_label_set_text(p->lbl_timestamp, "");

    /* Moon corner labels: children of page_container (NOT overlay_bar) so they
     * sit at the TOP corners above the image. Created after overlay_bar so their
     * z-order keeps them on top. Only the Moon instance has them; the other three
     * leave them NULL and every access is guarded. Start hidden. */
    if (p->src == IMG_SRC_MOON) {
        p->lbl_moon_age = lv_label_create(page_container);
        lv_obj_set_style_text_font(p->lbl_moon_age, &lv_font_overpass_27, 0);
        apply_chip_style(p->lbl_moon_age);
        lv_obj_align(p->lbl_moon_age, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_label_set_text(p->lbl_moon_age, "");
        lv_obj_add_flag(p->lbl_moon_age, LV_OBJ_FLAG_HIDDEN);

        p->lbl_moon_next = lv_label_create(page_container);
        lv_obj_set_style_text_font(p->lbl_moon_next, &lv_font_overpass_27, 0);
        apply_chip_style(p->lbl_moon_next);
        /* Stack flush below lbl_moon_age. At 27px each label is line_height(39)+pad(6)
         * = 45px tall, so the second row sits at y = 45 (rows touch, no gap). */
        lv_obj_align(p->lbl_moon_next, LV_ALIGN_TOP_LEFT, 0, 45);
        lv_label_set_text(p->lbl_moon_next, "");
        lv_obj_add_flag(p->lbl_moon_next, LV_OBJ_FLAG_HIDDEN);

        p->lbl_moon_rise = lv_label_create(page_container);
        lv_obj_set_style_text_font(p->lbl_moon_rise, &lv_font_overpass_27, 0);
        apply_chip_style(p->lbl_moon_rise);
        lv_obj_align(p->lbl_moon_rise, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_label_set_text(p->lbl_moon_rise, "");
        lv_obj_add_flag(p->lbl_moon_rise, LV_OBJ_FLAG_HIDDEN);

        p->lbl_moon_set = lv_label_create(page_container);
        lv_obj_set_style_text_font(p->lbl_moon_set, &lv_font_overpass_27, 0);
        apply_chip_style(p->lbl_moon_set);
        lv_obj_align(p->lbl_moon_set, LV_ALIGN_TOP_RIGHT, 0, 45);
        lv_label_set_text(p->lbl_moon_set, "");
        lv_obj_add_flag(p->lbl_moon_set, LV_OBJ_FLAG_HIDDEN);
    }

    /* Animated pages: a pulsing placeholder shown from page entry until the
     * loop holds ring_show_gate() frames. Without it the first seconds after
     * boot or entry are a black page (the ring is empty until the first
     * download lands, and one or two frames loop badly). Hidden until
     * ring_loading_sync() decides otherwise. */
    if (image_src_is_animated(p->src)) {
        p->loading = nina_empty_state_create(page_container, ICON_CLOUD_OFF,
                                             p->src == IMG_SRC_CLOUDS ? "Loading Cloud Cover"
                                                                      : "Loading Weather Radar",
                                             NULL, 0);
        if (p->loading) nina_empty_state_hide(p->loading);
    }

    /* Cloud Cover location marker: created LAST so it stacks above both
     * crossfade images and the overlay bar. Clouds instance only; the other
     * five sources never build it and so can never show it. Independent of the
     * caption overlay toggle — it has its own config flag. */
    if (p->src == IMG_SRC_CLOUDS) {
        marker_create(page_container);
        marker_set_visible(app_config_get()->clouds_show_location);
    }

    if (!image_page_config_overlay(app_config_get(), p->src)) {
        lv_obj_add_flag(p->overlay_bar, LV_OBJ_FLAG_HIDDEN);
        /* Corner labels also start hidden when the overlay is off. */
        hide_moon_corner_labels(p);
    }
    return page_container;
}

void image_page_render_frame(image_page_t *p)
{
    if (!p || !p->root) return;
    if (p->crossfade_active) return;
    if (!frame_lock(p, 100)) return;

    /* Consume the force-redraw request: a crop/full toggle re-renders the cached
     * frame, so bypass the "new image" half of the gate this once. Still bail if
     * nothing is cached (!frame.buf). The actual swap below is forced instant (no
     * crossfade) since a crossfade between two crops of the same frame is
     * pointless. */
    bool forced = p->force_redraw;
    p->force_redraw = false;

    /* Custom Image URL / Radar error overlay. A failed fetch stores a reason string in
     * frame.error_msg and leaves no fresh frame (stamp_ms not advanced), so the
     * new-image gate below would return early and the page would show a blank or
     * stale panel with no explanation. Read error_msg under frame_mux (like every
     * other frame read here), and when a reason is present render it as the
     * caption text and bail. A successful fetch clears error_msg, so the normal
     * swap path below runs and overwrites the caption — no stale error. Scoped to
     * the sources whose failures are user-visible (Custom Image, Radar whose site
     * token can be mistyped into a 404, Clouds behind a GIBS outage), so
     * GOES/Solar/Moon are visually unchanged. */
    if (image_page_shows_error(p->src) && p->frame.error_msg[0] != '\0') {
        char err_copy[48];
        strlcpy(err_copy, p->frame.error_msg, sizeof(err_copy));
        frame_unlock(p);
        lv_label_set_text(p->lbl_region, err_copy);
        lv_label_set_text(p->lbl_timestamp, "");
        hide_moon_corner_labels(p);
        /* Release any manual-fetch wait overlay so it cannot get stuck on the
         * error path (mirrors the other early-exit overlay-hide sites). */
        nina_wait_overlay_hide();
        return;
    }

    /* An animated page keeps its decoded frames in the ring, never in p->frame,
     * so the whole copy/crop/crossfade path below does not apply: re-show the
     * frame the playback cursor is on (the ring stores them already baked)
     * and make sure the playback timer matches the current ring state. */
    if (image_src_is_animated(p->src)) {
        frame_unlock(p);
        ring_show_idx(p, -1, false);           /* -1 = whatever the cursor is on */
        ring_timer_sync(p);
        nina_wait_overlay_hide();
        return;
    }

    if (!p->frame.buf || (!forced && p->frame.stamp_ms <= p->displayed_stamp_ms)) {
        frame_unlock(p);
        return;
    }

    /* Transform the RGB565 source into a freshly allocated PSRAM buffer (ONE PPA
     * pass, below) so frame_mux is released before the (asynchronous) crossfade
     * and the LVGL side owns its display buffers independently of the poller. */
    uint16_t sw = p->frame.w, sh = p->frame.h;
    if (sw == 0 || sh == 0) {
        frame_unlock(p);
        /* New-image gate passed but the image is unusable — release any
         * manual-fetch wait overlay so it can't get stuck. */
        nina_wait_overlay_hide();
        return;
    }

    /* Optional center-crop: keep the central portion so the solar/satellite disc
     * zooms to fill the panel and the timestamp/label baked into the source
     * border is cropped off. Skipped for the Moon: it has no text and is already
     * framed to fill. The crop percentage is per-band for Solar: 88% for AIA/EIT,
     * 92% for HMI (trims to the disc edge), 100% (no crop) for LASCO. Other
     * sources keep the historic 88% (e.g. GOES). */
    const app_config_t *cfg = app_config_get();
    uint16_t w = sw, h = sh, ox = 0, oy = 0;
    uint8_t crop_pct = (p->src == IMG_SRC_SOLAR)
                           ? solar_band_crop_pct(cfg->solar_band)
                           : 88;
    if (image_page_config_crop(cfg, p->src) && p->src != IMG_SRC_MOON &&
        crop_pct < 100) {
        w  = (uint16_t)((uint32_t)sw * crop_pct / 100);
        h  = (uint16_t)((uint32_t)sh * crop_pct / 100);
        ox = (uint16_t)((sw - w) / 2);
        oy = (uint16_t)((sh - h) / 2);
    }

    /* Everything the LVGL side needs out of the frame, captured under the same
     * lock as the pixels: the label so the on-screen text cannot desync from the
     * picture if a config change lands between fetches (issue #166). */
    int64_t stamp_ms = p->frame.stamp_ms;
    char label_copy[48];
    strlcpy(label_copy, p->frame.label, sizeof(label_copy));

    /* Optional caption mask (Solar bands only). The mask rect is in upright-full
     * image fractional coords; convert to a pixel rect [mx0..mx1, my0..my1]. For
     * masked pixels we composite a colour-sampled blend: per row, sample one
     * source pixel just to the RIGHT of the mask (col mx1+PAD) and stretch it
     * across the masked span. This extends the neighbouring background (black
     * margin for HMI -> seamless; corona for LASCO -> blends) horizontally over
     * the burned-in timestamp.
     *
     * The ONE case that still costs a full-frame CPU copy: the PPA cannot paint
     * it, and mapping the mask rect into output space would be a second geometry
     * to keep in step with image_fit. Every other source hands the PPA
     * p->frame.buf directly. Gated on the same crop/clean toggle as the crop, so
     * turning that off shows the raw frame, burned-in timestamp and all. */
    const uint8_t *src_pix = p->frame.buf;
    uint8_t *masked = NULL;
    float fx0, fy0, fx1, fy1;
    if (image_page_config_crop(cfg, p->src) && p->src == IMG_SRC_SOLAR &&
        solar_band_text_mask(cfg->solar_band, &fx0, &fy0, &fx1, &fy1)) {
        masked = heap_caps_malloc((size_t)sw * sh * 2, MALLOC_CAP_SPIRAM);
        if (masked) {
            memcpy(masked, p->frame.buf, (size_t)sw * sh * 2);
            int mx0 = (int)(fx0 * sw), mx1 = (int)(fx1 * sw);
            int my0 = (int)(fy0 * sh), my1 = (int)(fy1 * sh);
            int sample_col = mx1 + 8;                  /* PAD past the mask edge */
            if (sample_col > (int)sw - 1) sample_col = (int)sw - 1;
            if (sample_col < 0) sample_col = 0;
            if (mx0 < 0) mx0 = 0;
            if (mx1 > (int)sw) mx1 = (int)sw;
            if (my0 < 0) my0 = 0;
            if (my1 > (int)sh) my1 = (int)sh;
            for (int my = my0; my < my1; my++) {
                uint16_t *row  = (uint16_t *)(masked + (size_t)my * sw * 2);
                uint16_t  fill = row[sample_col];
                for (int mx = mx0; mx < mx1; mx++) row[mx] = fill;
            }
            src_pix = masked;
        } else {
            ESP_LOGW(TAG, "%s: no PSRAM for the caption mask, showing it raw", p->name);
        }
    }

    /* Per-source mirror and rotation flags. The PPA applies the mirrors to the
     * source and then the rotation, the same order the old software passes ran
     * in, so the on-screen result is unchanged. orient 0/1/2/3 = 0/90/180/270
     * degrees CLOCKWISE. The Moon has neither (its own moon_flip_u/v handling
     * lives elsewhere) and no crop, so its job below is an identity 1.0x copy. */
    uint8_t do_vflip = (p->src == IMG_SRC_GOES)   ? cfg->goes_vflip
                       : (p->src == IMG_SRC_SOLAR) ? cfg->solar_vflip
                       : (p->src == IMG_SRC_CUSTOM) ? cfg->custom_vflip
                                                    : 0;
    uint8_t do_hflip = (p->src == IMG_SRC_GOES)   ? cfg->goes_hflip
                       : (p->src == IMG_SRC_SOLAR) ? cfg->solar_hflip
                       : (p->src == IMG_SRC_CUSTOM) ? cfg->custom_hflip
                                                    : 0;
    uint8_t orient   = (p->src == IMG_SRC_GOES)   ? cfg->goes_orientation
                       : (p->src == IMG_SRC_SOLAR) ? cfg->solar_orientation
                       : (p->src == IMG_SRC_CUSTOM) ? cfg->custom_orientation
                                                    : 0;

    /* ONE hardware pass does crop + mirror + rotate + scale straight into a
     * panel-width destination. What this replaces: a crop memcpy, a row-swap
     * vflip, an in-row hflip and a gather-rotate into a second buffer, all on
     * the display lock and on up to 1024x1024 pixels, plus — every bit as
     * expensive — an lv_image_set_scale() != 256 that made LVGL software-
     * resample the whole 720x720 screen on EVERY redraw in full-refresh mode.
     * The destination is exactly screen_size() wide, so the image is a blit now.
     *
     * image_fit_pick() works in LOGICAL (post-rotation) space and hands back the
     * n/16 scale plus the symmetric trim that keeps the output inside the panel;
     * image_fit_logical_to_source() turns that trim back into the source
     * rectangle, which is then offset by the config crop origin (ox, oy). */
    uint32_t lw = (orient & 1) ? h : w;
    uint32_t lh = (orient & 1) ? w : h;
    image_fit_t fit;
    uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    uint8_t  *dst = NULL;
    size_t    dst_size = 0;
    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (image_fit_pick(lw, lh, screen_size(), &fit) &&
        image_fit_logical_to_source(&fit, w, h, orient, &bx, &by, &bw, &bh)) {
        /* 128 B aligned address AND size: the PPA rejects anything else. */
        dst_size = ((size_t)screen_size() * fit.out_h * 2 + 127) & ~(size_t)127;
        dst = heap_caps_aligned_alloc(128, dst_size, MALLOC_CAP_SPIRAM);
        if (dst) {
            ppa_srm_job_t job = {
                .src           = src_pix,
                .src_stride_px = sw,
                .src_h         = sh,
                .block_x       = (uint32_t)ox + bx,
                .block_y       = (uint32_t)oy + by,
                .block_w       = bw,
                .block_h       = bh,
                .rotate_cw     = orient,
                .hflip         = do_hflip != 0,
                .vflip         = do_vflip != 0,
                .dst           = dst,
                .dst_buf_size  = dst_size,
                .dst_w         = screen_size(),
                .dst_h         = fit.out_h,
                .dst_x         = fit.dst_x,
                .dst_y         = 0,
                .scale_n16     = fit.n16,
                /* Only a source too wide to fill the panel leaves bands to clear. */
                .clear_dst     = (fit.out_w < screen_size()),
            };
            err = ppa_srm_rgb565(&job);
        } else {
            ESP_LOGE(TAG, "PSRAM alloc failed for the display frame (%u bytes)",
                     (unsigned)dst_size);
            err = ESP_ERR_NO_MEM;
        }
    }
    /* Degraded fallback for a PPA that would not run at all (pool starvation, an
     * alignment corner the driver rejects): copy the config-crop window straight
     * out of the source, rows as they are — no flip, no rotate — so the page can
     * still put a picture up instead of staying blank forever after boot. It must
     * happen HERE, while frame_mux is still held and src_pix (p->frame.buf, or
     * `masked`) is still valid. LVGL scales it in software from the tail below,
     * which is exactly what this page did before the PPA pass existed. Skipped
     * when the PPA leg already failed on a PSRAM alloc: a second one would too. */
    uint8_t *fb = NULL;
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        fb = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        if (fb) {
            for (uint16_t r = 0; r < h; r++) {
                memcpy(fb + (size_t)r * w * 2,
                       src_pix + ((size_t)(oy + r) * sw + ox) * 2,
                       (size_t)w * 2);
            }
        }
    }

    /* The PPA read the source (blocking call), so the frame is free again. */
    frame_unlock(p);
    if (masked) heap_caps_free(masked);

    /* What the descriptor/crossfade tail below shows, set by whichever path won:
     * the PPA output (panel width, 1.0x blit) or the fallback crop (software
     * scale to panel width). */
    uint8_t  *show_buf   = NULL;
    uint16_t  show_w     = 0;
    uint16_t  show_h     = 0;
    uint16_t  show_scale = 256;

    if (err != ESP_OK) {
        if (dst) heap_caps_free(dst);
        /* Warn ONCE so a systematic failure is visible in /api/logs without a
         * line per frame. Screen rotation (main.c) drops the frame outright on a
         * PPA failure; here we degrade instead, because an image page with no
         * previous frame would otherwise stay blank until a later poll. */
        static bool ppa_warned;
        if (!ppa_warned) {
            ppa_warned = true;
            ESP_LOGE(TAG, "%s: PPA fit failed (%s); image pages fall back to "
                          "software scaling (no flip/rotate)",
                     p->name, esp_err_to_name(err));
        }
        if (!fb) {
            nina_wait_overlay_hide();
            return;
        }
        show_buf   = fb;
        show_w     = w;
        show_h     = h;
        show_scale = (uint16_t)(((uint32_t)screen_size() * 256 + w / 2) / w);
    } else {
        show_buf = dst;
        show_w   = screen_size();
        show_h   = (uint16_t)fit.out_h;
    }

    /* Red Night: recolour the decoded image to red shades (luma->red, in place).
     * Self-gates inside the helper (no-op for every non-red theme). Runs on
     * whichever buffer won, with that buffer's own pixel count. */
    image_red_remap_rgb565((uint16_t *)show_buf, (size_t)show_w * show_h);

    /* Point the BACK slot's descriptor at the new buffer. release_dsc() frees the
     * previous buffer unless it was borrowed (moon-drag), and clears that flag —
     * this owned buffer is not borrowed. */
    bool back_is_a = !p->front_is_a;
    lv_image_dsc_t *back_dsc = p->front_is_a ? &p->dsc_b : &p->dsc_a;
    release_dsc(p, back_dsc, back_is_a);
    back_dsc->data          = show_buf;
    back_dsc->data_size     = (size_t)show_w * show_h * 2;
    back_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    back_dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    back_dsc->header.w      = show_w;
    back_dsc->header.h      = show_h;
    back_dsc->header.stride = (uint32_t)show_w * 2;

    /* On the PPA path the buffer is already panel width and LVGL only blits it
     * (show_scale 256). The image keeps its create-time x = 0; only y moves, and
     * only when the height changes (center_image_y), so the per-update
     * lv_obj_set_pos() that once flipped the image under 90/270 display rotation
     * is still not happening here. */
    lv_image_set_src(p->img_back, back_dsc);
    lv_image_set_scale(p->img_back, show_scale);
    center_image_y(p->img_back, (int)show_h, show_scale);
    lv_obj_set_style_opa(p->img_back, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(p->img_back, LV_OBJ_FLAG_HIDDEN);

    bool first_image = (p->displayed_stamp_ms == 0);
    /* The Moon source re-posts a near-identical frame every ~60s; a crossfade
     * dips brightness at its midpoint (two ~50% copies over black) and reads as
     * a once-a-minute flicker. Swap instantly for the moon (and the first image).
     * A forced re-crop also swaps instantly: crossfading two crops of the same
     * frame is pointless and, since displayed_stamp_ms == stamp_ms here, would
     * not key as first_image.
     *
     * The settle->resting handoff (drag end) also swaps instantly: a crossfade
     * there hit the same midpoint brightness dip, reading as a visible jump as the
     * crisp resting frame faded in. Instant matches every other moon frame swap. */
    bool instant = first_image || forced || (p->src == IMG_SRC_MOON);
    /* stamp_ms == displayed_stamp_ms on a forced re-crop (same cached frame), so
     * this assignment leaves displayed_stamp_ms unchanged and a later real
     * download (higher stamp_ms) still passes the new-image gate. */
    p->displayed_stamp_ms = stamp_ms;

    if (instant) {
        /* Show the new image at full opacity and retire the old one immediately.
         * Mirrors crossfade_done_cb so the previous buffer is freed, not leaked.
         * On the first image old_dsc->data is NULL, so the free is a no-op. */
        lv_obj_set_style_opa(p->img_back, LV_OPA_COVER, 0);
        lv_image_dsc_t *old_dsc = p->front_is_a ? &p->dsc_a : &p->dsc_b;
        lv_obj_add_flag(p->img_front, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(p->img_front, NULL);
        release_dsc(p, old_dsc, p->front_is_a);   /* skips free if borrowed */
        p->front_is_a = !p->front_is_a;
        p->img_front  = p->front_is_a ? lv_obj_get_child(p->root, 0)
                                      : lv_obj_get_child(p->root, 1);
        p->img_back   = p->front_is_a ? lv_obj_get_child(p->root, 1)
                                      : lv_obj_get_child(p->root, 0);
    } else {
        p->crossfade_active = true;

        lv_anim_t a_in;
        lv_anim_init(&a_in);
        lv_anim_set_var(&a_in, p->img_back);
        lv_anim_set_values(&a_in, 0, 255);
        lv_anim_set_duration(&a_in, 500);
        lv_anim_set_path_cb(&a_in, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a_in, anim_opa_cb);
        lv_anim_set_user_data(&a_in, p);
        lv_anim_set_completed_cb(&a_in, crossfade_done_cb);
        lv_anim_start(&a_in);

        lv_anim_t a_out;
        lv_anim_init(&a_out);
        lv_anim_set_var(&a_out, p->img_front);
        lv_anim_set_values(&a_out, 255, 0);
        lv_anim_set_duration(&a_out, 500);
        lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a_out, anim_opa_cb);
        lv_anim_start(&a_out);
    }

    if (p->src == IMG_SRC_MOON) {
        char name[24], pct[16];
        moon_caption(name, sizeof(name), pct, sizeof(pct));
        lv_label_set_text(p->lbl_region, name);
        lv_label_set_text(p->lbl_timestamp, pct);
        update_moon_corner_labels(p);
    } else {                                         /* GOES / Solar / Custom */
        lv_label_set_text(p->lbl_region, label_copy);
        time_t now; struct tm ti; time(&now); localtime_r(&now, &ti);
        char ts[32]; strftime(ts, sizeof(ts), "Updated %H:%M", &ti);
        lv_label_set_text(p->lbl_timestamp, ts);
        hide_moon_corner_labels(p);
    }

    /* The new image is now committed (swap done, overlay labels updated). Hide
     * the wait overlay if it was shown for a manual refresh. This is a no-op when
     * the overlay is already hidden. We hold the display lock (the caller does),
     * so call directly — matching this module's convention. */
    nina_wait_overlay_hide();
}

void image_page_show_scaled(image_page_t *p, const uint16_t *buf, int w, int h)
{
    if (!atomic_load(&p->active)) return;
    if (!buf || !p->root || w <= 0 || h <= 0) return;
    /* A settle->resting crossfade may still be in flight when a SW-fallback drag
     * frame arrives. Cancel and finalize it (matching show_borrowed) so this fresh
     * frame takes over immediately instead of being dropped (which froze the disc
     * until the fade finished, then jumped). cancel_moon_crossfade() retires buffers
     * exactly as crossfade_done_cb would, so ownership stays consistent for the swap
     * below. */
    if (p->crossfade_active) cancel_moon_crossfade(p);

    /* COPY the small render into our OWNED, reused buffer, then software-scale it
     * to fill the panel. The render task overwrites its source buffer next frame,
     * so pointing the LVGL descriptor straight at it would let the flush task read
     * a half-written frame (tearing). Copying here de-couples the two: the per-frame
     * memcpy (~115-180KB) is negligible against the ~50-70ms render. We copy into the
     * buffer that pairs with the BACK slot (the one NOT on screen), so the overwrite
     * never touches the buffer the front image is still flushing. */
    bool back_is_a = !p->front_is_a;
    int  ci = back_is_a ? 0 : 1;
    size_t need = (size_t)w * h * 2;
    if (p->moon_copy_cap[ci] < need) {
        if (p->moon_copy_buf[ci]) heap_caps_free(p->moon_copy_buf[ci]);
        p->moon_copy_buf[ci] = heap_caps_aligned_alloc(128, need, MALLOC_CAP_SPIRAM);
        p->moon_copy_cap[ci] = p->moon_copy_buf[ci] ? need : 0;
        if (!p->moon_copy_buf[ci]) {
            ESP_LOGE(TAG, "moon scaled copy alloc failed (%u bytes)", (unsigned)need);
            return;
        }
    }
    memcpy(p->moon_copy_buf[ci], buf, need);

    /* Red Night: recolour the owned copy to red shades in place (self-gates to a
     * no-op for non-red themes). The remap is NOT idempotent — a second pass takes
     * the luma of (R,0,0) and darkens the image ~0.30x — so callers must hand this
     * function a NOT-yet-remapped source. The drag path (which remaps its 240x240
     * source before the PPA upscale) routes its red-night PPA-failure fallback
     * around this function for exactly that reason. */
    image_red_remap_rgb565(p->moon_copy_buf[ci], (size_t)w * h);

    /* Point the BACK slot at the owned copy. release_dsc() frees any prior OWNED
     * buffer; these copy buffers are page-owned and persistent, so we mark the
     * slot "borrowed" (release_dsc then skips the free) and free them ourselves in
     * image_page_release_lvgl(). */
    lv_image_dsc_t *back_dsc = p->front_is_a ? &p->dsc_b : &p->dsc_a;
    release_dsc(p, back_dsc, back_is_a);       /* free prior OWNED buffer if any */
    back_dsc->data          = (const uint8_t *)p->moon_copy_buf[ci];
    back_dsc->data_size     = need;
    back_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    back_dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    back_dsc->header.w      = w;
    back_dsc->header.h      = h;
    back_dsc->header.stride = w * 2;
    if (back_is_a) p->dsc_a_borrowed = true; else p->dsc_b_borrowed = true;

    /* Software-scale the small copy to fill the panel width (256 = 1.0x), the same
     * proven-clean path image_page_render_frame() uses. */
    uint16_t scale = (uint16_t)(((uint32_t)screen_size() * 256 + w / 2) / w);
    lv_image_set_src(p->img_back, back_dsc);
    lv_image_set_scale(p->img_back, scale);
    center_image_y(p->img_back, h, scale);   /* square moon render: resolves to 0 */
    lv_obj_set_style_opa(p->img_back, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p->img_back, LV_OBJ_FLAG_HIDDEN);

    /* Instant swap (no crossfade): retire the old front and flip designations,
     * mirroring the instant branch of image_page_render_frame(). release_dsc()
     * frees the old front only if this page owned it. */
    lv_image_dsc_t *old_dsc = p->front_is_a ? &p->dsc_a : &p->dsc_b;
    lv_obj_add_flag(p->img_front, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(p->img_front, NULL);
    release_dsc(p, old_dsc, p->front_is_a);

    p->front_is_a = !p->front_is_a;
    p->img_front  = p->front_is_a ? lv_obj_get_child(p->root, 0)
                                  : lv_obj_get_child(p->root, 1);
    p->img_back   = p->front_is_a ? lv_obj_get_child(p->root, 1)
                                  : lv_obj_get_child(p->root, 0);

    /* Mark a frame as displayed (mirrors render_frame), and update the moon
     * caption so the phase/illumination text stays live during the drag. */
    p->displayed_stamp_ms = esp_timer_get_time() / 1000;
    char name[24], pct[16];
    moon_caption(name, sizeof(name), pct, sizeof(pct));
    set_label_if_changed(p->lbl_region, name);
    set_label_if_changed(p->lbl_timestamp, pct);
    update_moon_corner_labels(p);

    nina_wait_overlay_hide();
}

/* Instant swap of the LVGL descriptor onto a buffer the PAGE DOES NOT OWN, with
 * no copy: the slot is flagged "borrowed" so release_dsc() never frees it.
 * Three callers share this: the moon drag loop (owner = the PPA ping-pong
 * scratch), the ring playback (owner = the page's dissolve scratch, or a ring
 * slot on the no-scratch fallback) and image_page_show_borrowed(). Each adds
 * its own caption after the swap. Caller holds the display lock and has already
 * checked p->active, p->root and the dimensions.
 *
 * ALWAYS INSTANT. The ring used to pass a fade flag here and cross-dissolve
 * with an lv_anim on the top image's opa; in FULL_REFRESH mode that is a
 * 720x720 SOFTWARE alpha blend on every animation tick (44 % SoC load on the
 * Cloud Cover page, both LVGL swdraw tasks near 40 %). The dissolve now runs on
 * the PPA Blend engine — see the ring playback scratch below — and the shown
 * image stays at LV_OPA_COVER. */
static void swap_borrowed_buf(image_page_t *p, const uint16_t *buf, int w, int h)
{
    /* A settle->resting crossfade may still be in flight when a new drag frame
     * arrives (rapid re-swipe right at the grace boundary). Rather than DROP this
     * frame (which froze the disc until the fade finished, then jumped — the
     * artifact), CANCEL the crossfade and finalize it so this fresh frame takes
     * over immediately. cancel_moon_crossfade() retires buffers exactly as
     * crossfade_done_cb would, so ownership stays consistent for the swap below. */
    if (p->crossfade_active) cancel_moon_crossfade(p);

    /* Point the BACK slot's descriptor straight at the caller's buffer — NO copy.
     * The moon drag loop ping-pongs two 720 PPA-output buffers and hands us the
     * one NOT currently on screen; the radar ring hands us a slot it will not
     * free while it is displayed. Either way nothing the front image is still
     * flushing is overwritten. Flag the slot "borrowed" so release_dsc() skips
     * freeing it. Free any PRIOR owned buffer this slot held first. */
    bool back_is_a = !p->front_is_a;
    lv_image_dsc_t *back_dsc = p->front_is_a ? &p->dsc_b : &p->dsc_a;
    release_dsc(p, back_dsc, back_is_a);       /* free prior OWNED buffer if any */
    size_t need = (size_t)w * h * 2;
    /* NO red-night remap here. The drag path remaps the small (240x240) render
     * source before the PPA upscale, which is ~9x cheaper than remapping the
     * 720x720 result on every frame. The remap is NOT idempotent (a second pass
     * takes the luma of (R,0,0) and darkens the image ~0.30x), so remapping the
     * already-remapped buffer here would visibly dim the disc. */
    back_dsc->data          = (const uint8_t *)buf;
    back_dsc->data_size     = need;
    back_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    back_dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    back_dsc->header.w      = w;
    back_dsc->header.h      = h;
    back_dsc->header.stride = w * 2;
    if (back_is_a) p->dsc_a_borrowed = true; else p->dsc_b_borrowed = true;

    /* 256 = 1.0x, which is what the two normal callers hand us: the moon drag
     * ping-pong is a full 720 PPA output, and the ring playback hands us its
     * 720-wide dissolve scratch. The general expression stays for the ring's
     * no-scratch fallback, which borrows a slot at its NATIVE size (a 600 px
     * radar tile) and needs LVGL to scale it as it always did. */
    uint16_t scale = (w > 0) ? (uint16_t)(((uint32_t)screen_size() * 256 + w / 2) / w) : 256;
    lv_image_set_src(p->img_back, back_dsc);
    lv_image_set_scale(p->img_back, scale);
    /* Centres the wide radar tile; a full-panel 720 moon-drag buffer gets 0. */
    center_image_y(p->img_back, h, scale);
    lv_obj_set_style_opa(p->img_back, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p->img_back, LV_OBJ_FLAG_HIDDEN);

    lv_image_dsc_t *old_dsc = p->front_is_a ? &p->dsc_a : &p->dsc_b;

    /* Retire the old front and flip designations, mirroring the
     * instant branch of image_page_render_frame(). release_dsc() frees the old
     * front only if this page owned it (skips a prior borrowed buffer). */
    lv_obj_add_flag(p->img_front, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(p->img_front, NULL);
    release_dsc(p, old_dsc, p->front_is_a);

    p->front_is_a = !p->front_is_a;
    p->img_front  = p->front_is_a ? lv_obj_get_child(p->root, 0)
                                  : lv_obj_get_child(p->root, 1);
    p->img_back   = p->front_is_a ? lv_obj_get_child(p->root, 1)
                                  : lv_obj_get_child(p->root, 0);

    /* Mark a frame as displayed (mirrors render_frame). */
    p->displayed_stamp_ms = esp_timer_get_time() / 1000;
}

void image_page_show_borrowed(image_page_t *p, const uint16_t *buf, int w, int h)
{
    if (!atomic_load(&p->active)) return;
    if (!buf || !p->root || w <= 0 || h <= 0) return;

    swap_borrowed_buf(p, buf, w, h);

    /* Keep the moon caption live during the drag. */
    char name[24], pct[16];
    moon_caption(name, sizeof(name), pct, sizeof(pct));
    set_label_if_changed(p->lbl_region, name);
    set_label_if_changed(p->lbl_timestamp, pct);
    update_moon_corner_labels(p);

    nina_wait_overlay_hide();
}

void image_page_force_redraw(image_page_t *p)
{
    /* Request that the next image_page_render_frame() re-render the cached frame
     * even though frame.stamp_ms has not advanced. The flag is a plain bool
     * touched only here (httpd task, display lock held by caller) and in
     * render_frame(); the caller wraps both this and the following render in
     * bsp_display_lock, so no extra synchronization is needed. */
    p->force_redraw = true;
}

bool image_page_has_image(image_page_t *p)
{
    /* True once a frame has been committed to the page; reset to 0 by
     * image_page_release_lvgl() on page leave and at create. Read under the
     * display lock held by the caller, matching this module's convention. */
    return p->displayed_stamp_ms != 0;
}

void image_page_release_lvgl(image_page_t *p)
{
    if (!p->root) return;

    p->crossfade_active = false;
    p->force_redraw     = false;
    lv_anim_delete(p->img_front, anim_opa_cb);
    lv_anim_delete(p->img_back, anim_opa_cb);

    lv_obj_t *child0 = lv_obj_get_child(p->root, 0);
    lv_obj_t *child1 = lv_obj_get_child(p->root, 1);
    lv_image_set_src(child0, NULL);
    lv_image_set_src(child1, NULL);
    lv_obj_add_flag(child0, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(child1, LV_OBJ_FLAG_HIDDEN);

    /* release_dsc() skips freeing a borrowed (moon-drag) buffer — that scratch is
     * owned by the moon drag loop and freed there via moon_drag_buffers_free() on
     * page leave, so this avoids a double-free of the borrowed 720x720 buffer. */
    release_dsc(p, &p->dsc_a, true);
    release_dsc(p, &p->dsc_b, false);

    /* Free the owned moon-drag copy buffers (release_dsc skipped them via the
     * borrowed flag). After this both descriptors are cleared, so no dangling
     * reference remains. */
    for (int i = 0; i < 2; i++) {
        if (p->moon_copy_buf[i]) {
            heap_caps_free(p->moon_copy_buf[i]);
            p->moon_copy_buf[i] = NULL;
        }
        p->moon_copy_cap[i] = 0;
    }

    hide_moon_corner_labels(p);

    p->displayed_stamp_ms = 0;
    p->front_is_a = true;
    p->img_front  = child0;
    p->img_back   = child1;
}

void image_page_set_overlay_visible(image_page_t *p, bool visible)
{
    if (!p->overlay_bar) return;
    if (visible) lv_obj_clear_flag(p->overlay_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(p->overlay_bar, LV_OBJ_FLAG_HIDDEN);

    /* Corner labels follow the overlay visibility, but only ever exist for Moon.
     * When making visible, restore them only on the Moon instance; when hiding,
     * always hide unconditionally. */
    if (visible && p->src == IMG_SRC_MOON) {
        update_moon_corner_labels(p);
    } else {
        hide_moon_corner_labels(p);
    }
}

void image_page_apply_theme(image_page_t *p)
{
    /* Re-apply the label style to every existing caption/corner label so a theme
     * switch recolours them live. apply_chip_style() reads current_theme and,
     * under Red Night, emits red text; non-red themes use white text. Each label
     * is NULL-guarded so this is safe before create() or after release_lvgl(),
     * and on the three instances that have no corner labels. */
    if (p->lbl_region)    apply_chip_style(p->lbl_region);
    if (p->lbl_timestamp) apply_chip_style(p->lbl_timestamp);
    if (p->lbl_moon_age)  apply_chip_style(p->lbl_moon_age);
    if (p->lbl_moon_next) apply_chip_style(p->lbl_moon_next);
    if (p->lbl_moon_rise) apply_chip_style(p->lbl_moon_rise);
    if (p->lbl_moon_set)  apply_chip_style(p->lbl_moon_set);

    /* Cloud Cover location marker follows the theme accent / Red Night rule.
     * No-op on the other five instances (s_clouds_marker is NULL there). */
    if (p->src == IMG_SRC_CLOUDS) marker_apply_theme();
    if (p->loading) nina_empty_state_apply_theme(p->loading, current_theme, app_config_get()->color_brightness);

    /* Ring pixels (Radar, Clouds) are recoloured once, at insert, so restyling
     * labels is not enough here: crossing the Red Night boundary changes both
     * the remap AND (see ring_bake) whether the radar dark treatment is forced, and
     * neither is reversible in place — the remap is lossy. Without this, a
     * switch INTO Red Night would leave up to ten bright frames looping for as
     * long as it takes the ring to roll over, on the one theme that must not put
     * light on the field.
     *
     * The fix is LOCAL: every slot keeps its compressed source bytes, so the
     * ring is re-derived from them under the new theme with no network. Request
     * it and return; the page's poll task does the work.
     *
     * Gated on the state actually flipping: a switch between two non-red themes
     * changes no ring pixel and must not rebuild anything. */
    if (ring_red_mismatch(p)) image_page_ring_request_retransform(p);
}

/* ─────────────────────────── instances & gates ─────────────────────────── */

static const char *const s_names[IMG_SRC_COUNT] = { "img_goes", "img_moon", "img_solar", "img_custom", "img_radar", "img_clouds" };

image_page_t *image_page_get(image_src_t src)
{
    return ((int)src >= 0 && (int)src < IMG_SRC_COUNT) ? &s_pages[src] : NULL;
}

image_page_t *image_page_by_page_idx(int page_idx)
{
    for (int s = 0; s < IMG_SRC_COUNT; s++) {
        if (s_pages[s].page_idx == page_idx) return &s_pages[s];
    }
    return NULL;
}

/* Recompute poll_gate from active||warm. This is a read-modify-write reachable
 * from three tasks (UI: set_active; data: prefetch/set_active; httpd: disable via
 * apply_live), so the three atomics alone are not enough — interleaving two
 * callers can land a stale false after a set_active(true) and park the poller for
 * a page that is on screen. The instance spinlock makes the recompute atomic
 * against itself; it is uncontended and held for three loads and a store. */
static void update_gate(image_page_t *p)
{
    portENTER_CRITICAL(&p->spawn_mux);
    atomic_store(&p->poll_gate, atomic_load(&p->active) || atomic_load(&p->warm));
    portEXIT_CRITICAL(&p->spawn_mux);
}

/* Drop the decoded frame (swap out under the mutex, free after unlock). Returns
 * false if frame_mux could not be taken (a long render_frame copy can hold it),
 * which the eviction loop uses as its termination guard. */
static bool frame_drop(image_page_t *p)
{
    uint8_t *old = NULL;
    if (!frame_lock(p, 200)) return false;
    old = p->frame.buf;
    p->frame.buf = NULL;
    p->frame.w = 0;
    p->frame.h = 0;
    p->frame.stamp_ms = 0;
    frame_unlock(p);
    if (old) heap_caps_free(old);
    return true;
}

/* Identity + mutexes. Eager, from app_main() right after app_config_init():
 * the web server starts BEFORE the dashboard, so an httpd worker can reach
 * image_page_get() concurrently with create_nina_dashboard()'s first
 * image_page_create() (which needs p->src correct to attach the Moon touch
 * handlers). Idempotent: identity is set only while frame_mux is NULL. */
void image_page_init(bool spawn_pollers)
{
    for (int s = 0; s < IMG_SRC_COUNT; s++) {
        image_page_t *p = &s_pages[s];
        if (p->frame_mux) continue;             /* already initialised */
        p->src = (image_src_t)s;
        p->page_idx = PAGE_IDX_IMG_GOES + s;
        p->name = s_names[s];
        portMUX_INITIALIZE(&p->spawn_mux);
        p->front_is_a = true;
        p->frame_mux = xSemaphoreCreateMutex();   /* last: it is the "initialised" marker */
    }
    image_page_poll_init();                          /* shared fetch gate (idempotent) */
    if (!spawn_pollers) return;
    for (int s = 0; s < IMG_SRC_COUNT; s++) {
        image_page_ensure_task_running(&s_pages[s]);
    }
}

/* 12288 bytes: TLS handshake + software JPEG decode (GOES/Solar/Custom) or the
 * tgx moon render + PPA scratch (Moon). Matches the former goes task. */
void image_page_ensure_task_running(image_page_t *p)
{
    if (!p || !image_page_config_enabled(app_config_get(), p->src)) return;
    psram_task_ensure(&p->task, &p->spawn_mux, image_page_poll_task, p->name, 12288, p, 3, 0);
}

void image_page_wake(image_page_t *p)
{
    if (p && p->task) xTaskNotifyGive(p->task);
}

void image_page_request_manual_fetch(image_page_t *p)
{
    if (!p) return;
    atomic_store(&p->manual_fetch, true);
    image_page_ensure_task_running(p);
    image_page_wake(p);
}

/* Registry ops show/hide land here (UI task, display lock held). */
void image_page_set_active(image_page_t *p, bool active)
{
    if (!p) return;
    bool was = atomic_exchange(&p->active, active);
    if (was == active) return;
    update_gate(p);
    if (active) {
        p->last_shown_ms = esp_timer_get_time() / 1000;   /* eviction order (LRU) */
        /* An animated page frees its whole ring on leave (see the ring block),
         * so every activation asks the poller to rebuild the history behind the
         * newest frame. The request is a no-op once the ring is full. */
        ring_request_backfill(p);
        image_page_ensure_task_running(p);
        image_page_render_frame(p);   /* a retained or warm frame shows instantly; no-op when empty */
        ring_loading_sync(p);         /* animated pages: placeholder until the loop has frames */
        image_page_wake(p);
        return;
    }
    /* Leaving: drop the LVGL copies now (we hold the display lock) and clear a
     * loading overlay a manual fetch may have left up. The decoded frame is
     * RETAINED (swipe-back re-shows it instantly; the resident cap is enforced
     * by image_page_evict_if_over_cap on later commits). Poke the poller so it
     * parks (Moon frees its texture in on_park unless it is still warm). */
    image_page_release_lvgl(p);
    /* A full ring is 6-10 MB — far past what the retained-frame budget was
     * sized for, and it is never counted by image_page_evict_if_over_cap (which
     * looks at p->frame, always NULL for an animated page). Free it outright
     * here rather than let a parked page sit on it; the next activation
     * backfills. No-op for the single-frame sources. */
    image_page_ring_reset(p);
    ring_loading_sync(p);             /* page hidden: placeholder down */
    nina_wait_overlay_hide();
    image_page_wake(p);
}

/* Warm exactly one page (the next slideshow stop) or none (-1). Every other
 * instance loses its warm flag; frames are retained (cap enforced elsewhere).
 * Called from the arbiter (data task); touches no LVGL. */
void image_page_prefetch(int page_idx)
{
    const app_config_t *c = app_config_get();
    for (int s = 0; s < IMG_SRC_COUNT; s++) {
        image_page_t *p = &s_pages[s];
        bool want = (p->page_idx == page_idx) && image_page_config_enabled(c, p->src);
        bool was = atomic_exchange(&p->warm, want);
        if (was == want) continue;
        update_gate(p);
        image_page_ensure_task_running(p);
        image_page_wake(p);   /* warm: start fetching; un-warmed: let the poller park */
    }
}

/* Config disable: this page leaves navigation, so nothing will show its
 * frame again; free it outright (does not count against the cap any more). */
void image_page_disable(image_page_t *p)
{
    if (!p) return;
    atomic_store(&p->warm, false);
    update_gate(p);
    frame_drop(p);
    /* An animated page holds its frames in the ring, not p->frame; take the
     * display lock (this function is called without it) and drop the whole
     * ring. No-op for the single-frame sources. */
    image_page_ring_invalidate(p);
    image_page_wake(p);
}

/* Resident cap (spec decision 2): while more than IMAGE_PAGE_MAX_RESIDENT
 * frames are resident, evict the least-recently-shown instance that is
 * neither active (on screen) nor warm (next slideshow stop). Two pinned +
 * one cap slot means at most one eviction per call in practice; the loop
 * covers the transient case where a page was disabled and re-enabled. */
void image_page_evict_if_over_cap(void)
{
    for (;;) {
        int resident = 0;
        image_page_t *victim = NULL;
        for (int s = 0; s < IMG_SRC_COUNT; s++) {
            image_page_t *p = &s_pages[s];
            bool have = false;
            if (frame_lock(p, 100)) {
                have = (p->frame.buf != NULL);
                frame_unlock(p);
            }
            if (!have) continue;
            resident++;
            if (atomic_load(&p->active) || atomic_load(&p->warm)) continue;   /* pinned */
            if (!victim || p->last_shown_ms < victim->last_shown_ms) victim = p;
        }
        if (resident <= IMAGE_PAGE_MAX_RESIDENT || !victim) return;
        ESP_LOGI(TAG, "%s: evicting retained frame (cap %d)", victim->name, IMAGE_PAGE_MAX_RESIDENT);
        /* Bail if the drop timed out: resident is unchanged, so re-looping would
         * re-pick the same victim forever. The next commit re-enforces the cap. */
        if (!frame_drop(victim)) {
            ESP_LOGW(TAG, "%s: frame_mux busy, eviction deferred", victim->name);
            return;
        }
    }
}

/* ─────────────────────────── config accessors ─────────────────────────── */

bool image_page_config_enabled(const app_config_t *c, image_src_t src)
{
    switch (src) {
        case IMG_SRC_GOES:   return c->goes_enabled;
        case IMG_SRC_MOON:   return c->moon_enabled;
        case IMG_SRC_SOLAR:  return c->solar_enabled;
        case IMG_SRC_CUSTOM: return c->custom_enabled;
        case IMG_SRC_RADAR:  return c->radar_enabled;
        case IMG_SRC_CLOUDS: return c->clouds_enabled;
        default:             return false;
    }
}

bool image_page_config_overlay(const app_config_t *c, image_src_t src)
{
    switch (src) {
        case IMG_SRC_GOES:   return c->goes_show_overlay;
        case IMG_SRC_MOON:   return c->moon_show_overlay;
        case IMG_SRC_SOLAR:  return c->solar_show_overlay;
        case IMG_SRC_CUSTOM: return c->custom_show_overlay;
        case IMG_SRC_RADAR:  return c->radar_show_overlay;
        case IMG_SRC_CLOUDS: return c->clouds_show_overlay;
        default:             return true;
    }
}

bool image_page_config_crop(const app_config_t *c, image_src_t src)
{
    switch (src) {
        case IMG_SRC_GOES:   return c->goes_crop;
        case IMG_SRC_SOLAR:  return c->solar_crop;
        case IMG_SRC_CUSTOM: return c->custom_crop;
        /* Radar's field is a 0..2 fit mode, not a flag; "any crop at all" is
         * all this predicate answers, and no radar path reaches it anyway
         * (render_frame returns early for radar, render_params_changed too).
         * The mode itself is read straight from config at insert, where the
         * radar_map_style gate lives; mirrored here so the predicate never
         * claims a crop that the bake would not apply. */
        case IMG_SRC_RADAR:  return c->radar_map_style == 0 && c->radar_crop != 0;
        default:             return false;   /* Moon and Clouds never crop */
    }
}

uint32_t image_page_interval_ms(image_page_t *p)
{
    const app_config_t *c = app_config_get();
    uint32_t s;
    switch (p->src) {
        case IMG_SRC_GOES:
            s = c->goes_update_interval_s;
            if (s < 300) s = 300;
            if (s > 7200) s = 7200;
            break;
        case IMG_SRC_SOLAR:
            s = c->solar_update_interval_s;
            if (s < 300) s = 300;
            if (s > 7200) s = 7200;
            break;
        case IMG_SRC_CUSTOM:
            s = c->custom_update_interval_s;
            if (s < 10) s = 10;
            if (s > 7200) s = 7200;
            break;
        case IMG_SRC_RADAR:
            /* RIDGE tiles refresh every few minutes; 120 s is the floor the
             * config clamp also enforces, so a stale blob cannot poll faster. */
            s = c->radar_update_interval_s;
            if (s < 120) s = 120;
            if (s > 7200) s = 7200;
            break;
        case IMG_SRC_CLOUDS:
            /* GIBS publishes on a 10 min grid (Air Mass is sparser still);
             * 300 s is the floor the config clamp also enforces. */
            s = c->clouds_update_interval_s;
            if (s < 300) s = 300;
            if (s > 7200) s = 7200;
            break;
        default: {   /* Moon: fast retry until SNTP sets the clock, then the configured cadence */
            time_t now;
            time(&now);
            if (now < (time_t)1577836800) return 3000;
            s = c->moon_update_interval_s;
            if (s < 10) s = 10;
            if (s > 3600) s = 3600;
            break;
        }
    }
    return s * 1000u;
}

/* A RIDGE token is a site id ("KTLX"), a region token ("SOUTHEAST") or "CONUS":
 * uppercase letters and digits only, 3..15 characters. Anything else is
 * rejected and the caller falls back to CONUS.
 *
 * This is NOT cosmetic validation, it is the choke point that enforces the
 * _loop.gif ban below. radar_frame_url() formats "%s_%d.gif", so the frame
 * index itself can never spell "loop" — but a token carrying a query or
 * fragment can smuggle one past it: radar_token = "KTLX_loop.gif?" builds
 * ".../KTLX_loop.gif?_0.gif", which serves the animated loop with the rest of
 * the path as a query string. Restricting the alphabet kills that, along with
 * path traversal and any other URL surgery through the token field. Enforcing
 * it here rather than at each URL site means a future radar URL cannot forget
 * it: every builder gets its token from this function. */
/* Implemented as radar_token_valid() in radar_play.h so the host suite can
 * prove the _loop.gif smuggling cases stay rejected. */

/* Resolved per call, never written back to config: image_page_poll.c calls this
 * from a poll task, and a poll task must not persist config. An explicit token
 * (a WSR-88D site id, a region token or CONUS) wins; otherwise a configured
 * weather location picks the nearest site; otherwise the national mosaic.
 *
 * HARD BAN — {TOKEN}_loop.gif MUST NEVER BE FETCHED OR DECODED. It is an
 * animated GIF whose stb path allocates every frame at once: 12.59 MiB in one
 * go plus 2.83 MiB of scratch, grown through nine successive reallocs that can
 * double-peak to 26.75 MiB, i.e. an intermittent OOM that depends on PSRAM
 * fragmentation. Its frames are inter-frame optimised too (one is a 78x9 patch
 * covering only the burnt-in timestamp), so it cannot be split into independent
 * stills either. The ten numbered stills _0.._9 ARE the loop and cost one frame
 * of memory at a time. radar_token_valid() (radar_play.h) is what keeps a
 * hand-typed token from reaching it. */
void image_page_radar_token(const app_config_t *c, char *out, size_t sz)
{
    if (!c || !out || sz == 0) return;
    if (radar_token_valid(c->radar_token)) {
        strlcpy(out, c->radar_token, sz);
    } else if (c->radar_token[0] == '\0' &&
               c->weather_lat != 0.0f && c->weather_lon != 0.0f) {
        strlcpy(out, radar_site_nearest(c->weather_lat, c->weather_lon), sz);
    } else {
        /* Empty with no location, or a token that failed validation: the
         * national mosaic is always a safe, always-available target. */
        strlcpy(out, "CONUS", sz);
    }
}

void image_page_label(image_page_t *p, char *out, size_t sz)
{
    const app_config_t *c = app_config_get();
    if (!out || sz == 0) return;
    out[0] = '\0';
    switch (p->src) {
        case IMG_SRC_GOES:   if (c->goes_region[0]) strlcpy(out, goes_region_name(c->goes_region), sz); break;
        case IMG_SRC_MOON:   strlcpy(out, "Moon", sz); break;
        case IMG_SRC_SOLAR:  strlcpy(out, solar_band_label(c->solar_band), sz); break;
        case IMG_SRC_CUSTOM: strlcpy(out, "Custom", sz); break;
        case IMG_SRC_RADAR: {
            char token[16];
            image_page_radar_token(c, token, sizeof(token));
            /* strlcpy/strlcat rather than snprintf: both truncate safely into
             * sz with no format-truncation diagnostic to satisfy. */
            strlcpy(out, "Radar ", sz);
            strlcat(out, token, sz);
            break;
        }
        case IMG_SRC_CLOUDS:
            /* Name the channel the way the Radar page names its site. */
            strlcpy(out, "Cloud Cover ", sz);
            strlcat(out, clouds_channel_label(c->clouds_channel), sz);
            break;
        default: break;
    }
}

bool image_page_get_error(image_page_t *p, char *out, size_t sz)
{
    if (!p || !out || sz == 0) return false;
    out[0] = '\0';
    if (frame_lock(p, 200)) {
        strlcpy(out, p->frame.error_msg, sz);
        frame_unlock(p);
    }
    return out[0] != '\0';
}

/* ───────────────────────── animation ring (Radar, Clouds) ────────────────────
 *
 * An ANIMATED image page (image_src_is_animated: Weather Radar, Clouds) keeps up
 * to N decoded stills (index 0 = newest) and plays them oldest -> newest on an
 * LVGL timer, holding longer on the newest. Everything else on such a page is
 * unchanged; the ring simply replaces p->frame as the storage, which is why
 * image_page_render_frame() dispatches here. One ring per instance
 * (s_rings[src]); the two animated pages never share state.
 *
 * LOCKING: the ring is guarded by the DISPLAY LOCK alone — no second mutex.
 * Every reader/mutator either runs on the UI task with the lock already held
 * (render, playback timer, set_active) or takes it itself (the poller's
 * image_page_ring_add / _invalidate). Never hold frame_mux across these.
 *
 * OWNERSHIP: the LVGL descriptor BORROWS a ring slot's buffer (release_dsc
 * skips borrowed slots), so ring->shown must be detached before that buffer
 * is freed — ring_free_slot() does exactly that.
 *
 * MEMORY: a full ten-frame ring is ~6.6 MB (radar, 600x550) or ~10 MB (clouds,
 * 720x720) — slots are kept at their NATIVE decoded size, the panel-width scale
 * happens per playback step — plus 3 MB of playback scratch (see below), several
 * times what a normal image page holds, so
 * IMAGE_PAGE_MAX_RESIDENT (which counts p->frame and therefore never counts a
 * ring) is not the right control. Instead the whole ring is freed when the
 * page is deactivated or disabled and rebuilt by the backfill on the next
 * activation. Two animated pages are never both resident: leaving one frees it.
 *
 * RETAINED SOURCE BYTES: each slot also keeps the frame's COMPRESSED source
 * (~37 KB radar GIF, ~90 KB clouds JPEG). The pixels carry the per-source bake
 * (radar: crop + night invert; both: the Red Night remap), so without the
 * source the only way to re-derive them was to re-download the whole ring.
 * With them, image_page_ring_retransform_if_requested() rebuilds the ring
 * locally. A slot therefore owns TWO allocations and ring_free_slot() is the
 * single place both are released.
 *
 * STAMPS: a slot may carry the frame's own time (uint32 UTC seconds, 0 =
 * unknown). Radar style 0 (RIDGE _i.gif stills) has no per-frame time to parse,
 * so those frames carry none and are ordered by insertion (head push / tail
 * append); so does a styles-1/2 site whose capabilities advertise no time list
 * (KHDC), which is one frame and no loop. Clouds frames carry the GIBS TIME and
 * radar styles 1/2 the advertised WMS TIME they were fetched for: a stamped
 * insert is placed by
 * time (newest first, whatever the caller's at_head hint), and a stamp already
 * held REPLACES that slot when the bytes differ (GIBS serves the newest one or
 * two frames partially at first) or is dropped as a duplicate when they match.
 */

typedef struct {
    uint8_t *buf;          /* RGB565, PSRAM, owned by the ring */
    uint16_t w, h;
    uint32_t hash;         /* FNV-1a over src (falls back to buf); dedupe key */
    uint8_t *src;          /* compressed source bytes, PSRAM, owned by the ring; NULL = not retained */
    size_t   src_len;
    uint32_t bake_gen;     /* ring->bake_gen these PIXELS were produced under */
    uint32_t stamp;        /* frame time, UTC seconds; 0 = unknown */
} ring_slot_t;

typedef struct {
    ring_slot_t     slots[RADAR_RING_MAX];
    _Atomic int     count;          /* resident frames; read lock-free by the poller */
    int             play_idx;       /* ring index currently on screen */
    const uint8_t  *shown;          /* buffer the LVGL descriptor borrows, or NULL */
    lv_timer_t     *timer;          /* playback timer; NULL when not animating */
    /* BAKE generation — distinct from the ring generation below, and answering a
     * different question. The ring generation asks "is this frame's CONTENT still
     * wanted?" (region / frame count); this one asks "were these PIXELS produced
     * under the settings in force now?" (crop, dark mode, Red Night).
     *
     * Bumped at the moment such a change is REQUESTED (the single funnel is
     * image_page_ring_request_retransform), recorded per slot by
     * image_page_ring_add() and re-stamped slot by slot as the re-transform pass
     * converts them. Playback advances only to matching slots (radar_play_next_baked),
     * so a ring that is momentarily half converted can never DISPLAY two geometries
     * — which is the property that makes the pumping structurally impossible rather
     * than merely unlikely.
     *
     * uint32_t for the same reason as gen: the esp-14.2.0 RISC-V subword atomic
     * sequence clobbers neighbouring bytes on narrow signed types. */
    _Atomic uint32_t bake_gen;
    uint32_t        bake_logged;    /* bake gen the "N slots stale" line was last logged for (UI task) */
    /* Ring generation: bumped by every reset (image_page_ring_reset, which
     * _invalidate, page leave and page disable all route through). A producer
     * captures it before resolving the token/times and hands it back to
     * image_page_ring_add(), which drops any frame carrying an older value. That
     * is what stops an in-flight backfill for the OLD region from repopulating the
     * ring a region switch just cleared. */
    _Atomic uint32_t gen;
    /* Deferred ring teardown. Raised when image_page_ring_invalidate() could not get
     * the display lock in time: the generation bump has already landed (so nothing in
     * flight can enter the ring), but the ring itself still holds frames baked under
     * the superseded settings and they have to go. Consumed by the poll task, which
     * CAN block for the lock. Cleared by image_page_ring_reset() too. */
    _Atomic bool    reset_req;
    /* Pending-backfill request: set on every activation by the UI task, consumed
     * by the poll task via image_page_ring_backfill_take(). */
    _Atomic bool    backfill_req;
    /* Pending local re-transform request (crop / dark-mode / Red Night change).
     * Set from the UI or httpd task, consumed by the poll task. */
    _Atomic bool    retransform_req;
    /* Red Night state the RESIDENT ring was recoloured with. Frames are recoloured
     * once, at insert, so image_page_apply_theme() needs this to tell a theme switch
     * that changes ring pixels from one that does not. Written and read only under
     * the display lock. */
    bool            ring_red;
    /* Playback scratch — see "ring playback scratch" below. Three page-owned
     * 720x720 RGB565 PSRAM buffers, allocated on the first show of a visit and
     * freed with the ring. NULL = not allocated (or the alloc failed, which
     * latches scratch_failed for the rest of the visit). */
    uint8_t        *cur;            /* the picture ON SCREEN; the LVGL descriptor borrows it */
    uint8_t        *nxt;            /* SRM staging for a slot that is not 720 wide */
    uint8_t        *mix;            /* dissolve output, becomes the next `cur` */
    uint16_t        cur_h;          /* rows valid in cur; 0 = nothing shown yet */
    bool            scratch_failed; /* alloc failed this visit: cut, do not dissolve */
    /* Running dissolve. fade_fg is `nxt` or a ring slot's buffer; both stay
     * alive for the dissolve because ring_free_slot() lands it first. */
    const uint8_t  *fade_fg;
    uint16_t        fade_h;
    int64_t         fade_t0;        /* esp_timer us at the start of the dissolve */
    lv_timer_t     *fade_timer;     /* NULL = no dissolve running */
} image_ring_t;

static image_ring_t s_rings[IMG_SRC_COUNT];   /* only the animated sources' entries are ever touched */

static inline image_ring_t *ring_of(image_page_t *p)
{
    return (p && image_src_is_animated(p->src)) ? &s_rings[p->src] : NULL;
}

/* Raise the pending-backfill request (UI task, page activation). No-op for a
 * single-frame source. */
static void ring_request_backfill(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (r) atomic_store(&r->backfill_req, true);
}

/* True when a resident ring was baked under a different Red Night state than
 * the theme now active (image_page_apply_theme). Display lock held. */
static bool ring_red_mismatch(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    return r && atomic_load(&r->count) > 0 && r->ring_red != image_red_remap_active();
}

int image_page_ring_capacity(image_page_t *p)
{
    const app_config_t *c = app_config_get();
    uint8_t n = (p && p->src == IMG_SRC_CLOUDS) ? c->clouds_frames : c->radar_frames;
    if (n < 1) n = 1;
    if (n > RADAR_RING_MAX) n = RADAR_RING_MAX;
    return (int)n;
}

int image_page_ring_count(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    return r ? atomic_load(&r->count) : 0;
}

bool image_page_ring_backfill_take(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    return r ? atomic_exchange(&r->backfill_req, false) : false;
}

uint32_t image_page_ring_gen(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    return r ? atomic_load(&r->gen) : 0;
}

/* Lock-free scan for a held stamp: only the poll task inserts and only resets
 * clear, so a torn read costs at most one redundant fetch. 0 never matches. */
bool image_page_ring_has_stamp(image_page_t *p, uint32_t stamp)
{
    image_ring_t *r = ring_of(p);
    if (!r || stamp == 0) return false;
    int n = atomic_load(&r->count);
    if (n > RADAR_RING_MAX) n = RADAR_RING_MAX;
    for (int i = 0; i < n; i++) {
        if (r->slots[i].stamp == stamp) return true;
    }
    return false;
}

/* Resident slot nearest in time to @p stamp and fit to judge a frame against:
 * usable pixels, a stamp of its own, and the CURRENT bake. @p skip is a slot
 * index to exclude (-1 = none), which is how the head re-judge keeps the head
 * from being compared with itself. -1 when the ring has no such slot.
 * Display lock held. */
static int ring_neighbour_idx(image_ring_t *r, uint32_t stamp, int skip)
{
    int n = atomic_load(&r->count);
    if (n > RADAR_RING_MAX) n = RADAR_RING_MAX;
    uint32_t cur = atomic_load(&r->bake_gen);
    int best = -1;
    uint32_t best_d = 0;
    bool best_same = true;
    for (int i = 0; i < n; i++) {
        ring_slot_t *s = &r->slots[i];
        if (i == skip || !s->buf || s->stamp == 0 || s->bake_gen != cur) continue;
        uint32_t d = (s->stamp > stamp) ? s->stamp - stamp : stamp - s->stamp;
        bool same = (d == 0);
        /* A different stamp always beats the same stamp (a re-fetch must not be
         * judged against its own earlier, possibly partial, version). */
        if (best < 0 || (best_same && !same) || (same == best_same && d < best_d)) {
            best = i; best_d = d; best_same = same;
        }
    }
    return best;
}

bool image_page_ring_with_neighbour(image_page_t *p, uint32_t stamp,
                                    bool (*fn)(const uint16_t *ref, int w, int h, void *arg),
                                    void *arg)
{
    image_ring_t *r = ring_of(p);
    if (!r || !fn || stamp == 0) return false;
    if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) return false;
    int best = ring_neighbour_idx(r, stamp, -1);
    bool ret = false;
    if (best >= 0) {
        ring_slot_t *s = &r->slots[best];
        ret = fn((const uint16_t *)s->buf, (int)s->w, (int)s->h, arg);
    }
    bsp_display_unlock();
    return ret;
}

/* Re-judge the HEAD slot (index 0, the newest) with the same predicate the
 * insert gate runs — the one frame that gate cannot see, because every page
 * entry frees the ring and the head therefore lands in it with no neighbour to
 * be compared against.
 *
 * @p fn is handed the NEIGHBOUR's pixels and, as its `arg`, an image_frame_t
 * view of the head slot, so the caller reuses its insert-time adapter verbatim
 * rather than growing a second callback shape. The view is stack-local and the
 * display lock is held across the call, so neither buffer can be freed under
 * fn; fn must not take the lock itself and must not retain either pointer.
 *
 * Returns the head's stamp when fn says the head is bad — feed it straight to
 * image_page_ring_drop_stamp(). 0 when the head is fine, when the ring holds
 * fewer than two frames, when the head is unstamped or stale-baked, when no
 * neighbour qualifies, or when the lock times out. */
uint32_t image_page_ring_judge_head(image_page_t *p,
                                    bool (*fn)(const uint16_t *ref, int w, int h, void *arg))
{
    image_ring_t *r = ring_of(p);
    if (!r || !fn) return 0;
    if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) return 0;
    uint32_t bad = 0;
    ring_slot_t *hd = &r->slots[0];
    if (atomic_load(&r->count) >= 2 && hd->buf && hd->stamp != 0 &&
        hd->bake_gen == atomic_load(&r->bake_gen)) {
        int ref = ring_neighbour_idx(r, hd->stamp, 0);
        if (ref >= 0) {
            image_frame_t view = { .buf = hd->buf, .w = hd->w, .h = hd->h };
            ring_slot_t *s = &r->slots[ref];
            if (fn((const uint16_t *)s->buf, (int)s->w, (int)s->h, &view)) bad = hd->stamp;
        }
    }
    bsp_display_unlock();
    return bad;
}

/* ───────────────────────── ring playback scratch ─────────────────────────────
 *
 * THE DISSOLVE RUNS ON THE PPA BLEND ENGINE, NOT ON THE CPU.
 *
 * Playback used to hand the ring slot straight to LVGL and cross-dissolve with
 * an lv_anim on the top image's opa. In FULL_REFRESH mode every animation tick
 * of that is a 720x720 SOFTWARE alpha blend: measured on the fleet, the Cloud
 * Cover page sat at 44 % SoC load with the two LVGL swdraw tasks at 40 % and
 * 38 %, against 1.8 % idle on the Clock page. Frames are already 720 wide for
 * Clouds, so the scaling was not the cost — the blend was.
 *
 * Three page-owned 720x720 RGB565 PSRAM buffers per animated page carry the new
 * scheme:
 *
 *   cur  the picture currently ON SCREEN. The LVGL descriptor BORROWS it, so
 *        release_dsc() never frees it — the same contract moon_copy_buf uses.
 *   nxt  staging for a slot that is not already 720 wide (one PPA SRM pass).
 *   mix  the dissolve output, and the buffer that becomes the next `cur`.
 *
 * INVARIANT, whenever the scratch is available:
 *   no dissolve  -> the descriptor points at `cur`, cur_h rows valid
 *   dissolving   -> the descriptor points at `mix`; `cur` holds the OUTGOING
 *                   picture and fade_fg the INCOMING one, both 720 x fade_h
 *
 * A commit is one blend at full alpha (the output is then the foreground
 * exactly — 565 -> 888 -> 565 round trips losslessly) followed by rotating
 * cur <-> mix, so committing and the last dissolve step are the same operation
 * and neither costs a CPU memcpy of a megabyte on the LVGL task.
 *
 * Per step the CPU does: one blocking PPA blend per ~33 ms tick (a few ms of
 * DMA each) and LVGL's 1:1 blit of `mix`. The shown image stays at
 * LV_OPA_COVER and nothing animates.
 *
 * If PSRAM cannot give the three buffers, the page falls back to the pre-PPA
 * behaviour for the rest of the visit: the descriptor borrows the ring slot and
 * every show is an instant cut (swap_borrowed_buf still scales a native-size
 * slot the way it always did).
 */

/* 1,036,800 B — a 128 B multiple, which is what the PPA demands of an output
 * buffer's SIZE as well as its address. */
#define RING_SCRATCH_BYTES ((size_t)screen_size() * screen_size() * 2)
#define RING_FADE_STEP_MS  33          /* ~30 blends/s while a dissolve runs */

/* Stop a running dissolve without committing it. Display lock held. */
static void ring_fade_stop(image_ring_t *r)
{
    if (r->fade_timer) {
        lv_timer_delete(r->fade_timer);
        r->fade_timer = NULL;
    }
    r->fade_fg = NULL;
    r->fade_h  = 0;
}

/* Free the three scratch buffers. Display lock held. The descriptor borrows the
 * shown one and release_dsc() skips borrowed buffers, so this is the ONLY place
 * they are released — and it drops the descriptor first so LVGL is never left
 * on freed memory. Clears scratch_failed too: the next visit gets a fresh try
 * on a heap that may have recovered. */
static void ring_scratch_free(image_page_t *p, image_ring_t *r)
{
    ring_fade_stop(r);
    uint8_t *bufs[3] = { r->cur, r->nxt, r->mix };
    for (int i = 0; i < 3; i++) {
        if (bufs[i] && (p->dsc_a.data == bufs[i] || p->dsc_b.data == bufs[i])) {
            image_page_release_lvgl(p);
            break;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (bufs[i]) heap_caps_free(bufs[i]);
    }
    r->cur = r->nxt = r->mix = NULL;
    r->cur_h = 0;
    r->scratch_failed = false;
}

/* Allocate the scratch on the first show of a visit. Logs ONCE per visit on
 * failure and latches, so a fragmented heap cannot turn every playback step
 * into a warning. Display lock held. */
static bool ring_scratch_ensure(image_page_t *p, image_ring_t *r)
{
    if (r->cur && r->nxt && r->mix) return true;
    if (r->scratch_failed) return false;

    uint8_t **slot[3] = { &r->cur, &r->nxt, &r->mix };
    for (int i = 0; i < 3; i++) {
        if (*slot[i]) continue;
        *slot[i] = heap_caps_aligned_alloc(128, RING_SCRATCH_BYTES, MALLOC_CAP_SPIRAM);
        if (!*slot[i]) {
            ring_scratch_free(p, r);       /* give the partial allocation back */
            r->scratch_failed = true;      /* set AFTER the free, which clears it */
            ESP_LOGW(TAG, "%s: no PSRAM for the playback scratch (3 x %u KB); frames will cut",
                     p->name, (unsigned)(RING_SCRATCH_BYTES / 1024));
            return false;
        }
    }
    r->cur_h = 0;                          /* nothing valid in a fresh `cur` */
    return true;
}

/* Scale a NATIVE-size ring slot up to the panel width into `nxt` with one PPA
 * SRM pass. Returns the rows written, 0 if the hardware refused.
 *
 * Ring slots are stored at their native size (WMS/RIDGE radar tiles are 600 px
 * or less) because a 720-wide ring costs ~10 MB for a ten-frame radar loop
 * against ~6.6 MB native. Paying one hardware scale per PLAYBACK STEP instead
 * of one per FETCHED frame is the trade: the step already runs several blends,
 * and the SRM engine is a different engine from the Blend one. Clouds frames
 * are already 720 wide and never come here at all. */
static int ring_fit_into(image_ring_t *r, const ring_slot_t *s)
{
    image_fit_t fit;
    uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    if (!image_fit_pick(s->w, s->h, screen_size(), &fit) ||
        !image_fit_logical_to_source(&fit, s->w, s->h, 0, &bx, &by, &bw, &bh)) {
        return 0;
    }
    ppa_srm_job_t job = {
        .src           = s->buf,
        .src_stride_px = s->w,
        .src_h         = s->h,
        .block_x       = bx,
        .block_y       = by,
        .block_w       = bw,
        .block_h       = bh,
        .rotate_cw     = 0,
        .hflip         = false,
        .vflip         = false,
        .dst           = r->nxt,
        .dst_buf_size  = RING_SCRATCH_BYTES,
        .dst_w         = screen_size(),
        .dst_h         = fit.out_h,
        .dst_x         = fit.dst_x,
        .dst_y         = 0,
        .scale_n16     = fit.n16,
        .clear_dst     = (fit.out_w < screen_size()),
    };
    return (ppa_srm_rgb565(&job) == ESP_OK) ? (int)fit.out_h : 0;
}

/* Land @p fg (720 x @p h) in `cur` and on screen. The full-alpha blend IS the
 * copy — DMA, not a megabyte of memcpy on the LVGL task — and it writes `mix`,
 * so the roles rotate afterwards and the displayed buffer is always `cur`.
 * On a hardware refusal cur_h is cleared so the next show cuts instead of
 * dissolving from a picture that is not the one on screen. Display lock held. */
static bool ring_commit(image_page_t *p, image_ring_t *r, const uint8_t *fg, int h)
{
    if (ppa_blend_rgb565(r->cur, fg, r->mix, screen_size(), (uint32_t)h, 255) != ESP_OK) {
        r->cur_h = 0;
        return false;
    }
    uint8_t *t = r->cur;
    r->cur = r->mix;
    r->mix = t;
    r->cur_h = (uint16_t)h;
    swap_borrowed_buf(p, (const uint16_t *)r->cur, screen_size(), h);
    return true;
}

/* One dissolve step: blend `cur` and the incoming frame into `mix` at the alpha
 * @p el ms into the fade. Display lock held. */
static bool ring_fade_blend(image_ring_t *r, int el)
{
    uint32_t a = (el <= 0) ? 0u : ((uint32_t)el * 255u) / RADAR_PLAY_FADE_MS;
    if (a > 255u) a = 255u;
    return ppa_blend_rgb565(r->cur, r->fade_fg, r->mix,
                            screen_size(), r->fade_h, (uint8_t)a) == ESP_OK;
}

/* End a running dissolve NOW, at full alpha: the incoming frame lands in `cur`
 * and on screen exactly as the last timer step would have left it. This is the
 * ring's cancel_moon_crossfade — every path that must not leave a half-mixed
 * picture behind (a new frame, a slot being freed, page teardown) calls it.
 * Display lock held. */
static void ring_fade_finish(image_page_t *p, image_ring_t *r)
{
    const uint8_t *fg = r->fade_fg;
    int h = r->fade_h;
    ring_fade_stop(r);
    /* No LVGL objects (page never built, or torn down): the timer is gone, which
     * is the part that mattered; nothing is on screen to land. */
    if (fg && h > 0 && p->root) ring_commit(p, r, fg, h);
}

/* Runs on the LVGL task, which holds the display lock. */
static void ring_fade_cb(lv_timer_t *t)
{
    image_page_t *p = lv_timer_get_user_data(t);
    image_ring_t *r = ring_of(p);
    if (!r) return;
    if (!r->fade_fg || !r->cur || !r->mix) {
        ring_fade_stop(r);
        return;
    }
    int el = (int)((esp_timer_get_time() - r->fade_t0) / 1000);
    /* Past the duration, or the page went away mid-dissolve: land it. A failed
     * blend lands it too — one cut beats a frozen half-mix. */
    if (el >= RADAR_PLAY_FADE_MS || !atomic_load(&p->active) || !ring_fade_blend(r, el)) {
        ring_fade_finish(p, r);
        return;
    }
    /* The descriptor already points at `mix`; only its pixels changed, so a
     * plain invalidate is the whole update. */
    lv_obj_invalidate(p->img_front);
}

/* Start a dissolve from `cur` to @p fg. Returns false if the first blend failed,
 * leaving the caller to commit instantly instead. Display lock held; no
 * dissolve may be running. */
static bool ring_fade_start(image_page_t *p, image_ring_t *r, const uint8_t *fg, int h)
{
    r->fade_fg = fg;
    r->fade_h  = (uint16_t)h;
    /* Backdated one step so the FIRST painted frame has already moved: starting
     * at alpha 0 would paint a copy of what is already on screen and spend a
     * blend doing it. */
    r->fade_t0 = esp_timer_get_time() - (int64_t)RING_FADE_STEP_MS * 1000;
    if (!ring_fade_blend(r, RING_FADE_STEP_MS)) {
        ring_fade_stop(r);
        return false;
    }
    swap_borrowed_buf(p, (const uint16_t *)r->mix, screen_size(), h);
    r->fade_timer = lv_timer_create(ring_fade_cb, RING_FADE_STEP_MS, p);
    if (!r->fade_timer) {
        ring_fade_finish(p, r);            /* no timer: land the frame now */
    }
    return true;
}

/* Put a ring slot on screen, dissolving from the frame already there when asked
 * and the two are the same shape. Display lock held; the slot is non-empty. */
static void ring_play(image_page_t *p, image_ring_t *r, const ring_slot_t *s, bool fade)
{
    /* A dissolve still running belongs to the PREVIOUS frame: land it before
     * this one takes over, so `cur` describes what is actually on screen. */
    if (r->fade_timer) ring_fade_finish(p, r);

    if (!ring_scratch_ensure(p, r)) {
        swap_borrowed_buf(p, (const uint16_t *)s->buf, (int)s->w, (int)s->h);
        return;
    }

    /* A 720-wide slot IS the foreground — no copy, no scale. Anything else gets
     * one SRM pass into `nxt`. */
    const uint8_t *fg = s->buf;
    int fg_h = (int)s->h;
    if (s->w != screen_size() || s->h > screen_size()) {
        fg_h = ring_fit_into(r, s);
        if (fg_h <= 0) {
            swap_borrowed_buf(p, (const uint16_t *)s->buf, (int)s->w, (int)s->h);
            r->cur_h = 0;
            return;
        }
        fg = r->nxt;
    }

    if (fade && r->cur_h == fg_h && ring_fade_start(p, r, fg, fg_h)) return;
    /* Instant, and the fallback for a hardware refusal: borrow the slot the way
     * playback did before the scratch existed. */
    if (!ring_commit(p, r, fg, fg_h)) {
        swap_borrowed_buf(p, (const uint16_t *)s->buf, (int)s->w, (int)s->h);
    }
}

/* Free one slot — BOTH its allocations, the decoded pixels and the retained
 * compressed source. Display lock held. Detaches the LVGL descriptors first if
 * they borrow this slot's buffer, so the free can never leave LVGL on freed
 * memory. Every ring teardown routes through here (image_page_ring_reset over
 * all RADAR_RING_MAX slots, the oldest-end eviction and the same-stamp replace
 * in _add), so there is one place to get the pairing right. */
static void ring_free_slot(image_page_t *p, image_ring_t *r, int i)
{
    ring_slot_t *s = &r->slots[i];
    /* A dissolve in flight may be reading THIS slot as its foreground, and in
     * any case leaves the screen showing `mix` rather than `cur`. Land it first:
     * afterwards the displayed picture lives entirely in the page-owned scratch,
     * so freeing the slot below cannot pull it out from under LVGL. */
    if (r->fade_timer) ring_fade_finish(p, r);
    if (s->buf) {
        const uint8_t *b = (const uint8_t *)s->buf;
        if (b == r->shown || b == p->dsc_a.data || b == p->dsc_b.data) {
            image_page_release_lvgl(p);  /* drops both descriptors + borrowed flags */
            r->shown = NULL;
        }
    }
    if (s->buf) heap_caps_free(s->buf);
    if (s->src) heap_caps_free(s->src);
    memset(s, 0, sizeof(*s));
}

/* Put ring slot @p idx on screen (idx < 0 = the current cursor). Display lock
 * held. No-op when the page has no LVGL objects or the slot is empty. */
static void ring_show_idx(image_page_t *p, int idx, bool fade)
{
    image_ring_t *r = ring_of(p);
    if (!r) return;
    int count = atomic_load(&r->count);
    if (idx < 0) idx = r->play_idx;
    if (idx < 0 || idx >= count) return;

    ring_slot_t *s = &r->slots[idx];
    if (!s->buf || !p->root || s->w == 0 || s->h == 0) return;

    ring_play(p, r, s, fade);
    r->shown    = s->buf;
    r->play_idx = idx;

    char label[48];
    image_page_label(p, label, sizeof(label));
    set_label_if_changed(p->lbl_region, label);

    /* A stamped frame shows its own time (local clock); the newest carries the
     * "Latest" prefix. Unstamped rings (radar style 0, and a styles-1/2 site
     * with no advertised time list): the newest frame is stamped with the wall
     * clock and the history frames show their position in the loop instead of a
     * time we would be inventing. */
    char ts[32];
    struct tm ti;
    if (s->stamp != 0) {
        time_t t = (time_t)s->stamp;
        localtime_r(&t, &ti);
        strftime(ts, sizeof(ts), idx == 0 ? "Latest %H:%M" : "%H:%M", &ti);
    } else if (idx == 0) {
        time_t now; time(&now); localtime_r(&now, &ti);
        strftime(ts, sizeof(ts), "Latest %H:%M", &ti);
    } else {
        snprintf(ts, sizeof(ts), "Loop %d/%d", count - idx, count);
    }
    set_label_if_changed(p->lbl_timestamp, ts);
}

/* True if slot @p i holds pixels baked under the settings in force now. Display
 * lock held. Out-of-range answers false so a caller can use it as a guard. */
static bool ring_slot_current(image_ring_t *r, int i)
{
    if (i < 0 || i >= atomic_load(&r->count)) return false;
    return r->slots[i].bake_gen == atomic_load(&r->bake_gen);
}

static void ring_tick_cb(lv_timer_t *t)
{
    image_page_t *p = lv_timer_get_user_data(t);
    image_ring_t *r = ring_of(p);
    if (!r) return;
    int count = atomic_load(&r->count);
    if (count <= 1 || !atomic_load(&p->active)) return;

    /* Advance only to a slot baked under the CURRENT settings. Mid-re-transform
     * the rest still hold the old crop / dark treatment, and showing them is
     * what made the picture pump between two sizes; they are skipped outright
     * until the pass re-stamps them. Same index back = no current-bake slot
     * anywhere, so hold on what is displayed rather than blank the page. */
    uint32_t gens[RADAR_RING_MAX];
    if (count > RADAR_RING_MAX) count = RADAR_RING_MAX;
    for (int i = 0; i < count; i++) gens[i] = r->slots[i].bake_gen;
    uint32_t cur_gen = atomic_load(&r->bake_gen);

    /* One line per bake change, not per tick: how much of the ring is waiting on
     * the re-transform. Makes the skip visible from /api/logs. */
    if (cur_gen != r->bake_logged) {
        r->bake_logged = cur_gen;
        ESP_LOGI(TAG, "%s playback: %d/%d slots stale after bake change (skipped)",
                 p->name, radar_stale_count(gens, count, cur_gen), count);
    }

    int next = radar_play_next_baked(r->play_idx, count, gens, cur_gen);
    if (next == r->play_idx) return;
    ring_show_idx(p, next, true);
    lv_timer_set_period(t, radar_play_period_ms(next));
}

/* Create or delete the playback timer to match the current state. Display lock
 * held. A single still (frames == 1, or a ring that has not filled past one
 * frame) never animates, matching the other image pages. */
/* Frames the loop must hold before anything is put on screen: 3, or the whole
 * capacity when it is smaller. One frame is a still, two flicker. */
static int ring_show_gate(image_page_t *p)
{
    int c = image_page_ring_capacity(p);
    return c < 3 ? c : 3;
}

/* Show or hide the loading placeholder to match the ring state. Display lock
 * held. "Loading" = the page is visible, nothing has been shown yet and the
 * ring is still under the gate; once a frame is up the placeholder never
 * returns for that visit (a ring shrunk by memory pressure keeps playing). The
 * Cloud Cover marker hides with it so the page is not a lone dot on black. */
static void ring_loading_sync(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r || !p->loading) return;
    int count = atomic_load(&r->count);
    int gate  = ring_show_gate(p);
    bool loading = atomic_load(&p->active) && r->shown == NULL && count < gate;
    if (loading) {
        /* Progress in the title so the wait reads as work, not a hang: each
         * frame is a ~3 s download + decode. Shown as a countdown of frames
         * still to come (3, 2, 1) on its own line, so it reads as a counter
         * rather than a stray number next to the words. */
        char title[48];
        snprintf(title, sizeof(title), "Loading %s\n%d",
                 p->src == IMG_SRC_CLOUDS ? "Cloud Cover" : "Weather Radar", gate - count);
        nina_empty_state_set_title(p->loading, title);
        nina_empty_state_show(p->loading);
        nina_empty_state_set_busy(p->loading, true);
        if (p->src == IMG_SRC_CLOUDS) marker_set_visible(false);
    } else {
        nina_empty_state_set_busy(p->loading, false);
        nina_empty_state_hide(p->loading);
        if (p->src == IMG_SRC_CLOUDS) marker_set_visible(app_config_get()->clouds_show_location);
    }
}

static void ring_timer_sync(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r) return;
    bool want = atomic_load(&p->active) &&
                image_page_ring_capacity(p) > 1 &&
                atomic_load(&r->count) > 1 &&
                r->shown != NULL;              /* nothing plays before the gate */
    if (want && !r->timer) {
        r->timer = lv_timer_create(ring_tick_cb, RADAR_PLAY_FRAME_MS, p);
    } else if (!want && r->timer) {
        lv_timer_delete(r->timer);
        r->timer = NULL;
    }
}

/* Free the OLDEST resident frame so the next decode has somewhere to go.
 *
 * WHY: a full ring is 6-10 MB and nothing ever shrank it. Once PSRAM is
 * fragmented past the point where a decode can get its blocks, the fetch fails,
 * the poll spine backs off, and the poller retries the same doomed decode
 * forever — the NEWEST frame stops updating while the old ones keep animating.
 * This is the release valve; the caller (anim_decode_headroom in
 * image_page_poll.c) decides when to pull it.
 *
 * LOCKING / CURSOR: identical to the oldest-end eviction inside
 * image_page_ring_add() — display lock taken here (this runs on the page's poll
 * task, outside the lock, exactly like _add), ring_free_slot() for the pairing
 * of pixels + retained source and for detaching the LVGL descriptor if that
 * buffer is the one on screen. The only case _add does not have to handle is the
 * cursor sitting ON the freed tail (it always inserts afterwards and re-shows),
 * so clamp play_idx and put the newest frame up instead; ring_timer_sync() then
 * stops the timer if a single frame is left.
 *
 * Never drops the last frame: the newest must survive or the page goes blank. */
bool image_page_ring_drop_oldest(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r || atomic_load(&r->count) <= 1) return false;
    if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) return false;

    int count = atomic_load(&r->count);
    if (count <= 1) {                      /* re-tested under the lock */
        bsp_display_unlock();
        return false;
    }
    ring_free_slot(p, r, count - 1);
    count--;
    atomic_store(&r->count, count);
    if (r->play_idx >= count) {            /* the cursor was on the slot just freed */
        r->play_idx = 0;
        if (atomic_load(&p->active)) ring_show_idx(p, 0, false);   /* same gate as _add */
    }
    ring_timer_sync(p);
    bsp_display_unlock();

    /* Deliberately NO backfill re-arm. The backfill assumes an empty ring (its
     * URL index i lands in slot i); on a partial ring it would fetch the wrong
     * history slot (Radar) or re-download the stamp just evicted (Clouds), once
     * per poll, forever. A ring shrunk under memory pressure stays shorter until
     * the next page activation rebuilds it. */
    return true;
}

/* Free the slot carrying @p stamp (the head, in practice: the re-judge in
 * image_page_poll.c is the only caller) so the poller's next newest fetch
 * re-downloads it and the same-stamp path in _add installs the completed frame.
 *
 * LOCKING / CURSOR / DISSOLVE: identical to image_page_ring_drop_oldest() —
 * display lock taken here (the page's poll task, outside the lock, exactly like
 * _add), and the free routed through ring_free_slot(), which LANDS ANY RUNNING
 * DISSOLVE first. That is what makes dropping a slot that may be the live
 * foreground of a dissolve safe: after ring_fade_finish() the displayed picture
 * lives entirely in the page-owned scratch (`cur`), never in a ring slot, and
 * ring_free_slot() then detaches the LVGL descriptors as well if the scratch
 * was unavailable and they borrow this slot directly. Unlike the oldest-end
 * eviction this one can free a slot in the MIDDLE of the list, so the slots
 * above it are compacted down by one and the playback cursor follows them.
 *
 * false — freeing nothing — when @p stamp is 0, no slot carries it, the ring
 * holds one frame or none (the newest must survive or the page goes blank), or
 * the lock times out. Like _drop_oldest it does NOT re-arm the backfill. */
bool image_page_ring_drop_stamp(image_page_t *p, uint32_t stamp)
{
    image_ring_t *r = ring_of(p);
    if (!r || stamp == 0 || atomic_load(&r->count) <= 1) return false;
    if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) return false;

    int count = atomic_load(&r->count);
    if (count > RADAR_RING_MAX) count = RADAR_RING_MAX;
    int i = -1;
    for (int k = 0; k < count; k++) {
        if (r->slots[k].stamp == stamp) { i = k; break; }
    }
    if (count <= 1 || i < 0) {             /* both re-tested under the lock */
        bsp_display_unlock();
        return false;
    }
    ring_free_slot(p, r, i);               /* lands the dissolve, detaches LVGL, frees both allocations */
    for (int k = i; k < count - 1; k++) r->slots[k] = r->slots[k + 1];
    memset(&r->slots[count - 1], 0, sizeof(r->slots[0]));
    count--;
    atomic_store(&r->count, count);
    if (r->play_idx > i) r->play_idx--;    /* the cursor's slot shifted down with the rest */
    if (r->play_idx >= count) r->play_idx = 0;
    /* ring_free_slot() clears `shown` when it had to detach the descriptors, so
     * this is exactly the "nothing is on screen any more" test the same-stamp
     * replace in _add uses. */
    if (!r->shown && atomic_load(&p->active)) ring_show_idx(p, r->play_idx, false);
    ring_timer_sync(p);
    bsp_display_unlock();
    return true;
}

void image_page_ring_reset(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r) return;
    if (r->timer) {
        lv_timer_delete(r->timer);
        r->timer = NULL;
    }
    for (int i = 0; i < RADAR_RING_MAX; i++) ring_free_slot(p, r, i);
    /* The playback scratch is the ring's other display resource (3 MB): it goes
     * with the frames, on the same page-leave / invalidate paths, and is rebuilt
     * on the first show after the next activation. */
    ring_scratch_free(p, r);
    atomic_store(&r->count, 0);
    r->play_idx = 0;
    r->shown    = NULL;
    /* Nothing left to re-transform; a request raised against the ring we just
     * freed would otherwise run one no-op pass later. Same for a deferred
     * teardown: the ring is gone, so honouring it later could only free a ring
     * built AFTER the change that asked for it. */
    atomic_store(&r->retransform_req, false);
    atomic_store(&r->reset_req, false);
    /* The bake generation is deliberately NOT reset: it is a monotonic counter,
     * and every slot of the ring being freed here goes with it. The next ring's
     * frames stamp themselves with whatever it reads then, so they agree with
     * each other and with the playback cursor from the first frame on. */
    /* Every ring reset invalidates every fetch already in flight, whatever the
     * reason for the reset: a region/frame-count/crop change (via _invalidate),
     * a page leave, or a page disable. Bumping here rather than at each of
     * those call sites means a future reset path cannot forget it. Runs with
     * the display lock held, the same lock image_page_ring_add() tests it
     * under, so there is no window between the bump and the free. */
    atomic_fetch_add(&r->gen, 1);
    ring_loading_sync(p);
}

void image_page_ring_invalidate(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r) return;
    /* Bump the generation FIRST, UNCONDITIONALLY, and OUTSIDE the display lock.
     * It is an atomic and needs no lock, and it is the half that actually
     * guarantees correctness: every producer captured the old value before it
     * started, so image_page_ring_add() drops every frame already in flight
     * under the superseded settings. Doing this inside the lock (as it was)
     * meant a 1 s lock timeout skipped BOTH the bump and the clear, leaving a
     * ring that could genuinely mix old and new content. image_page_ring_reset()
     * bumps again below; a double bump is harmless, the frame check is an
     * inequality, not a delta. */
    atomic_fetch_add(&r->gen, 1);
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        image_page_ring_reset(p);
        bsp_display_unlock();
    } else {
        /* Hand the teardown to the poll task, which can block for the lock.
         * Until it runs, the ring keeps showing the OLD frames: stale, never
         * mixed. Worth a line in /api/logs — a silent 1 s lock timeout is exactly
         * the kind of thing that should leave a trace. */
        ESP_LOGW(TAG, "%s invalidate: display lock timeout, deferring ring reset", p->name);
        atomic_store(&r->reset_req, true);
        image_page_wake(p);
    }
    atomic_store(&r->backfill_req, true);
}

/* Service a teardown image_page_ring_invalidate() had to defer.
 *
 * THIS PAGE'S POLL TASK ONLY, and only at a point where no insert is in flight:
 * the top of net_poll_once() (before any image_page_ring_add() this pass) and
 * the park hook. image_page_ring_add() is called only from that same task, so
 * "no concurrent insert" is structural, not a timing argument.
 *
 * The generation was already bumped by the invalidate, so this is a pure free of
 * frames nothing will show again — a no-op on an already-empty ring. A second
 * lock timeout re-raises the request rather than dropping it; nothing was freed,
 * so retrying can neither leak nor double free. */
void image_page_ring_reset_if_requested(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r || !atomic_exchange(&r->reset_req, false)) return;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        image_page_ring_reset(p);
        bsp_display_unlock();
        return;
    }
    ESP_LOGW(TAG, "%s deferred ring reset: display lock still busy, retrying", p->name);
    atomic_store(&r->reset_req, true);
}

/* Center-crop a decoded radar frame in place-of-allocation: returns a fresh
 * smaller PSRAM buffer and frees the original. Applied ONCE per decode (the
 * playback path borrows the stored buffer and cannot crop), which is why a
 * live radar_crop change re-derives every frame from its retained source bytes.
 * Returns false and leaves the frame untouched if the allocation fails — an
 * uncropped frame beats none.
 *
 * `mode` is a RADAR_FIT_* value passed in rather than read from config, so the
 * geometry (radar_fit_rect(), radar_play.h) stays pure and host-testable. There
 * are two modes: 0 keeps the whole published picture, and ANY non-zero value
 * drops the NOAA header/legend rows and crops what is left to a centred square.
 * The render path scales to the panel WIDTH and centres vertically, so a square
 * frame lands on exactly 720x720 and center_image_y() resolves to 0 — the bars
 * go away without moving the image. Non-zero rather than == 1 is deliberate: it
 * is where a device still holding the retired mode 2 gets resolved. The origin
 * is NOT (x,0), which is why the copy below indexes rows from oy, not 0. */
static bool radar_crop_frame(image_frame_t *f, uint8_t mode)
{
    uint16_t w, h, ox, oy;
    if (!radar_fit_rect(mode, f->w, f->h, &w, &h, &ox, &oy)) return false;

    size_t   dst_stride = (size_t)w * 2;
    size_t   src_stride = (size_t)f->w * 2;
    uint8_t *out = heap_caps_malloc(dst_stride * h, MALLOC_CAP_SPIRAM);
    if (!out) return false;
    for (uint32_t y = 0; y < h; y++) {
        memcpy(out + (size_t)y * dst_stride,
               f->buf + (size_t)(oy + y) * src_stride + (size_t)ox * 2,
               dst_stride);
    }
    heap_caps_free(f->buf);
    f->buf = out;
    f->w   = w;
    f->h   = h;
    return true;
}

/* Per-source display transforms a ring frame carries — THE ONE PLACE these are
 * applied. Both the insert path and the local re-transform call it, so a frame
 * re-derived from its source bytes is pixel-identical to one that came off the
 * wire under the same settings.
 *
 * Radar: the centred crop, the night invert and the Red Night remap, in that
 * order. Night readability: invert the RIDGE tile's greyscale basemap (white
 * sheet -> black, dark state lines -> light) and leave every chromatic pixel —
 * the whole dBZ echo ramp — untouched (image_night_invert.h). Gated on
 * radar_dark_mode (default true, v64) EXCEPT under Red Night, which forces the
 * dark treatment whatever the toggle says: the red remap maps luma to red
 * WITHOUT lowering it, so the light basemap would come out a BRIGHT red panel,
 * precisely what that theme exists to prevent. Invert BEFORE the remap: the
 * remap collapses luma to red shades and would destroy the neutral/chromatic
 * distinction the invert keys on. Map styles 1/2 come from a different NWS
 * service (black background, no banner, no legend): the crop has nothing to
 * trim and the invert would turn that black sheet WHITE, so both are skipped
 * by design there, Red Night included; the theme-gated remap still runs.
 *
 * Clouds: no crop, no invert on any channel (GeoColor is already a dark night
 * picture, and Clean IR / Air Mass carry meaning in their own grey/colour ramp
 * that an invert would reverse); only the theme-gated Red Night remap.
 *
 * `cfg` and `red` are passed in rather than read here so the re-transform can
 * bake a whole ring from ONE snapshot of the settings.
 *
 * CROP AND INVERT ARE NOT ACCELERATED, deliberately: the PPA offers SRM
 * (scale/rotate/mirror), BLEND and FILL only — there is no colour-transform
 * unit for the invert — and the crop is already a row-wise memcpy. Software is
 * correct for both, and they stay cheap because the frame is still at its
 * NATIVE size here: a ring slot is stored as decoded (and cropped), never
 * scaled to the panel. Playback does the scale, once per step, on the PPA
 * (ring_fit_into). */
static void ring_bake(image_page_t *p, image_frame_t *f, const app_config_t *cfg, bool red)
{
    if (p->src == IMG_SRC_RADAR) {
        bool    dark = cfg->radar_map_style == 0 && (cfg->radar_dark_mode || red);
        uint8_t mode = cfg->radar_map_style ? 0 : cfg->radar_crop;
        radar_crop_frame(f, mode);            /* best-effort; keeps the frame either way */
        if (dark) image_night_invert_rgb565((uint16_t *)f->buf, (size_t)f->w * f->h);
    }
    image_red_remap_rgb565((uint16_t *)f->buf, (size_t)f->w * f->h);   /* self-gated on the active theme */
}

/* Free everything a rejected frame owns. Both allocations are taken over by
 * image_page_ring_add() on entry, so every one of its reject paths must land
 * here — one helper instead of five hand-paired frees. */
static void ring_drop(image_frame_t *f, uint8_t *src)
{
    if (f && f->buf) { heap_caps_free(f->buf); f->buf = NULL; }
    if (src) heap_caps_free(src);
}

/* Move a fresh frame's allocations into slot @p i (which must be empty or already
 * freed). Display lock held. */
static void ring_fill_slot(image_ring_t *r, int i, image_frame_t *fresh, uint32_t hash,
                           uint8_t *src, size_t src_len, uint32_t bake_gen, uint32_t stamp)
{
    r->slots[i].buf      = fresh->buf;
    r->slots[i].w        = fresh->w;
    r->slots[i].h        = fresh->h;
    r->slots[i].hash     = hash;
    r->slots[i].src      = src;      /* ownership moves into the slot */
    r->slots[i].src_len  = src_len;
    r->slots[i].bake_gen = bake_gen;
    r->slots[i].stamp    = stamp;
    fresh->buf = NULL;
}

void image_page_ring_add(image_page_t *p, image_frame_t *fresh, bool at_head, uint32_t gen,
                         uint8_t *src, size_t src_len, uint32_t stamp)
{
    image_ring_t *r = ring_of(p);
    if (!r || !fresh || !fresh->buf) {
        ring_drop(fresh, src);
        return;
    }

    /* SEAM: this is the ring equivalent of the invert+remap block in
     * image_page_render_frame() — animated pages return early from that
     * function because their frames live in the ring, not p->frame. Here is
     * the only place a ring frame is committed; it runs once per FETCHED frame
     * on the poll task (image_page_poll.c net_poll_once / anim_backfill),
     * outside the display lock. The bake is applied to the pixels — but the
     * compressed source is kept alongside, so a crop / dark-mode / Red-Night
     * change re-bakes the ring locally (image_page_ring_retransform_if_requested)
     * instead of re-downloading it. Only a token / area / frame-count change,
     * which genuinely means different images, still rebuilds over the network. */
    /* ORDERING CONTRACT, same shape as the ring generation: read the bake
     * generation BEFORE the settings it labels. The writer changes the setting
     * first and bumps second, so gen-then-settings can only ever pair OLD pixels
     * with an OLD generation (skipped by playback until the pass re-bakes them,
     * conservative and correct) or NEW pixels with a NEW one. Settings-then-gen
     * could label old pixels as current — which is the pumping bug. */
    uint32_t bake_gen = atomic_load(&r->bake_gen);
    const app_config_t *cfg = app_config_get();
    bool red = image_red_remap_active();
    ring_bake(p, fresh, cfg, red);

    /* Dedupe on the COMPRESSED bytes when they were retained: kilobytes hashed
     * instead of a megabyte, and — the reason it matters — the key is invariant
     * under the transforms above, so a local re-transform never has to
     * recompute it. Falls back to the pixel hash if a caller did not retain. */
    uint32_t hash = src ? radar_fnv1a(src, src_len)
                        : radar_fnv1a(fresh->buf, (size_t)fresh->w * fresh->h * 2);

    bool notify = false;
    if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        ring_drop(fresh, src);
        return;
    }

    /* STALENESS CHOKE POINT. Every ring frame that ever enters passes through
     * here, so this is the one place a frame fetched for a region (or crop, or
     * frame count) the user has already left can be rejected. Tested INSIDE the
     * display lock because that is the lock image_page_ring_reset() bumps the
     * generation under; testing it before the lock would leave the original
     * race in a smaller window. The producers' own abort checks are a bandwidth
     * optimisation on top of this, never a substitute. */
    if (radar_frame_is_stale(gen, atomic_load(&r->gen))) {
        bsp_display_unlock();
        ring_drop(fresh, src);
        return;
    }

    int count = atomic_load(&r->count);
    int cap   = image_page_ring_capacity(p);

    /* Past every reject path that depends on content, so record the Red Night
     * state these pixels were baked with. Every frame in the ring shares it (a
     * flip clears the ring), so one flag covers the whole ring. Display lock
     * held, which is the lock image_page_apply_theme() reads it under. */
    r->ring_red = red;

    int pos;   /* slot the frame lands in */
    if (stamp != 0) {
        /* Stamped: same stamp already held -> replace when the bytes differ
         * (a partial frame healed), drop when identical. Otherwise insert by
         * time so the ring stays newest-first even when a gap fills in later. */
        int same = -1;
        for (int i = 0; i < count; i++) if (r->slots[i].stamp == stamp) { same = i; break; }
        if (same >= 0) {
            if (r->slots[same].hash == hash) {
                bsp_display_unlock();
                ring_drop(fresh, src);
                return;
            }
            bool was_shown = (r->slots[same].buf && (const uint8_t *)r->slots[same].buf == r->shown);
            ring_free_slot(p, r, same);           /* detaches LVGL if it was on screen */
            ring_fill_slot(r, same, fresh, hash, src, src_len, bake_gen, stamp);
            if (atomic_load(&p->active)) {
                /* Re-show only if the healed slot was on screen (its buffer is
                 * gone) or nothing is up yet; otherwise playback carries on. */
                if (!r->shown && atomic_load(&r->count) < ring_show_gate(p)) {
                    ring_loading_sync(p);
                } else {
                    if (was_shown || !r->shown) ring_show_idx(p, was_shown ? same : 0, false);
                    ring_loading_sync(p);
                    ring_timer_sync(p);
                }
                notify = true;
            }
            bsp_display_unlock();
            if (notify) nav_arbiter_notify_content_ready(p->page_idx);
            return;
        }
        pos = 0;
        while (pos < count && r->slots[pos].stamp > stamp) pos++;
        if (pos >= cap) {                          /* older than everything a full ring holds */
            bsp_display_unlock();
            ring_drop(fresh, src);
            return;
        }
        /* Neighbour dedupe as for the unstamped path: two consecutive slots
         * with identical bytes (a re-served or blank frame) are one frame. */
        uint32_t neighbour = (pos > 0) ? r->slots[pos - 1].hash
                           : (count > 0 ? r->slots[0].hash : 0);
        if (radar_frame_is_dup(neighbour, count, hash)) {
            bsp_display_unlock();
            ring_drop(fresh, src);
            return;
        }
    } else {
        /* Unstamped (radar): dedupe against the entry this frame would sit next
         * to — the head for a newest-frame push, the tail for a backfill append.
         * RIDGE re-serves the same still between updates, so this is the common
         * case, not an edge. */
        uint32_t neighbour = count > 0 ? r->slots[at_head ? 0 : count - 1].hash : 0;
        if (radar_frame_is_dup(neighbour, count, hash)) {
            bsp_display_unlock();
            ring_drop(fresh, src);
            return;
        }
        if (!at_head && count >= cap) {            /* ring already full: nothing to backfill */
            bsp_display_unlock();
            ring_drop(fresh, src);
            return;
        }
        pos = at_head ? 0 : count;
    }

    /* Make room: evict from the oldest end while full (a head/mid insert into a
     * full ring pushes the tail out; pos < cap is guaranteed above). */
    while (count >= cap) {
        ring_free_slot(p, r, count - 1);
        count--;
    }
    for (int i = count; i > pos; i--) r->slots[i] = r->slots[i - 1];
    memset(&r->slots[pos], 0, sizeof(r->slots[pos]));
    /* Everything from pos on shifted down one slot, so did the frame on screen. */
    if (r->play_idx >= pos && r->play_idx + 1 < cap) r->play_idx++;
    count++;
    ring_fill_slot(r, pos, fresh, hash, src, src_len, bake_gen, stamp);
    atomic_store(&r->count, count);

    if (atomic_load(&p->active)) {
        /* Show the newest frame immediately when nothing is up yet (page entry,
         * or the displayed slot was just evicted); otherwise let the timer keep
         * playing and only refresh the caption's frame count.
         *
         * ...and also when the cursor is parked on a STALE-bake slot, which is
         * where playback holds while a re-transform is still catching up. The
         * frame just inserted was baked live, so it is the one slot guaranteed
         * current: jumping to it is what ends that hold at the first opportunity
         * instead of waiting for the pass. */
        if (!r->shown && count < ring_show_gate(p)) {
            ring_loading_sync(p);             /* keep the placeholder up */
        } else {
            /* First show of the visit goes to the NEWEST frame (index 0), not
             * to the slot just inserted, which past the gate is history. */
            ring_show_idx(p, !r->shown ? 0
                             : (ring_slot_current(r, r->play_idx) ? -1 : pos), false);
            ring_loading_sync(p);
            ring_timer_sync(p);
        }
        notify = true;
    }
    bsp_display_unlock();
    /* Content-ready dwell for the slideshow: the first frame that lands while
     * the page is on screen is what the arbiter waits for. Same call
     * image_page_commit_frame() makes for the single-frame pages. */
    if (notify) nav_arbiter_notify_content_ready(p->page_idx);
}

void image_page_ring_request_retransform(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r) return;

    /* THE SINGLE FUNNEL for every pixel-affecting ring setting: the radar crop
     * and dark-mode toggles (radar_local_params_changed, image_page_config_apply_live)
     * and the Red Night theme flip (image_page_apply_theme) both land here. So
     * this is the one place the bake generation has to be bumped, and a future
     * pixel-affecting setting gets the skip for free by routing here too.
     *
     * Bumped at REQUEST time, not when the pass starts: the window this closes
     * is exactly the gap between the two — 18.4 s in the logged failure, because
     * the pass could not start until a backfill finished. From this instruction
     * on, every resident slot reads as stale and playback skips it, so the mixed
     * ring is never displayed however long the pass takes to get going.
     *
     * Unconditional, ahead of the empty-ring return below: it is a monotonic
     * counter, and stamping later inserts with the newer value is exactly right. */
    atomic_fetch_add(&r->bake_gen, 1);

    /* Nothing resident means nothing to re-derive: the next fetch already bakes
     * the new settings in (image_page_ring_add reads them live). Without this,
     * a change made while the page is away would re-bake the frames the backfill
     * has only just fetched under exactly those settings. */
    if (atomic_load(&r->count) == 0) return;
    atomic_store(&r->retransform_req, true);
    image_page_wake(p);
}

/* Re-derive the whole ring from the retained compressed bytes under the CURRENT
 * settings and theme. Replaces the staggered re-download a crop, dark-mode or
 * theme change used to cost, on a board that glitches its panel under sustained
 * radio transmission.
 *
 * RUNS ON THE PAGE'S POLL TASK (it blocks and allocates megabytes), never on the
 * LVGL timer. ONE FRAME AT A TIME: peak extra memory is a single decode plus its
 * crop output, not ten.
 *
 * LOCKING: the display lock is taken only to detach the source, and again to
 * swap the pixel pointer in — never around the decode. A slot's `src` is
 * detached (set NULL) for the duration of its decode, so a concurrent
 * image_page_ring_reset() frees the slot's pixels but not the source we are
 * reading; the generation check on re-attach then tells us the slot is gone and
 * we free both allocations ourselves. The playback timer can never see a
 * half-swapped slot: it runs on the UI task, which needs the same lock, and the
 * slot's old buffer stays valid and displayed until the swap completes.
 *
 * NO CONCURRENT INSERT is possible: image_page_ring_add() is called only from
 * this same task, so nothing can shift the ring under a slot index while this
 * runs. Only resets race, and the generation covers those.
 *
 * ponytail: a frame whose decode fails keeps its OLD pixels rather than being
 * dropped and re-fetched — one stale-looking frame in the loop after an OOM,
 * which the next ring rollover clears. Dropping it would mean compacting the
 * ring mid-pass for a case that needs PSRAM exhaustion to reach.
 *
 * Returns false ONLY when a display-lock timeout cut the pass short, which is
 * the one abort that can leave the ring genuinely part converted: the caller
 * re-raises the request so a later cycle finishes the job. A pass stopped by the
 * generation check returns true — that ring is being freed, not left mixed. */
static bool ring_retransform_pass(image_page_t *p, image_ring_t *r)
{
    int count = atomic_load(&r->count);
    if (count <= 0) return true;
    bool aborted = false;

    /* ONE snapshot of the settings for the whole pass, so the ring cannot end up
     * half baked one way and half the other. The bake generation is read FIRST,
     * same ordering contract as image_page_ring_add(): a change landing between
     * these two reads leaves this pass stamping the OLDER generation, so its
     * output reads as stale and the next pass (the request is re-raised) redoes
     * it — conservative, and it converges on the latest settings. */
    uint32_t bake_gen = atomic_load(&r->bake_gen);
    const app_config_t *c = app_config_get();
    bool     red  = image_red_remap_active();
    uint32_t gen  = atomic_load(&r->gen);

    /* Claim the Red Night state for this pass UP FRONT. image_page_apply_theme()
     * compares the live theme against it, so a second flip arriving mid-pass
     * sees a mismatch and raises another request instead of being swallowed.
     *
     * Same lock reads the playback cursor: the slot ON SCREEN is converted FIRST
     * so the visible picture resizes exactly once, promptly, and the rest convert
     * behind the hold. */
    int start = 0;
    if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        r->ring_red = red;
        start = r->play_idx;
        bsp_display_unlock();
    }

    int64_t t0 = esp_timer_get_time();
    int done = 0;
    ESP_LOGI(TAG, "%s re-transform: start, %d frames from slot %d (playback held)",
             p->name, count, start);
    for (int k = 0; k < count; k++) {
        int i = radar_retransform_idx(start, k, count);
        if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            aborted = true;
            break;
        }
        if (radar_frame_is_stale(gen, atomic_load(&r->gen)) ||
            i >= atomic_load(&r->count)) {
            bsp_display_unlock();
            break;
        }
        uint8_t *src = r->slots[i].src;
        size_t   len = r->slots[i].src_len;
        r->slots[i].src = NULL;              /* detached: this task owns it now */
        bsp_display_unlock();
        if (!src) continue;                  /* nothing retained for this slot */

        image_frame_t f = {0};
        uint8_t *px = NULL;
        uint32_t w = 0, h = 0;
        size_t   sz = 0;
        if (jpeg_decode_rgb565(src, len, &px, &w, &h, &sz) && px) {
            f.buf = px;
            f.w   = (uint16_t)w;
            f.h   = (uint16_t)h;
            ring_bake(p, &f, c, red);
        } else if (px) {
            heap_caps_free(px);
        }

        if (!bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
            ring_drop(&f, src);
            aborted = true;
            break;
        }
        if (radar_frame_is_stale(gen, atomic_load(&r->gen)) ||
            i >= atomic_load(&r->count)) {
            bsp_display_unlock();
            ring_drop(&f, src);              /* the slot is gone; we own both */
            break;
        }
        r->slots[i].src = src;               /* hand the source back, decoded or not */
        if (!f.buf) {
            bsp_display_unlock();
            /* Keeps its OLD pixels and, deliberately, its OLD bake generation:
             * playback skips it instead of showing a frame at the superseded
             * geometry. It rejoins the loop when the ring rolls it out or a
             * later pass decodes it. */
            ESP_LOGW(TAG, "%s re-transform: frame %d kept its old pixels (skipped in playback)", p->name, i);
            continue;
        }
        uint8_t *old = r->slots[i].buf;
        bool was_shown = (old && (const uint8_t *)old == r->shown);
        if (was_shown) {
            image_page_release_lvgl(p);      /* stop borrowing before the free */
            r->shown = NULL;
        }
        r->slots[i].buf = f.buf;
        r->slots[i].w   = f.w;
        r->slots[i].h   = f.h;
        /* Stamped BEFORE the re-show below, so the slot is already eligible for
         * playback by the time it is on screen. */
        r->slots[i].bake_gen = bake_gen;
        /* .hash is over the compressed bytes, so it survives the re-bake. */
        if (was_shown) ring_show_idx(p, i, false);
        bsp_display_unlock();
        if (old) heap_caps_free(old);
        done++;
        vTaskDelay(pdMS_TO_TICKS(10));       /* stay polite between ~50 ms decodes */
    }
    ESP_LOGI(TAG, "%s re-transform: %d/%d frames in %d ms (no network)",
             p->name, done, count, (int)((esp_timer_get_time() - t0) / 1000));
    return !aborted;
}

/* Re-bake the ring under the current settings.
 *
 * NO PLAYBACK HOLD. There used to be one and it was the wrong shape: it covered
 * the PASS, while the ring is mixed from the moment the setting CHANGES. Per-slot
 * bake generations cover the whole window by construction and need nothing
 * raised or lowered.
 *
 * DOUBLE REQUEST: a second toggle mid-pass bumps the bake generation again and
 * re-raises retransform_req, so the loop runs another pass with a fresh
 * settings snapshot; the slots the first pass converted now read as stale and
 * are re-converted. The ring converges on the LATEST settings, and playback
 * shows only slots that have caught up throughout. A reset clears the request,
 * which is what ends the loop when the ring goes away. */
void image_page_ring_retransform_if_requested(image_page_t *p)
{
    image_ring_t *r = ring_of(p);
    if (!r || !atomic_exchange(&r->retransform_req, false)) return;
    if (atomic_load(&r->count) <= 0) return;

    while (1) {
        if (!ring_retransform_pass(p, r)) {
            /* Display lock timed out mid-pass, so the ring is part converted.
             * Re-raise and wake ourselves rather than retrying inline (that
             * would spin on a jammed lock); the next poll cycle finishes the
             * ring. Playback meanwhile runs over the converted slots only, or
             * holds on the displayed frame if none converted. */
            ESP_LOGW(TAG, "%s re-transform: display lock timeout, retrying next cycle", p->name);
            atomic_store(&r->retransform_req, true);
            image_page_wake(p);
            break;
        }
        if (!atomic_exchange(&r->retransform_req, false)) break;
    }
}

/* ─────────────────────────── poller -> page handoff ────────────────────── */

/* Stores the fresh frame ALWAYS (spec decision 2: a fetch that lands after the
 * gate closed is RETAINED, not dropped, so a swipe-back or lock-return re-shows
 * it instantly and the poller's freshness check skips the re-download), then
 * renders only when the page is on screen. The resident cap is re-enforced
 * after every commit, so a retained frame on a page that is neither active nor
 * warm is the first eviction candidate. The only remaining drop path is a
 * frame_mux timeout: there is nowhere to put the buffer. */
void image_page_commit_frame(image_page_t *p, image_frame_t *fresh, bool force)
{
    if (!p || !fresh) return;
    uint8_t *old = NULL;
    bool stored = false;
    if (frame_lock(p, 1000)) {
        old = p->frame.buf;
        p->frame = *fresh;   /* buf ownership moves; w/h/stamp_ms/error_msg follow */
        fresh->buf = NULL;
        stored = true;
        frame_unlock(p);
    }
    if (old) heap_caps_free(old);
    if (!stored) {
        if (fresh->buf) heap_caps_free(fresh->buf);
        fresh->buf = NULL;
        return;
    }
    image_page_evict_if_over_cap();   /* this commit may have made a 4th frame resident */
    if (atomic_load(&p->active) && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        if (force) image_page_force_redraw(p);
        image_page_render_frame(p);
        bsp_display_unlock();
        nav_arbiter_notify_content_ready(p->page_idx);   /* outside the display lock */
    }
}

void image_page_set_error(image_page_t *p, const char *msg)
{
    if (!p) return;
    if (frame_lock(p, 200)) {
        strlcpy(p->frame.error_msg, msg ? msg : "", sizeof(p->frame.error_msg));
        frame_unlock(p);
    }
    /* The Custom, Radar and Clouds pages show the reason as their caption;
     * render_frame() handles the error branch before the newer-frame gate, so a
     * plain call suffices. */
    if (image_page_shows_error(p->src) &&
        atomic_load(&p->active) && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
        image_page_render_frame(p);
        bsp_display_unlock();
    }
}

/* ─────────────────────────── live config apply ─────────────────────────── */

/* Per-source "needs a fresh download" test: region / band / URL changed. */
static bool source_params_changed(image_src_t s, const app_config_t *prev, const app_config_t *cur, bool force_fetch)
{
    switch (s) {
        case IMG_SRC_GOES:   return strcmp(cur->goes_region, prev->goes_region) != 0;
        case IMG_SRC_SOLAR:  return cur->solar_band != prev->solar_band;
        case IMG_SRC_CUSTOM: return force_fetch || strcmp(cur->custom_image_url, prev->custom_image_url) != 0;
        case IMG_SRC_RADAR:
            /* ONLY the things that mean DIFFERENT IMAGES belong here. The token
             * (explicitly, or implicitly when it is empty/auto and the weather
             * location moved, which re-picks the nearest site) and the frame
             * count both change which files must be downloaded.
             *
             * radar_crop and radar_dark_mode do NOT: they are local display
             * transforms of images the ring already holds the compressed bytes
             * for, and are handled by radar_local_params_changed() below.
             *
             * radar_frames stays a refetch even when it DECREASES (dropping the
             * surplus slots would work, but it is a rarely-touched setting and
             * "keep it simple" beats a second shrink-only path). */
            if (strcmp(cur->radar_token, prev->radar_token) != 0) return true;
            if (cur->radar_frames != prev->radar_frames) return true;
            /* radar_map_style picks a different NWS service: different images. */
            if (cur->radar_map_style != prev->radar_map_style) return true;
            return cur->radar_token[0] == '\0' &&
                   (cur->weather_lat != prev->weather_lat ||
                    cur->weather_lon != prev->weather_lon);
        case IMG_SRC_CLOUDS:
            /* Channel (a different GIBS layer), basemap (different vector
             * overlay layers, baked into the JPEG the server returns), area
             * (zoom), frame count and the weather location all change which
             * GIBS frames are fetched: DIFFERENT images, so the ring goes. */
            return cur->clouds_channel != prev->clouds_channel ||
                   cur->clouds_basemap != prev->clouds_basemap ||
                   cur->clouds_zoom != prev->clouds_zoom ||
                   cur->clouds_frames != prev->clouds_frames ||
                   cur->weather_lat != prev->weather_lat ||
                   cur->weather_lon != prev->weather_lon;
        default:             return false;   /* Moon: local render, handled below */
    }
}

/* Radar-only "re-derive the ring locally" test: purely how the SAME pictures are
 * drawn. Both are baked into the stored pixels, so they cannot be re-rendered on
 * the fly like the other pages' crop — but the ring keeps each frame's
 * compressed bytes, so re-deriving costs a decode, not a download.
 * Deliberately NOT gated on radar_map_style: on styles 1/2 both settings are
 * ignored at bake time, so a toggle there costs one harmless local re-derive
 * that lands on identical pixels. Cheaper than a second predicate. */
static bool radar_local_params_changed(const app_config_t *prev, const app_config_t *cur)
{
    return cur->radar_crop != prev->radar_crop ||
           cur->radar_dark_mode != prev->radar_dark_mode;
}

/* Per-source "re-render the cached frame locally" test: crop / rotation / flips. */
static bool render_params_changed(image_src_t s, const app_config_t *prev, const app_config_t *cur)
{
    /* Animated pages are not re-rendered from p->frame (they have none); radar's
     * local changes go through radar_local_params_changed() above and Clouds
     * has no local render parameters at all. */
    if (image_src_is_animated(s)) return false;
    if (image_page_config_crop(cur, s) != image_page_config_crop(prev, s)) return true;
    switch (s) {
        case IMG_SRC_GOES:   return cur->goes_orientation != prev->goes_orientation || cur->goes_vflip != prev->goes_vflip || cur->goes_hflip != prev->goes_hflip;
        case IMG_SRC_SOLAR:  return cur->solar_orientation != prev->solar_orientation || cur->solar_vflip != prev->solar_vflip || cur->solar_hflip != prev->solar_hflip;
        case IMG_SRC_CUSTOM: return cur->custom_orientation != prev->custom_orientation || cur->custom_vflip != prev->custom_vflip || cur->custom_hflip != prev->custom_hflip;
        /* Radar has no orientation/flip settings: RIDGE tiles are already
         * north-up, so the shared crop test above is its whole re-render gate. */
        case IMG_SRC_RADAR:  return false;
        default:             return false;
    }
}

static bool moon_params_changed(const app_config_t *prev, const app_config_t *cur)
{
    return cur->moon_bg_style != prev->moon_bg_style ||
           cur->moon_flip_u != prev->moon_flip_u ||
           cur->moon_flip_v != prev->moon_flip_v ||
           cur->moon_roll_offset != prev->moon_roll_offset ||
           cur->moon_yaw_offset != prev->moon_yaw_offset ||
           cur->moon_pitch_offset != prev->moon_pitch_offset ||
           cur->moon_north_up != prev->moon_north_up ||
           cur->moon_spin_mode != prev->moon_spin_mode ||
           cur->moon_spin_return_s != prev->moon_spin_return_s ||
           cur->moon_drag_light_mode != prev->moon_drag_light_mode ||
           cur->moon_lat != prev->moon_lat ||
           cur->moon_lon != prev->moon_lon;
}

/* Apply image-page config changes live (preview) for all six pages, comparing
 * prev to cur. Shared by the image config POST handler, the control registry
 * and config_trigger_side_effects (main Save), so live-apply lives once:
 *   - enable toggled: create/hide the page (display lock), start or park its poller;
 *   - overlay toggled: show/hide the caption bar;
 *   - region/band/URL changed (or force_fetch on Custom): manual fetch (overlay + download);
 *   - Moon parameters changed: wake the Moon poller (local re-render, no overlay);
 *   - crop/rotation/flip only: re-render the cached frame locally, ONLY if the
 *     page is on screen (a hidden page re-renders on its next activate anyway,
 *     and rendering it here would allocate a crossfade copy nobody sees). */
void image_page_config_apply_live(const app_config_t *prev, const app_config_t *cur, bool force_fetch)
{
    for (int s = 0; s < IMG_SRC_COUNT; s++) {
        image_page_t *p = &s_pages[s];
        bool en_prev = image_page_config_enabled(prev, (image_src_t)s);
        bool en_cur  = image_page_config_enabled(cur,  (image_src_t)s);
        bool ov_prev = image_page_config_overlay(prev, (image_src_t)s);
        bool ov_cur  = image_page_config_overlay(cur,  (image_src_t)s);

        if (en_prev != en_cur || ov_prev != ov_cur) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                nina_dashboard_set_image_page_enabled(s, en_cur);
                image_page_set_overlay_visible(p, ov_cur);
                bsp_display_unlock();
            }
            if (en_cur && !en_prev) image_page_ensure_task_running(p);
            if (!en_cur && en_prev) image_page_disable(p);   /* clear warm, free the frame */
        }
        /* Cloud Cover location marker: pure visibility, no fetch. Deliberately
         * NOT part of source_params_changed() — the frames are identical either
         * way, so toggling it must never invalidate the ring. */
        if (s == IMG_SRC_CLOUDS && prev->clouds_show_location != cur->clouds_show_location) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                marker_set_visible(cur->clouds_show_location);
                bsp_display_unlock();
            }
        }

        if (!en_cur) continue;

        if (source_params_changed((image_src_t)s, prev, cur, force_fetch)) {
            /* Animated pages: these are DIFFERENT images (region / area / frame
             * count), so the ring goes and the poller refetches. No-op otherwise. */
            image_page_ring_invalidate(p);
            image_page_request_manual_fetch(p);            /* overlay + fresh download */
        } else if (s == IMG_SRC_RADAR && radar_local_params_changed(prev, cur)) {
            /* Same images, drawn differently: re-derive the ring from the
             * retained compressed bytes. No network, no ring teardown. */
            image_page_ring_request_retransform(p);
        } else if (s == IMG_SRC_MOON && moon_params_changed(prev, cur)) {
            image_page_ensure_task_running(p);
            image_page_wake(p);                              /* local re-render, no overlay */
        } else if (render_params_changed((image_src_t)s, prev, cur) && atomic_load(&p->active)) {
            if (bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
                image_page_force_redraw(p);
                image_page_render_frame(p);                  /* re-crop/rotate the cached frame */
                bsp_display_unlock();
            }
        }
    }
}
