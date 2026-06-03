/**
 * @file nina_image_display.c
 * @brief Full-screen satellite image page with crossfade transitions.
 *
 * Displays a GOES/NESDIS sector image scaled to fill the 720x720 panel.
 * New images crossfade over the previous one (500 ms ease-in-out). A
 * toggle-able bottom overlay bar shows the region name and last-update time.
 *
 * Threading: all LVGL calls below run under the display lock held by the
 * CALLER (data_update_task / web handler), matching allsky_page_update and
 * nina_spotify_update. Do NOT take lvgl_port_lock / bsp_display_lock here.
 */

#include "nina_image_display.h"
#include "nina_wait_overlay.h"
#include "nina_dashboard_internal.h"
#include "moon_interaction.h"
#include "app_config.h"
#include "display_defs.h"
#include "tasks.h"          /* goes_task_handle, moon_anim_request */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>      /* atomic_store */
#include <string.h>
#include <time.h>

/* moon_anim_request is owned by another task and normally declared in tasks.h.
 * Provide a guarded fallback extern in case that declaration is not yet present
 * so this translation unit still compiles. */
#ifndef MOON_ANIM_REQUEST_DECLARED
extern _Atomic bool moon_anim_request;
#endif

static const char *TAG = "image_display";

/* Overlay label font — overpass_27 is unconditionally compiled into the
 * firmware (see main/CMakeLists.txt), so it avoids any dependency on the
 * LV_FONT_MONTSERRAT_* Kconfig toggles. */
extern const lv_font_t lv_font_overpass_27;

static lv_obj_t *page_container;
static lv_obj_t *img_front;
static lv_obj_t *img_back;
static lv_obj_t *overlay_bar;
static lv_obj_t *lbl_region;
static lv_obj_t *lbl_timestamp;

/* One persistent RGB565 descriptor per image slot. The slot whose image is
 * currently "front" owns the displayed buffer; the "back" slot owns the
 * incoming/copy buffer during a crossfade. */
static lv_image_dsc_t img_dsc_a;
static lv_image_dsc_t img_dsc_b;
static bool front_is_a = true;
static int64_t displayed_poll_ms;
static bool crossfade_active = false;
/* Set by nina_image_display_force_redraw() to make the NEXT update() re-render
 * the already-cached frame even though last_poll_ms hasn't advanced. Consumed
 * (cleared) on the next update() once the new-image gate has been bypassed.
 * Used for a crop/full-mode toggle so the page re-crops the cached image
 * locally instead of forcing an HTTP re-download. */
static bool force_redraw = false;

/* NESDIS sector code -> human-readable region name. */
static const struct { const char *code; const char *name; } region_labels[] = {
    {"umv","Upper Mississippi Valley"}, {"cgl","Great Lakes"},
    {"ne","Northeast"},                 {"se","Southeast"},
    {"smv","Southern Mississippi Valley"},{"sp","Southern Plains"},
    {"nr","Northern Rockies"},          {"sr","Southern Rockies"},
    {"pnw","Pacific Northwest"},        {"psw","Pacific Southwest"},
    {"pr","Puerto Rico"},               {"eus","U.S. Atlantic Coast"},
    {"cam","Central America"},          {"car","Caribbean"},
    {"mex","Mexico"},                   {"ga","Gulf of America"},
    {"na","Northern Atlantic"},         {"ssa","South America (South)"},
    {"eep","Eastern Pacific"},          {"taw","Tropical Atlantic"},
    {"nsa","South America (North)"},    {"can","Canada"},
    {NULL,NULL}
};

static const char *region_code_to_name(const char *code)
{
    for (int i = 0; region_labels[i].code; i++) {
        if (!strcmp(region_labels[i].code, code)) return region_labels[i].name;
    }
    return code;
}

static void init_image_dsc(lv_image_dsc_t *dsc)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf    = LV_COLOR_FORMAT_RGB565;
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* Runs when the fade-IN of the new image completes. Hides/clears the OLD
 * image (the one that just faded OUT, which is always img_front at callback
 * time), frees its buffer, flips the front/back designation, and recomputes
 * the img_front/img_back pointers from the container children. */
static void crossfade_done_cb(lv_anim_t *a)
{
    (void)a;
    /* img_front is the OLD image that just faded OUT. Its buffer is img_dsc_a
     * when front_is_a, else img_dsc_b. */
    lv_image_dsc_t *old_dsc = front_is_a ? &img_dsc_a : &img_dsc_b;

    /* Detach the source BEFORE freeing the buffer so LVGL never dereferences a
     * freed descriptor. */
    lv_obj_add_flag(img_front, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(img_front, NULL);
    if (old_dsc->data) {
        heap_caps_free((void *)old_dsc->data);
        old_dsc->data       = NULL;
        old_dsc->data_size  = 0;
        old_dsc->header.w   = 0;
        old_dsc->header.h   = 0;
    }

    front_is_a = !front_is_a;
    img_front  = front_is_a ? lv_obj_get_child(page_container, 0)
                            : lv_obj_get_child(page_container, 1);
    img_back   = front_is_a ? lv_obj_get_child(page_container, 1)
                            : lv_obj_get_child(page_container, 0);
    crossfade_active = false;
}

static void moon_tap_cb(lv_event_t *e)
{
    (void)e;
    /* Only the Moon source animates; ignore taps on the GOES image. */
    if (app_config_get()->image_display_source != 1) return;
    atomic_store(&moon_anim_request, true);
    if (goes_task_handle) xTaskNotifyGive(goes_task_handle);
}

/* Single-finger drag-to-rotate (Moon source only). PRESSED/PRESSING/RELEASED
 * feed moon_interaction's accumulated yaw/pitch; the render (poll) task reads
 * that state and rebuilds the sphere. Each handler early-returns on non-Moon
 * sources so GOES/Solar pages are unaffected. The render task is nudged on
 * move/release via goes_task_handle (same notify moon_tap_cb uses). */
static bool moon_indev_point(lv_point_t *p)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return false;
    lv_indev_get_point(indev, p);
    return true;
}

static void moon_drag_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (app_config_get()->image_display_source != 1) return;   /* Moon only */
    lv_point_t p;
    if (!moon_indev_point(&p)) return;
    moon_drag_begin((float)p.x, (float)p.y);
}

static void moon_drag_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (app_config_get()->image_display_source != 1) return;   /* Moon only */
    lv_point_t p;
    if (!moon_indev_point(&p)) return;
    moon_drag_move((float)p.x, (float)p.y);
    /* Re-render promptly so the rotation tracks the finger. */
    if (goes_task_handle) xTaskNotifyGive(goes_task_handle);
}

static void moon_drag_released_cb(lv_event_t *e)
{
    (void)e;
    if (app_config_get()->image_display_source != 1) return;   /* Moon only */
    moon_drag_end();
    if (goes_task_handle) xTaskNotifyGive(goes_task_handle);
}

lv_obj_t *nina_image_display_create(lv_obj_t *parent)
{
    page_container = lv_obj_create(parent);
    lv_obj_remove_style_all(page_container);
    lv_obj_set_size(page_container, SCREEN_SIZE, SCREEN_SIZE);
    /* Negate the parent main_cont's OUTER_PADDING so the image fills edge-to-edge
     * (matches nina_spotify spotify_page_create). Without this the page renders
     * inset by OUTER_PADDING, leaving a black band on the top and left. */
    lv_obj_set_pos(page_container, -OUTER_PADDING, -OUTER_PADDING);
    lv_obj_set_style_bg_color(page_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Tap (short click) on the page triggers the moon-cycle animation. Use
     * SHORT_CLICKED (not CLICKED) so it does not fire on the swipe gesture used
     * for page navigation. The handler is a no-op when the source is not Moon. */
    lv_obj_add_flag(page_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page_container, moon_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);

    /* Single-finger drag-to-rotate for the Moon source. These handlers no-op on
     * GOES/Solar sources (checked inside each cb). PRESSED snapshots the start
     * point, PRESSING accumulates yaw/pitch, RELEASED ends the drag. */
    lv_obj_add_event_cb(page_container, moon_drag_pressed_cb,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(page_container, moon_drag_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(page_container, moon_drag_released_cb, LV_EVENT_RELEASED, NULL);

    init_image_dsc(&img_dsc_a);
    init_image_dsc(&img_dsc_b);

    /* Child 0 = image A, child 1 = image B (the two stacked crossfade images);
     * overlay_bar (child 2) MUST be created after both images so the child-index
     * math in crossfade_done_cb / cleanup (children 0 and 1) stays valid.
     *
     * Each image uses pos + pivot (0,0) and no explicit object size, so the
     * source auto-sizes and lv_image_set_scale() (in _update) expands it from
     * the top-left to fill the panel — matching nina_spotify's album art. Do
     * NOT add LV_IMAGE_ALIGN_CENTER or a fixed 720x720 size: combined with the
     * scale transform they offset the image and leave a black border. */
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
     * This only affects the Image Display / Moon page objects, not main_cont or
     * other pages, and is independent of the LV_EVENT_GESTURE page-swipe on
     * scr_dashboard. */
    lv_obj_clear_flag(page_container, LV_OBJ_FLAG_SCROLLABLE |
                                      LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                                      LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_clear_flag(img_a, LV_OBJ_FLAG_SCROLLABLE |
                             LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                             LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_clear_flag(img_b, LV_OBJ_FLAG_SCROLLABLE |
                             LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                             LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    img_front = img_a;
    img_back  = img_b;
    front_is_a = true;
    displayed_poll_ms = 0;
    crossfade_active  = false;

    overlay_bar = lv_obj_create(page_container);
    lv_obj_remove_style_all(overlay_bar);
    lv_obj_set_size(overlay_bar, SCREEN_SIZE, 68);
    lv_obj_align(overlay_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_bar, 210, 0);
    lv_obj_set_style_pad_left(overlay_bar, 18, 0);
    lv_obj_set_style_pad_right(overlay_bar, 18, 0);
    lv_obj_clear_flag(overlay_bar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_region = lv_label_create(overlay_bar);
    lv_obj_set_style_text_color(lbl_region, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_region, &lv_font_overpass_27, 0);
    lv_obj_align(lbl_region, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(lbl_region, "");

    lbl_timestamp = lv_label_create(overlay_bar);
    lv_obj_set_style_text_color(lbl_timestamp, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_text_font(lbl_timestamp, &lv_font_overpass_27, 0);
    lv_obj_align(lbl_timestamp, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(lbl_timestamp, "");

    if (!app_config_get()->image_display_show_overlay) {
        lv_obj_add_flag(overlay_bar, LV_OBJ_FLAG_HIDDEN);
    }
    return page_container;
}

void nina_image_display_update(goes_data_t *data)
{
    if (!data || !page_container) return;
    if (crossfade_active) return;
    if (!goes_data_lock(data, 100)) return;

    /* Consume the force-redraw request: a crop/full toggle re-renders the cached
     * frame, so bypass the "new image" half of the gate this once. Still bail if
     * nothing is cached (!image_buf). The flag is read-and-cleared here under the
     * goes lock; the actual swap below is forced instant (no crossfade) since a
     * crossfade between two crops of the same frame is pointless. */
    bool forced = force_redraw;
    force_redraw = false;
    if (!data->image_buf || (!forced && data->last_poll_ms <= displayed_poll_ms)) {
        goes_data_unlock(data);
        return;
    }

    /* Copy the RGB565 source into a freshly allocated PSRAM buffer so we can
     * release the goes mutex before running the (asynchronous) crossfade and
     * so this page owns its display buffers independently of the poller. */
    uint16_t sw = data->image_w, sh = data->image_h;
    if (sw == 0 || sh == 0) {
        goes_data_unlock(data);
        /* New-image gate passed but the image is unusable — release any
         * manual-fetch wait overlay so it can't get stuck. */
        nina_wait_overlay_hide();
        return;
    }

    /* Optional center-crop: keep the central portion so the solar/satellite disc
     * zooms to fill the panel and the timestamp/label baked into the source
     * border is cropped off. Skipped for the Moon (source 1): it has no text
     * and is already framed to fill. The crop percentage is per-band for Solar
     * (source 2): 88% for AIA/EIT, 92% for HMI (trims to the disc edge), 100%
     * (no crop) for LASCO. Non-solar sources keep the historic 88% (e.g. GOES). */
    extern uint8_t solar_band_crop_pct(uint8_t idx);
    extern bool    solar_band_text_mask(uint8_t idx, float *x0, float *y0, float *x1, float *y1);
    const app_config_t *cfg = app_config_get();
    uint16_t w = sw, h = sh, ox = 0, oy = 0;
    uint8_t crop_pct = (cfg->image_display_source == 2)
                           ? solar_band_crop_pct(cfg->solar_band)
                           : 88;
    if (cfg->image_display_crop && cfg->image_display_source != 1 && crop_pct < 100) {
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
        goes_data_unlock(data);
        /* New-image gate passed but the display copy failed — release any
         * manual-fetch wait overlay so it can't get stuck. */
        nina_wait_overlay_hide();
        return;
    }
    /* Copy the (optionally cropped) source row-by-row. The software JPEG decoder
     * path sets vflip=true (its buffer is top<->bottom relative to the hardware
     * decode path); reverse the source row index in that case so the logical
     * buffer is upright. ox/oy apply the center-crop. */
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
    if (cfg->image_display_crop && cfg->image_display_source == 2) {
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
        uint32_t src_y = data->vflip ? (uint32_t)(sh - 1 - (oy + y)) : (uint32_t)(oy + y);
        memcpy((uint8_t *)copy + (size_t)y * dst_stride,
               data->image_buf + (size_t)src_y * src_stride + (size_t)ox * 2,
               dst_stride);

        if (!mask_on) continue;

        /* Upright-full row for this dst row. Mask rect is in upright coords. */
        int uY = (int)oy + (int)y;
        if (uY < my0 || uY >= my1) continue;  /* row outside the mask band */

        /* Sample the per-row replacement color ONCE (right of the mask, same
         * upright row, same vflip mapping the copy uses). */
        const uint8_t *src_px = data->image_buf
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
    int64_t poll_ms = data->last_poll_ms;
    goes_data_unlock(data);

    /* Point the BACK slot's descriptor at the new copy. */
    lv_image_dsc_t *back_dsc = front_is_a ? &img_dsc_b : &img_dsc_a;
    if (back_dsc->data) heap_caps_free((void *)back_dsc->data);
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
    lv_image_set_src(img_back, back_dsc);
    lv_image_set_scale(img_back, scale);
    lv_obj_set_style_opa(img_back, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(img_back, LV_OBJ_FLAG_HIDDEN);

    bool first_image = (displayed_poll_ms == 0);
    /* The Moon source re-posts a near-identical frame every ~60s; a crossfade
     * dips brightness at its midpoint (two ~50% copies over black) and reads as
     * a once-a-minute flicker. Swap instantly for the moon (and the first image).
     * A forced re-crop also swaps instantly: crossfading two crops of the same
     * frame is pointless and, since displayed_poll_ms == poll_ms here, would not
     * key as first_image. */
    bool instant = first_image || forced || (app_config_get()->image_display_source == 1);
    /* poll_ms == displayed_poll_ms on a forced re-crop (same cached frame), so
     * this assignment leaves displayed_poll_ms unchanged and a later real
     * download (higher last_poll_ms) still passes the new-image gate. */
    displayed_poll_ms = poll_ms;

    if (instant) {
        /* Show the new image at full opacity and retire the old one immediately.
         * Mirrors crossfade_done_cb so the previous buffer is freed, not leaked.
         * On the first image old_dsc->data is NULL, so the free is a no-op. */
        lv_obj_set_style_opa(img_back, LV_OPA_COVER, 0);
        lv_image_dsc_t *old_dsc = front_is_a ? &img_dsc_a : &img_dsc_b;
        lv_obj_add_flag(img_front, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(img_front, NULL);
        if (old_dsc->data) {
            heap_caps_free((void *)old_dsc->data);
            old_dsc->data      = NULL;
            old_dsc->data_size = 0;
            old_dsc->header.w  = 0;
            old_dsc->header.h  = 0;
        }
        front_is_a = !front_is_a;
        img_front  = front_is_a ? lv_obj_get_child(page_container, 0)
                                : lv_obj_get_child(page_container, 1);
        img_back   = front_is_a ? lv_obj_get_child(page_container, 1)
                                : lv_obj_get_child(page_container, 0);
    } else {
        crossfade_active = true;

        lv_anim_t a_in;
        lv_anim_init(&a_in);
        lv_anim_set_var(&a_in, img_back);
        lv_anim_set_values(&a_in, 0, 255);
        lv_anim_set_duration(&a_in, 500);
        lv_anim_set_path_cb(&a_in, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a_in, anim_opa_cb);
        lv_anim_set_completed_cb(&a_in, crossfade_done_cb);
        lv_anim_start(&a_in);

        lv_anim_t a_out;
        lv_anim_init(&a_out);
        lv_anim_set_var(&a_out, img_front);
        lv_anim_set_values(&a_out, 255, 0);
        lv_anim_set_duration(&a_out, 500);
        lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a_out, anim_opa_cb);
        lv_anim_start(&a_out);
    }

    /* cfg is already fetched above (crop check). */
    if (cfg->image_display_source == 1) {           /* Moon */
        extern void moon_caption(char *name_out, size_t name_sz,
                                 char *pct_out, size_t pct_sz);
        char name[24], pct[16];
        moon_caption(name, sizeof(name), pct, sizeof(pct));
        lv_label_set_text(lbl_region, name);
        lv_label_set_text(lbl_timestamp, pct);
    } else if (cfg->image_display_source == 2) {     /* Solar (SDO/AIA) */
        extern const char *solar_band_label(uint8_t idx);
        lv_label_set_text(lbl_region, solar_band_label(cfg->solar_band));
        time_t now; struct tm ti; time(&now); localtime_r(&now, &ti);
        char ts[32]; strftime(ts, sizeof(ts), "Updated %H:%M", &ti);
        lv_label_set_text(lbl_timestamp, ts);
    } else {                                         /* GOES */
        lv_label_set_text(lbl_region, region_code_to_name(cfg->goes_region));
        time_t now; struct tm ti; time(&now); localtime_r(&now, &ti);
        char ts[32]; strftime(ts, sizeof(ts), "Updated %H:%M", &ti);
        lv_label_set_text(lbl_timestamp, ts);
    }

    /* The new image is now committed (swap done, overlay labels updated). Hide
     * the wait overlay if it was shown for a manual source/band change. This is
     * a no-op when the overlay is already hidden. We hold the display lock (the
     * caller does), so call directly — matching this module's convention. */
    nina_wait_overlay_hide();
}

void nina_image_display_force_redraw(void)
{
    /* Request that the next nina_image_display_update() re-render the cached
     * frame even though last_poll_ms has not advanced. The flag is a plain bool
     * touched only here (httpd task, display lock held by caller) and in
     * update() under the goes lock; the caller wraps both this and the following
     * update() in bsp_display_lock, so no extra synchronization is needed. */
    force_redraw = true;
}

bool nina_image_display_has_image(void)
{
    /* True once a frame has been committed to the page; reset to 0 by
     * nina_image_display_cleanup() on page leave and at create. Read under the
     * display lock held by the caller, matching this module's convention. */
    return displayed_poll_ms != 0;
}

void nina_image_display_cleanup(void)
{
    if (!page_container) return;

    crossfade_active = false;
    lv_anim_delete(img_front, anim_opa_cb);
    lv_anim_delete(img_back, anim_opa_cb);

    lv_obj_t *child0 = lv_obj_get_child(page_container, 0);
    lv_obj_t *child1 = lv_obj_get_child(page_container, 1);
    lv_image_set_src(child0, NULL);
    lv_image_set_src(child1, NULL);
    lv_obj_add_flag(child0, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(child1, LV_OBJ_FLAG_HIDDEN);

    if (img_dsc_a.data) { heap_caps_free((void *)img_dsc_a.data); img_dsc_a.data = NULL; }
    if (img_dsc_b.data) { heap_caps_free((void *)img_dsc_b.data); img_dsc_b.data = NULL; }
    img_dsc_a.data_size = 0;
    img_dsc_b.data_size = 0;
    img_dsc_a.header.w = img_dsc_a.header.h = 0;
    img_dsc_b.header.w = img_dsc_b.header.h = 0;

    displayed_poll_ms = 0;
    front_is_a = true;
    img_front  = child0;
    img_back   = child1;
}

void nina_image_display_set_overlay_visible(bool visible)
{
    if (!overlay_bar) return;
    if (visible) lv_obj_clear_flag(overlay_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(overlay_bar, LV_OBJ_FLAG_HIDDEN);
}

void nina_image_display_apply_theme(void)
{
    if (!overlay_bar || !current_theme) return;
    int gb = app_config_get()->color_brightness;
    lv_obj_set_style_text_color(
        lbl_region,
        lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
}
