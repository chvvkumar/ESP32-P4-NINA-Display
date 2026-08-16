/**
 * @file nina_image_page.c
 * @brief Image page spine: rendering + lifecycle for the four image pages
 *        (GOES / Moon / Solar / Custom). Ported from nina_image_display.c;
 *        every former module-level static is now a field of image_page_t.
 *
 * Threading: every LVGL call below runs under the display lock held by the
 * CALLER, except image_page_commit_frame() / image_page_set_error(), which
 * take it themselves (documented in the header). Never take the display lock
 * while holding p->frame_mux.
 */

#include "nina_image_page.h"
#include "nina_wait_overlay.h"
#include "nina_dashboard.h"            /* nina_dashboard_set_image_page_enabled (config apply, B5) */
#include "nina_dashboard_internal.h"   /* SCREEN_SIZE, OUTER_PADDING, current_theme, PAGE_IDX_IMG_* */
#include "image_red_remap.h"
#include "moon_interaction.h"
#include "app_config.h"
#include "display_defs.h"
#include "tasks.h"                     /* psram_task_ensure, psram_task_spawn */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>

static const char *TAG = "image_page";

extern const lv_font_t lv_font_overpass_27;

/* The four instances. Identity fields are filled by image_page_init(). */
static image_page_t s_pages[IMG_SRC_COUNT];

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
    lv_obj_set_size(page_container, SCREEN_SIZE, SCREEN_SIZE);
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
    lv_obj_set_size(p->overlay_bar, SCREEN_SIZE, 68);
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

    /* Custom Image URL error overlay. A failed fetch stores a reason string in
     * frame.error_msg and leaves no fresh frame (stamp_ms not advanced), so the
     * new-image gate below would return early and the page would show a blank or
     * stale panel with no explanation. Read error_msg under frame_mux (like every
     * other frame read here), and when a reason is present render it as the
     * caption text and bail. A successful fetch clears error_msg, so the normal
     * swap path below runs and overwrites the caption — no stale error. Scoped to
     * Custom, so GOES/Solar/Moon are visually unchanged. */
    if (p->src == IMG_SRC_CUSTOM && p->frame.error_msg[0] != '\0') {
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

    if (!p->frame.buf || (!forced && p->frame.stamp_ms <= p->displayed_stamp_ms)) {
        frame_unlock(p);
        return;
    }

    /* Copy the RGB565 source into a freshly allocated PSRAM buffer so we can
     * release frame_mux before running the (asynchronous) crossfade and so the
     * LVGL side owns its display buffers independently of the poller. */
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

    size_t   buf_size = (size_t)w * h * 2;
    uint8_t *copy = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!copy) {
        ESP_LOGE(TAG, "PSRAM alloc failed for crossfade buffer (%u bytes)",
                 (unsigned)buf_size);
        frame_unlock(p);
        /* New-image gate passed but the display copy failed — release any
         * manual-fetch wait overlay so it can't get stuck. */
        nina_wait_overlay_hide();
        return;
    }
    /* Copy the (optionally cropped) source row-by-row. Decode is top-down end to
     * end (the historical "SW decode is flipped" belief was stb's flip-on-load
     * flag reading uninitialized TLS garbage; see STBI_NO_THREAD_LOCALS in
     * stb_image.c), so src_y is simply oy + y. ox/oy apply the center-crop. */
    size_t src_stride = (size_t)sw * 2;
    size_t dst_stride = (size_t)w * 2;

    /* Optional caption mask (Solar bands only). The mask rect is in upright-full
     * image fractional coords; convert to a pixel rect [mx0..mx1, my0..my1]. For
     * masked pixels we composite a color-sampled blend: per row, sample one
     * source pixel just to the RIGHT of the mask (col mx1+PAD) at the same
     * upright row and stretch it across the masked span. This extends the
     * neighbouring background (black margin for HMI -> seamless; corona for
     * LASCO -> blends) horizontally over the burned-in timestamp. */
    bool  mask_on = false;
    int   mx0 = 0, mx1 = 0, my0 = 0, my1 = 0, sample_col = 0;
    /* Gate the caption mask on the same crop/clean toggle as the crop, so turning
     * the toggle off shows the raw frame (incl. burned-in timestamp). */
    if (image_page_config_crop(cfg, p->src) && p->src == IMG_SRC_SOLAR) {
        float fx0, fy0, fx1, fy1;
        if (solar_band_text_mask(cfg->solar_band, &fx0, &fy0, &fx1, &fy1)) {
            mask_on = true;
            mx0 = (int)(fx0 * sw);
            mx1 = (int)(fx1 * sw);
            my0 = (int)(fy0 * sh);
            my1 = (int)(fy1 * sh);
            const int PAD = 8;
            sample_col = mx1 + PAD;
            if (sample_col > sw - 1) sample_col = sw - 1;  /* clamp into bounds */
        }
    }

    for (uint32_t y = 0; y < h; y++) {
        uint32_t src_y = (uint32_t)oy + y;
        memcpy((uint8_t *)copy + (size_t)y * dst_stride,
               p->frame.buf + (size_t)src_y * src_stride + (size_t)ox * 2,
               dst_stride);

        if (!mask_on) continue;

        /* Upright-full row for this dst row. Mask rect is in upright coords. */
        int uY = (int)oy + (int)y;
        if (uY < my0 || uY >= my1) continue;  /* row outside the mask band */

        /* Sample the per-row replacement color ONCE (right of the mask, same
         * upright row). */
        const uint8_t *src_px = p->frame.buf
                                + (size_t)src_y * src_stride
                                + (size_t)sample_col * 2;
        uint8_t lo = src_px[0], hi = src_px[1];

        /* Overwrite the masked columns in this dst row. dst col x maps to
         * upright col (ox + x); paint where ox+x is within [mx0, mx1). */
        for (uint32_t x = 0; x < w; x++) {
            int uX = (int)ox + (int)x;
            if (uX < mx0 || uX >= mx1) continue;
            uint8_t *dst_px = (uint8_t *)copy + (size_t)y * dst_stride + (size_t)x * 2;
            dst_px[0] = lo;
            dst_px[1] = hi;
        }
    }
    int64_t stamp_ms = p->frame.stamp_ms;
    /* Capture the label this buffer was fetched for UNDER the same lock so the
     * on-screen text is coupled to frame.buf and cannot desync if a config
     * change lands between fetches (issue #166). */
    char label_copy[48];
    strlcpy(label_copy, p->frame.label, sizeof(label_copy));
    frame_unlock(p);

    /* Per-source logical mirror pass. Runs on the upright `copy` (after the
     * caption mask) and BEFORE the rotation block, so flips compose naturally
     * with rotation. vflip reverses row order (top<->bottom); hflip reverses the
     * pixel order within each row (left<->right). Moon is excluded — it has its
     * own moon_flip_u/v handling elsewhere. Dimensions are unchanged. */
    uint8_t do_vflip = (p->src == IMG_SRC_GOES)   ? cfg->goes_vflip
                       : (p->src == IMG_SRC_SOLAR) ? cfg->solar_vflip
                       : (p->src == IMG_SRC_CUSTOM) ? cfg->custom_vflip
                                                    : 0;
    uint8_t do_hflip = (p->src == IMG_SRC_GOES)   ? cfg->goes_hflip
                       : (p->src == IMG_SRC_SOLAR) ? cfg->solar_hflip
                       : (p->src == IMG_SRC_CUSTOM) ? cfg->custom_hflip
                                                    : 0;
    if (do_vflip) {
        /* Swap row y with row (h-1-y) using one PSRAM temp row of dst_stride. */
        uint8_t *tmp_row = heap_caps_malloc(dst_stride, MALLOC_CAP_SPIRAM);
        if (!tmp_row) {
            ESP_LOGE(TAG, "PSRAM alloc failed for vflip temp row (%u bytes)",
                     (unsigned)dst_stride);
            heap_caps_free(copy);
            /* frame_mux already released above; just retire the wait overlay. */
            nina_wait_overlay_hide();
            return;
        }
        for (uint32_t y = 0; y < h / 2; y++) {
            uint8_t *row_a = (uint8_t *)copy + (size_t)y * dst_stride;
            uint8_t *row_b = (uint8_t *)copy + (size_t)(h - 1 - y) * dst_stride;
            memcpy(tmp_row, row_a, dst_stride);
            memcpy(row_a, row_b, dst_stride);
            memcpy(row_b, tmp_row, dst_stride);
        }
        heap_caps_free(tmp_row);
    }
    if (do_hflip) {
        /* Reverse the w pixels in each row: swap col x with (w-1-x). */
        for (uint32_t y = 0; y < h; y++) {
            uint16_t *row = (uint16_t *)((uint8_t *)copy + (size_t)y * dst_stride);
            for (uint32_t x = 0; x < (uint32_t)w / 2; x++) {
                uint16_t t          = row[x];
                row[x]              = row[w - 1 - x];
                row[w - 1 - x]      = t;
            }
        }
    }

    /* Per-source logical rotation pass. Rotates the decoded RGB565 pixel buffer
     * (NOT via an LVGL transform) so it is independent of display rotation and
     * runs on the upright `copy` produced above (after the caption mask).
     * orient: 0/1/2/3 = 0/90/180/270 degrees CLOCKWISE. Moon never rotates.
     * 90/270 swap width<->height. */
    uint8_t orient = (p->src == IMG_SRC_GOES)   ? cfg->goes_orientation
                     : (p->src == IMG_SRC_SOLAR) ? cfg->solar_orientation
                     : (p->src == IMG_SRC_CUSTOM) ? cfg->custom_orientation
                                                  : 0;
    if (orient != 0) {
        uint16_t rw, rh;
        if (orient == 2) {            /* 180deg: same dims */
            rw = w;  rh = h;
        } else {                       /* 90/270: dims swap */
            rw = h;  rh = w;
        }
        size_t   rot_size = (size_t)rw * rh * 2;
        uint8_t *rot = heap_caps_malloc(rot_size, MALLOC_CAP_SPIRAM);
        if (!rot) {
            ESP_LOGE(TAG, "PSRAM alloc failed for rotation buffer (%u bytes)",
                     (unsigned)rot_size);
            heap_caps_free(copy);
            /* frame_mux already released above; just retire the wait overlay. */
            nina_wait_overlay_hide();
            return;
        }
        const uint16_t *src = (const uint16_t *)copy;
        uint16_t       *dst = (uint16_t *)rot;
        if (orient == 2) {
            /* rot[x,y] = copy[w-1-x, h-1-y] */
            for (uint32_t Y = 0; Y < rh; Y++) {
                for (uint32_t X = 0; X < rw; X++) {
                    uint32_t sx = (uint32_t)w - 1 - X;
                    uint32_t sy = (uint32_t)h - 1 - Y;
                    dst[(size_t)Y * rw + X] = src[(size_t)sy * w + sx];
                }
            }
        } else if (orient == 1) {
            /* 90deg CW: rot[X,Y] = copy[Y, h-1-X], dims rw=h, rh=w */
            for (uint32_t Y = 0; Y < rh; Y++) {           /* Y in [0,w) */
                for (uint32_t X = 0; X < rw; X++) {       /* X in [0,h) */
                    uint32_t sx = Y;
                    uint32_t sy = (uint32_t)h - 1 - X;
                    dst[(size_t)Y * rw + X] = src[(size_t)sy * w + sx];
                }
            }
        } else {  /* orient == 3 */
            /* 270deg CW: rot[X,Y] = copy[w-1-Y, X], dims rw=h, rh=w */
            for (uint32_t Y = 0; Y < rh; Y++) {           /* Y in [0,w) */
                for (uint32_t X = 0; X < rw; X++) {       /* X in [0,h) */
                    uint32_t sx = (uint32_t)w - 1 - Y;
                    uint32_t sy = X;
                    dst[(size_t)Y * rw + X] = src[(size_t)sy * w + sx];
                }
            }
        }
        heap_caps_free(copy);
        copy     = rot;
        w        = rw;
        h        = rh;
        buf_size = rot_size;
    }

    /* Red Night: recolour the decoded image to red shades (luma->red, in place).
     * Self-gates inside the helper (no-op for every non-red theme), so call it
     * unconditionally on the final upright/cropped/rotated `copy` before it is
     * handed to LVGL. w*h is the committed pixel count. */
    image_red_remap_rgb565((uint16_t *)copy, (size_t)w * h);

    /* Point the BACK slot's descriptor at the new copy. release_dsc() frees the
     * previous buffer unless it was borrowed (moon-drag), and clears that flag —
     * this owned copy is not borrowed. */
    bool back_is_a = !p->front_is_a;
    lv_image_dsc_t *back_dsc = p->front_is_a ? &p->dsc_b : &p->dsc_a;
    release_dsc(p, back_dsc, back_is_a);
    back_dsc->data          = copy;
    back_dsc->data_size     = buf_size;
    back_dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    back_dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    back_dsc->header.w      = w;
    back_dsc->header.h      = h;
    back_dsc->header.stride = w * 2;

    /* Scale the source to fill the panel width. 256 = 1.0x. Do NOT call
     * lv_obj_set_pos() on the image here: re-anchoring a scaled image every
     * update flips it upside down under 90/270 display rotation. The image
     * stays at its create-time (0,0); wide sectors are top-aligned. */
    uint16_t scale = (w > 0) ? (uint16_t)(((uint32_t)SCREEN_SIZE * 256 + w / 2) / w) : 256;
    lv_image_set_src(p->img_back, back_dsc);
    lv_image_set_scale(p->img_back, scale);
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
    uint16_t scale = (uint16_t)(((uint32_t)SCREEN_SIZE * 256 + w / 2) / w);
    lv_image_set_src(p->img_back, back_dsc);
    lv_image_set_scale(p->img_back, scale);
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

void image_page_show_borrowed(image_page_t *p, const uint16_t *buf, int w, int h)
{
    if (!atomic_load(&p->active)) return;
    if (!buf || !p->root || w <= 0 || h <= 0) return;
    /* A settle->resting crossfade may still be in flight when a new drag frame
     * arrives (rapid re-swipe right at the grace boundary). Rather than DROP this
     * frame (which froze the disc until the fade finished, then jumped — the
     * artifact), CANCEL the crossfade and finalize it so this fresh frame takes
     * over immediately. cancel_moon_crossfade() retires buffers exactly as
     * crossfade_done_cb would, so ownership stays consistent for the swap below. */
    if (p->crossfade_active) cancel_moon_crossfade(p);

    /* Point the BACK slot's descriptor straight at the caller's buffer — NO copy.
     * The caller (moon drag loop) ping-pongs two 720 PPA-output buffers and hands
     * us the one NOT currently on screen, so nothing the front image is still
     * flushing is ever overwritten. Flag the slot "borrowed" so release_dsc()
     * skips freeing it (the caller owns it and frees on page leave). Free any
     * PRIOR owned buffer this slot held first. */
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

    /* Full-panel buffer (720): display 1:1 (256 = 1.0x), no software scale. */
    uint16_t scale = (w > 0) ? (uint16_t)(((uint32_t)SCREEN_SIZE * 256 + w / 2) / w) : 256;
    lv_image_set_src(p->img_back, back_dsc);
    lv_image_set_scale(p->img_back, scale);
    lv_obj_set_style_opa(p->img_back, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p->img_back, LV_OBJ_FLAG_HIDDEN);

    /* Instant swap: retire the old front and flip designations, mirroring the
     * instant branch of image_page_render_frame(). release_dsc() frees the old
     * front only if this page owned it (skips a prior borrowed buffer). */
    lv_image_dsc_t *old_dsc = p->front_is_a ? &p->dsc_a : &p->dsc_b;
    lv_obj_add_flag(p->img_front, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(p->img_front, NULL);
    release_dsc(p, old_dsc, p->front_is_a);

    p->front_is_a = !p->front_is_a;
    p->img_front  = p->front_is_a ? lv_obj_get_child(p->root, 0)
                                  : lv_obj_get_child(p->root, 1);
    p->img_back   = p->front_is_a ? lv_obj_get_child(p->root, 1)
                                  : lv_obj_get_child(p->root, 0);

    /* Mark a frame as displayed (mirrors render_frame) and keep the moon caption
     * live during the drag. */
    p->displayed_stamp_ms = esp_timer_get_time() / 1000;
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
}

/* ─────────────────────────── instances & gates ─────────────────────────── */

static const char *const s_names[IMG_SRC_COUNT] = { "img_goes", "img_moon", "img_solar", "img_custom" };

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
        image_page_ensure_task_running(p);
        image_page_render_frame(p);   /* a retained or warm frame shows instantly; no-op when empty */
        image_page_wake(p);
        return;
    }
    /* Leaving: drop the LVGL copies now (we hold the display lock) and clear a
     * loading overlay a manual fetch may have left up. The decoded frame is
     * RETAINED (swipe-back re-shows it instantly; the resident cap is enforced
     * by image_page_evict_if_over_cap on later commits). Poke the poller so it
     * parks (Moon frees its texture in on_park unless it is still warm). */
    image_page_release_lvgl(p);
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
        default:             return true;
    }
}

bool image_page_config_crop(const app_config_t *c, image_src_t src)
{
    switch (src) {
        case IMG_SRC_GOES:   return c->goes_crop;
        case IMG_SRC_SOLAR:  return c->solar_crop;
        case IMG_SRC_CUSTOM: return c->custom_crop;
        default:             return false;   /* Moon never crops */
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
    }
}

void image_page_set_error(image_page_t *p, const char *msg)
{
    if (!p) return;
    if (frame_lock(p, 200)) {
        strlcpy(p->frame.error_msg, msg ? msg : "", sizeof(p->frame.error_msg));
        frame_unlock(p);
    }
    /* The Custom page shows the reason as its caption; render_frame() handles
     * the error branch before the newer-frame gate, so a plain call suffices. */
    if (p->src == IMG_SRC_CUSTOM && atomic_load(&p->active) && bsp_display_lock(LVGL_LOCK_TIMEOUT_MS)) {
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
        default:             return false;   /* Moon: local render, handled below */
    }
}

/* Per-source "re-render the cached frame locally" test: crop / rotation / flips. */
static bool render_params_changed(image_src_t s, const app_config_t *prev, const app_config_t *cur)
{
    if (image_page_config_crop(cur, s) != image_page_config_crop(prev, s)) return true;
    switch (s) {
        case IMG_SRC_GOES:   return cur->goes_orientation != prev->goes_orientation || cur->goes_vflip != prev->goes_vflip || cur->goes_hflip != prev->goes_hflip;
        case IMG_SRC_SOLAR:  return cur->solar_orientation != prev->solar_orientation || cur->solar_vflip != prev->solar_vflip || cur->solar_hflip != prev->solar_hflip;
        case IMG_SRC_CUSTOM: return cur->custom_orientation != prev->custom_orientation || cur->custom_vflip != prev->custom_vflip || cur->custom_hflip != prev->custom_hflip;
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

/* Apply image-page config changes live (preview) for all four pages, comparing
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
        if (!en_cur) continue;

        if (source_params_changed((image_src_t)s, prev, cur, force_fetch)) {
            image_page_request_manual_fetch(p);            /* overlay + fresh download */
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
