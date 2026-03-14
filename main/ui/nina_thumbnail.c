/**
 * @file nina_thumbnail.c
 * @brief Thumbnail overlay with zoom support for NINA dashboard.
 *
 * Uses PPA hardware accelerator for pre-scaling on zoom changes,
 * so panning at zoomed levels is a fast 1:1 blit with no per-frame transform.
 */

#include "nina_thumbnail.h"
#include "nina_dashboard_internal.h"
#include "jpeg_utils.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "thumbnail";

/* Zoom levels */
#define ZOOM_FIT  0
#define ZOOM_100  1
#define ZOOM_200  2

/* Double-tap threshold in milliseconds */
#define DOUBLE_TAP_MS 350

/* Thumbnail overlay state — shared via nina_dashboard_internal.h */
lv_obj_t *thumbnail_overlay = NULL;
static lv_obj_t *img_container = NULL;
static lv_obj_t *thumbnail_img = NULL;
static lv_obj_t *thumbnail_loading_lbl = NULL;
static lv_obj_t *zoom_badge = NULL;
static lv_obj_t *zoom_badge_lbl = NULL;
static lv_obj_t *btn_back = NULL;
static lv_obj_t *btn_back_lbl = NULL;
static lv_image_dsc_t thumbnail_dsc;
static uint8_t *thumbnail_original_buf = NULL;   /* Original decoded image */
static uint8_t *thumbnail_scaled_buf = NULL;     /* PPA-scaled for current zoom */
static int scaled_zoom_level = -1;               /* Zoom level of scaled_buf */
static volatile bool thumbnail_requested = false;

/* Zoom state */
static int current_zoom = ZOOM_FIT;
static uint32_t fit_scale = 256;
static uint32_t orig_w = 0;
static uint32_t orig_h = 0;
static uint32_t last_click_time = 0;

/* ------------------------------------------------------------------ */
/*  Zoom logic                                                        */
/* ------------------------------------------------------------------ */

static void update_image_descriptor(uint8_t *buf, uint32_t w, uint32_t h, size_t size) {
    memset(&thumbnail_dsc, 0, sizeof(thumbnail_dsc));
    thumbnail_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    thumbnail_dsc.header.w = w;
    thumbnail_dsc.header.h = h;
    thumbnail_dsc.header.stride = w * 2;
    thumbnail_dsc.data = buf;
    thumbnail_dsc.data_size = size;
    lv_image_set_src(thumbnail_img, &thumbnail_dsc);
}

static void apply_zoom(void) {
    if (!thumbnail_img || !img_container || !thumbnail_original_buf) return;

    uint32_t scale;
    switch (current_zoom) {
        case ZOOM_100: scale = 256; break;
        case ZOOM_200: scale = 512; break;
        default:       scale = fit_scale; break;
    }

    uint32_t disp_w = orig_w * scale / 256;
    uint32_t disp_h = orig_h * scale / 256;
    if (scale == 256) {
        /* 1:1 — use original buffer directly, no scaling needed */
        if (thumbnail_scaled_buf) {
            free(thumbnail_scaled_buf);
            thumbnail_scaled_buf = NULL;
            scaled_zoom_level = -1;
        }
        update_image_descriptor(thumbnail_original_buf, orig_w, orig_h,
                                orig_w * orig_h * 2);
        lv_image_set_scale(thumbnail_img, 256);
    } else if (scaled_zoom_level == current_zoom && thumbnail_scaled_buf) {
        /* Already have the correct PPA-scaled buffer — just reuse it */
        lv_image_set_scale(thumbnail_img, 256);
    } else {
        /* PPA pre-scale to target dimensions for smooth panning */
        size_t ppa_size = 0;
        uint8_t *scaled = ppa_scale_rgb565(thumbnail_original_buf,
                                            orig_w, orig_h,
                                            disp_w, disp_h, &ppa_size);
        if (scaled) {
            if (thumbnail_scaled_buf) free(thumbnail_scaled_buf);
            thumbnail_scaled_buf = scaled;
            scaled_zoom_level = current_zoom;
            update_image_descriptor(scaled, disp_w, disp_h, ppa_size);
            lv_image_set_scale(thumbnail_img, 256);
            ESP_LOGI(TAG, "PPA zoom %dx: %lux%lu -> %lux%lu",
                     (scale == 512) ? 2 : 1,
                     (unsigned long)orig_w, (unsigned long)orig_h,
                     (unsigned long)disp_w, (unsigned long)disp_h);
        } else {
            /* PPA failed — fall back to LVGL software scaling */
            ESP_LOGW(TAG, "PPA scale failed, using SW fallback");
            update_image_descriptor(thumbnail_original_buf, orig_w, orig_h,
                                    orig_w * orig_h * 2);
            lv_image_set_scale(thumbnail_img, scale);
        }
    }

    /* Layout: set widget size to display dimensions for scroll calculations.
     * When PPA-scaled, the buffer IS disp_w x disp_h at scale 256.
     * When SW-fallback, LVGL scales orig_w x orig_h by scale factor. */
    lv_obj_set_size(thumbnail_img, (int32_t)disp_w, (int32_t)disp_h);

    if (disp_w > SCREEN_SIZE || disp_h > SCREEN_SIZE) {
        lv_obj_set_style_align(thumbnail_img, LV_ALIGN_TOP_LEFT, 0);
        lv_obj_add_flag(img_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(img_container, LV_DIR_ALL);
        lv_obj_scroll_to(img_container,
                         (disp_w > SCREEN_SIZE) ? (int32_t)(disp_w - SCREEN_SIZE) / 2 : 0,
                         (disp_h > SCREEN_SIZE) ? (int32_t)(disp_h - SCREEN_SIZE) / 2 : 0,
                         LV_ANIM_OFF);
    } else {
        lv_obj_clear_flag(img_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_scroll_to(img_container, 0, 0, LV_ANIM_OFF);
        lv_obj_set_size(thumbnail_img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_center(thumbnail_img);
    }

    /* Update zoom badge */
    if (zoom_badge) {
        if (current_zoom == ZOOM_FIT) {
            lv_obj_add_flag(zoom_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            const char *txt = (current_zoom == ZOOM_100) ? "1:1" : "2x";
            lv_label_set_text(zoom_badge_lbl, txt);
            lv_obj_clear_flag(zoom_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Double-tap on image container to cycle zoom */
static void img_container_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    uint32_t now = lv_tick_get();
    if (last_click_time != 0 && (now - last_click_time) < DOUBLE_TAP_MS) {
        /* Double-tap detected — cycle zoom */
        current_zoom++;
        if (current_zoom > ZOOM_200) current_zoom = ZOOM_FIT;
        apply_zoom();
        last_click_time = 0; /* reset so next single tap starts fresh */
    } else {
        last_click_time = now;
    }
}

/* Back button dismisses overlay */
static void back_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_dashboard_hide_thumbnail();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void nina_thumbnail_create(lv_obj_t *parent) {
    /* --- Fullscreen overlay --- */
    thumbnail_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(thumbnail_overlay);
    lv_obj_set_size(thumbnail_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(thumbnail_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(thumbnail_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(thumbnail_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(thumbnail_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(thumbnail_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* --- Image container (child of overlay, fills 720x720) --- */
    img_container = lv_obj_create(thumbnail_overlay);
    lv_obj_remove_style_all(img_container);
    lv_obj_set_size(img_container, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_opa(img_container, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(img_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(img_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(img_container, img_container_click_cb, LV_EVENT_SHORT_CLICKED, NULL);

    /* --- Thumbnail image (child of img_container, hidden initially) --- */
    thumbnail_img = lv_image_create(img_container);
    lv_obj_add_flag(thumbnail_img, LV_OBJ_FLAG_HIDDEN);

    /* --- Loading label (floating child of overlay, centered) --- */
    thumbnail_loading_lbl = lv_label_create(thumbnail_overlay);
    lv_obj_add_flag(thumbnail_loading_lbl, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_color(thumbnail_loading_lbl,
        lv_color_hex(current_theme ? current_theme->text_color : 0xFFFFFF), 0);
    lv_obj_set_style_text_font(thumbnail_loading_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(thumbnail_loading_lbl, "Loading image...");
    lv_obj_center(thumbnail_loading_lbl);

    /* --- Zoom badge (floating child of overlay, top-left) --- */
    zoom_badge = lv_obj_create(thumbnail_overlay);
    lv_obj_remove_style_all(zoom_badge);
    lv_obj_add_flag(zoom_badge, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(zoom_badge, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(zoom_badge, LV_OPA_70, 0);
    lv_obj_set_style_radius(zoom_badge, 8, 0);
    lv_obj_set_style_pad_hor(zoom_badge, 6, 0);
    lv_obj_set_style_pad_ver(zoom_badge, 3, 0);
    lv_obj_set_size(zoom_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(zoom_badge, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_add_flag(zoom_badge, LV_OBJ_FLAG_HIDDEN);

    zoom_badge_lbl = lv_label_create(zoom_badge);
    lv_obj_set_style_text_font(zoom_badge_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(zoom_badge_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(zoom_badge_lbl, "");

    /* --- Back button (floating child of overlay, bottom-right) --- */
    btn_back = lv_button_create(thumbnail_overlay);
    lv_obj_set_size(btn_back, 84, 108);
    lv_obj_set_style_radius(btn_back, 14, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme ? current_theme->bento_border : 0x333333), 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme ? current_theme->progress_color : 0x4FC3F7), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_add_flag(btn_back, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_RIGHT, -16, -16);

    btn_back_lbl = lv_label_create(btn_back);
    lv_label_set_text(btn_back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(btn_back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(btn_back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btn_back_lbl);

    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    memset(&thumbnail_dsc, 0, sizeof(thumbnail_dsc));
}

bool nina_dashboard_thumbnail_requested(void) {
    return thumbnail_requested;
}

void nina_dashboard_clear_thumbnail_request(void) {
    thumbnail_requested = false;
}

void nina_dashboard_set_thumbnail(const uint8_t *rgb565_data, uint32_t w, uint32_t h, uint32_t data_size) {
    if (!thumbnail_overlay || !thumbnail_img) return;

    /* If overlay was dismissed while fetch was in progress, discard the data */
    if (lv_obj_has_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN)) {
        free((void *)rgb565_data);
        return;
    }

    /* Free previous buffers */
    if (thumbnail_scaled_buf) {
        free(thumbnail_scaled_buf);
        thumbnail_scaled_buf = NULL;
        scaled_zoom_level = -1;
    }
    if (thumbnail_original_buf) {
        free(thumbnail_original_buf);
        thumbnail_original_buf = NULL;
    }

    /* Take ownership of the buffer */
    thumbnail_original_buf = (uint8_t *)rgb565_data;

    /* Set up LVGL image descriptor with original buffer */
    update_image_descriptor(thumbnail_original_buf, w, h, data_size);

    /* Store original dimensions and compute fit scale */
    orig_w = w;
    orig_h = h;
    if (w > 0) {
        fit_scale = (uint32_t)SCREEN_SIZE * 256 / w;
    } else {
        fit_scale = 256;
    }

    /* Reset to fit view and apply */
    current_zoom = ZOOM_FIT;
    apply_zoom();

    /* Hide loading, show image */
    if (thumbnail_loading_lbl) {
        lv_obj_add_flag(thumbnail_loading_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(thumbnail_img, LV_OBJ_FLAG_HIDDEN);
}

void nina_dashboard_hide_thumbnail(void) {
    if (thumbnail_overlay) {
        lv_obj_add_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (thumbnail_img) {
        lv_obj_add_flag(thumbnail_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(thumbnail_img, NULL);
    }
    if (thumbnail_scaled_buf) {
        free(thumbnail_scaled_buf);
        thumbnail_scaled_buf = NULL;
        scaled_zoom_level = -1;
    }
    if (thumbnail_original_buf) {
        free(thumbnail_original_buf);
        thumbnail_original_buf = NULL;
    }
    thumbnail_requested = false;

    /* Reset zoom state */
    current_zoom = ZOOM_FIT;
    orig_w = 0;
    orig_h = 0;
}

bool nina_dashboard_thumbnail_visible(void) {
    return thumbnail_overlay && !lv_obj_has_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* Called from nina_dashboard.c when the header box is clicked */
void nina_thumbnail_request(void) {
    if (!thumbnail_overlay) return;

    lv_obj_clear_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN);
    if (thumbnail_loading_lbl) {
        lv_obj_clear_flag(thumbnail_loading_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (thumbnail_img) {
        lv_obj_add_flag(thumbnail_img, LV_OBJ_FLAG_HIDDEN);
    }

    /* Reset zoom to fit on new request */
    current_zoom = ZOOM_FIT;
    last_click_time = 0;

    thumbnail_requested = true;
}

void nina_thumbnail_apply_theme(void) {
    if (btn_back) {
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme ? current_theme->bento_border : 0x333333), 0);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(current_theme ? current_theme->progress_color : 0x4FC3F7), LV_STATE_PRESSED);
    }
    if (btn_back_lbl) {
        lv_obj_set_style_text_color(btn_back_lbl, lv_color_hex(0xFFFFFF), 0);
    }
    if (thumbnail_loading_lbl) {
        lv_obj_set_style_text_color(thumbnail_loading_lbl,
            lv_color_hex(current_theme ? current_theme->text_color : 0xFFFFFF), 0);
    }
    if (zoom_badge) {
        lv_obj_set_style_bg_color(zoom_badge, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(zoom_badge, LV_OPA_70, 0);
    }
    if (zoom_badge_lbl) {
        lv_obj_set_style_text_color(zoom_badge_lbl, lv_color_hex(0xFFFFFF), 0);
    }
}
