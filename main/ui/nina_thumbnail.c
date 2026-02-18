/**
 * @file nina_thumbnail.c
 * @brief Thumbnail overlay creation and management for NINA dashboard.
 */

#include "nina_thumbnail.h"
#include "nina_dashboard_internal.h"
#include <string.h>
#include <stdlib.h>

/* Thumbnail overlay state — shared via nina_dashboard_internal.h */
lv_obj_t *thumbnail_overlay = NULL;
static lv_obj_t *thumbnail_img = NULL;
static lv_obj_t *thumbnail_loading_lbl = NULL;
static lv_image_dsc_t thumbnail_dsc;
static uint8_t *thumbnail_decoded_buf = NULL;
static volatile bool thumbnail_requested = false;

/* Click overlay to dismiss */
static void thumbnail_overlay_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_dashboard_hide_thumbnail();
}

void nina_thumbnail_create(lv_obj_t *parent) {
    thumbnail_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(thumbnail_overlay);
    lv_obj_set_size(thumbnail_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(thumbnail_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(thumbnail_overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(thumbnail_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(thumbnail_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(thumbnail_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(thumbnail_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(thumbnail_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(thumbnail_overlay, thumbnail_overlay_click_cb, LV_EVENT_CLICKED, NULL);

    // Loading label
    thumbnail_loading_lbl = lv_label_create(thumbnail_overlay);
    lv_obj_set_style_text_color(thumbnail_loading_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(thumbnail_loading_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(thumbnail_loading_lbl, "Loading image...");

    // Image widget (hidden until data arrives)
    thumbnail_img = lv_image_create(thumbnail_overlay);
    lv_obj_add_flag(thumbnail_img, LV_OBJ_FLAG_HIDDEN);

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

    // If overlay was dismissed while fetch was in progress, discard the data
    if (lv_obj_has_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN)) {
        free((void *)rgb565_data);
        return;
    }

    // Free previous buffer
    if (thumbnail_decoded_buf) {
        free(thumbnail_decoded_buf);
        thumbnail_decoded_buf = NULL;
    }

    // Take ownership of the buffer
    thumbnail_decoded_buf = (uint8_t *)rgb565_data;

    // Set up LVGL image descriptor
    memset(&thumbnail_dsc, 0, sizeof(thumbnail_dsc));
    thumbnail_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    thumbnail_dsc.header.w = w;
    thumbnail_dsc.header.h = h;
    thumbnail_dsc.header.stride = w * 2;
    thumbnail_dsc.data = thumbnail_decoded_buf;
    thumbnail_dsc.data_size = data_size;

    lv_image_set_src(thumbnail_img, &thumbnail_dsc);

    // Scale image to fit screen width while preserving aspect ratio
    if (w > 0) {
        uint32_t scale = (uint32_t)SCREEN_SIZE * 256 / w;
        lv_image_set_scale(thumbnail_img, scale);
    }

    // Hide loading, show image
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
    if (thumbnail_decoded_buf) {
        free(thumbnail_decoded_buf);
        thumbnail_decoded_buf = NULL;
    }
    thumbnail_requested = false;
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
    thumbnail_requested = true;
}
