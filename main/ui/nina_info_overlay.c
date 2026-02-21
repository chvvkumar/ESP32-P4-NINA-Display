/**
 * @file nina_info_overlay.c
 * @brief Shared info overlay framework — base container, helpers, dispatch.
 *
 * Creates a single fullscreen overlay with title bar, content area, back button,
 * and loading label. Content is cleared and rebuilt when show() switches type.
 * Per-type content builders live in separate files (nina_info_camera.c, etc.).
 */

#include "nina_info_internal.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include <stdio.h>
#include <string.h>

/* ── Shared widget state ─────────────────────────────────────────── */
lv_obj_t *info_overlay       = NULL;
lv_obj_t *info_title_bar     = NULL;
lv_obj_t *info_lbl_title     = NULL;
lv_obj_t *info_lbl_instance  = NULL;
lv_obj_t *info_content       = NULL;
lv_obj_t *info_loading_lbl   = NULL;
lv_obj_t *info_btn_back      = NULL;
lv_obj_t *info_btn_back_lbl  = NULL;

volatile bool info_requested          = false;
info_overlay_type_t info_current_type = INFO_OVERLAY_CAMERA;
int info_return_page                  = 0;

/* Track which content type is currently built (-1 = none) */
static int built_content_type = -1;

/* ── Theme helpers ───────────────────────────────────────────────── */

bool info_is_red_night(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

uint32_t info_get_text_color(int gb) {
    if (info_is_red_night()) {
        return app_config_apply_brightness(current_theme->text_color, gb);
    }
    return app_config_apply_brightness(0xffffff, gb);
}

/* ── Widget factories ────────────────────────────────────────────── */

lv_obj_t *make_info_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_bento_box, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, INFO_CARD_PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, INFO_CARD_ROW_GAP, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *make_info_section(lv_obj_t *parent, const char *title) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
    return lbl;
}

lv_obj_t *make_info_kv(lv_obj_t *parent, const char *key, lv_obj_t **out_val) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_key = lv_label_create(row);
    lv_label_set_text(lbl_key, key);
    lv_obj_set_style_text_font(lbl_key, &lv_font_montserrat_16, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_key,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_18, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl_val,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    *out_val = lbl_val;
    return row;
}

lv_obj_t *make_info_kv_wide(lv_obj_t *parent, const char *key, lv_obj_t **out_val) {
    lv_obj_t *row = make_info_kv(parent, key, out_val);
    /* Override value font to 24 for emphasis */
    lv_obj_set_style_text_font(*out_val, &lv_font_montserrat_24, 0);
    return row;
}

/* ── Back button callback ────────────────────────────────────────── */

static void info_back_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_hide();
    nina_dashboard_show_page(info_return_page, 0);
}

/* ── Title text for each overlay type ────────────────────────────── */

static const char *overlay_titles[] = {
    "CAMERA & ENVIRONMENT",
    "MOUNT POSITION",
    "IMAGE STATISTICS",
    "SEQUENCE DETAILS",
    "FILTER WHEEL",
    "AUTOFOCUS CURVE",
};

/* ── Create overlay (once) ───────────────────────────────────────── */

void nina_info_overlay_create(lv_obj_t *parent) {
    int gb = app_config_get()->color_brightness;

    /* Base container */
    info_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(info_overlay);
    lv_obj_set_size(info_overlay, SCREEN_SIZE, SCREEN_SIZE);
    if (current_theme) {
        lv_obj_set_style_bg_color(info_overlay, lv_color_hex(current_theme->bg_main), 0);
    } else {
        lv_obj_set_style_bg_color(info_overlay, lv_color_hex(0x000000), 0);
    }
    lv_obj_set_style_bg_opa(info_overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(info_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(info_overlay, INFO_OUTER_PAD, 0);
    lv_obj_set_style_pad_row(info_overlay, 10, 0);
    lv_obj_add_flag(info_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(info_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(info_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* ── Title bar ── */
    info_title_bar = lv_obj_create(info_overlay);
    lv_obj_remove_style_all(info_title_bar);
    lv_obj_add_style(info_title_bar, &style_header_gradient, 0);
    lv_obj_set_width(info_title_bar, LV_PCT(100));
    lv_obj_set_height(info_title_bar, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(info_title_bar, 14, 0);
    lv_obj_set_flex_flow(info_title_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_title_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(info_title_bar, LV_OBJ_FLAG_SCROLLABLE);

    info_lbl_title = lv_label_create(info_title_bar);
    lv_obj_set_style_text_font(info_lbl_title, &lv_font_montserrat_26, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(info_lbl_title,
            lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    }
    lv_label_set_text(info_lbl_title, "INFO");

    info_lbl_instance = lv_label_create(info_title_bar);
    lv_obj_set_style_text_font(info_lbl_instance, &lv_font_montserrat_16, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(info_lbl_instance,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
    lv_label_set_text(info_lbl_instance, "");

    /* ── Content area ── */
    info_content = lv_obj_create(info_overlay);
    lv_obj_remove_style_all(info_content);
    lv_obj_set_width(info_content, LV_PCT(100));
    lv_obj_set_flex_grow(info_content, 1);
    lv_obj_set_flex_flow(info_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info_content, 10, 0);
    lv_obj_set_style_pad_bottom(info_content, INFO_BACK_BTN_H + INFO_OUTER_PAD, 0);
    lv_obj_clear_flag(info_content, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Loading label (floating, centered in content) ── */
    info_loading_lbl = lv_label_create(info_content);
    lv_obj_set_style_text_font(info_loading_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(info_loading_lbl, lv_color_hex(info_get_text_color(gb)), 0);
    lv_label_set_text(info_loading_lbl, "Loading...");
    lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_FLOATING);
    lv_obj_center(info_loading_lbl);

    /* ── Back button (floating, bottom-right) ── */
    info_btn_back = lv_button_create(info_overlay);
    lv_obj_set_size(info_btn_back, INFO_BACK_BTN_W, INFO_BACK_BTN_H);
    lv_obj_set_style_radius(info_btn_back, 14, 0);
    lv_obj_set_style_bg_opa(info_btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(info_btn_back,
        lv_color_hex(current_theme ? current_theme->bento_border : 0x333333), 0);
    lv_obj_set_style_bg_color(info_btn_back,
        lv_color_hex(current_theme ? current_theme->progress_color : 0x4FC3F7), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(info_btn_back, 0, 0);
    lv_obj_set_style_shadow_width(info_btn_back, 0, 0);
    lv_obj_add_flag(info_btn_back, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(info_btn_back, LV_ALIGN_BOTTOM_RIGHT, -INFO_OUTER_PAD, -INFO_OUTER_PAD);

    info_btn_back_lbl = lv_label_create(info_btn_back);
    lv_label_set_text(info_btn_back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(info_btn_back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(info_btn_back_lbl, lv_color_hex(info_get_text_color(gb)), 0);
    lv_obj_center(info_btn_back_lbl);

    lv_obj_add_event_cb(info_btn_back, info_back_btn_cb, LV_EVENT_CLICKED, NULL);

    built_content_type = -1;
}

/* ── Show ─────────────────────────────────────────────────────────── */

void nina_info_overlay_show(info_overlay_type_t type, int page_index) {
    if (!info_overlay) return;

    info_current_type = type;
    info_return_page  = page_index;

    /* Update title text */
    if (info_lbl_title && (int)type < (int)(sizeof(overlay_titles) / sizeof(overlay_titles[0]))) {
        lv_label_set_text(info_lbl_title, overlay_titles[type]);
    }

    /* Update instance hostname in title bar */
    if (info_lbl_instance) {
        int inst_idx = page_index - 1;  /* page 1..N -> instance 0..N-1 */
        if (inst_idx >= 0 && inst_idx < MAX_NINA_INSTANCES) {
            const char *url = app_config_get_instance_url(inst_idx);
            char host[64] = {0};
            extract_host_from_url(url, host, sizeof(host));
            lv_label_set_text(info_lbl_instance, host);
        } else {
            lv_label_set_text(info_lbl_instance, "");
        }
    }

    /* Clear content and rebuild for this type */
    if (built_content_type != (int)type) {
        lv_obj_clean(info_content);

        /* Re-create loading label since clean() destroyed it */
        int gb = app_config_get()->color_brightness;
        info_loading_lbl = lv_label_create(info_content);
        lv_obj_set_style_text_font(info_loading_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(info_loading_lbl,
            lv_color_hex(info_get_text_color(gb)), 0);
        lv_label_set_text(info_loading_lbl, "Loading...");
        lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_FLOATING);
        lv_obj_center(info_loading_lbl);

        /* Build type-specific content */
        switch (type) {
        case INFO_OVERLAY_CAMERA:    build_camera_content(info_content);    break;
        case INFO_OVERLAY_MOUNT:     build_mount_content(info_content);     break;
        case INFO_OVERLAY_IMAGESTATS: build_imagestats_content(info_content); break;
        case INFO_OVERLAY_SEQUENCE:  build_sequence_content(info_content);  break;
        case INFO_OVERLAY_FILTER:    build_filter_content(info_content);    break;
        case INFO_OVERLAY_AUTOFOCUS: build_autofocus_content(info_content); break;
        }

        /* Re-apply bottom padding — some builders call lv_obj_remove_style_all()
         * which strips the padding set in create(). */
        lv_obj_set_style_pad_bottom(info_content, INFO_BACK_BTN_H + INFO_OUTER_PAD, 0);

        built_content_type = (int)type;
    }

    /* Show loading label */
    if (info_loading_lbl) {
        lv_obj_clear_flag(info_loading_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* Show overlay */
    lv_obj_clear_flag(info_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Ensure back button is on top */
    if (info_btn_back) lv_obj_move_foreground(info_btn_back);

    /* Request data fetch */
    info_requested = true;
}

/* ── Hide ─────────────────────────────────────────────────────────── */

void nina_info_overlay_hide(void) {
    if (info_overlay) {
        lv_obj_add_flag(info_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    info_requested = false;
}

/* ── Query functions ─────────────────────────────────────────────── */

bool nina_info_overlay_visible(void) {
    return info_overlay && !lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool nina_info_overlay_requested(void) {
    return info_requested;
}

void nina_info_overlay_clear_request(void) {
    info_requested = false;
}

info_overlay_type_t nina_info_overlay_get_type(void) {
    return info_current_type;
}

int nina_info_overlay_get_return_page(void) {
    return info_return_page;
}

/* ── Data population dispatch ────────────────────────────────────── */

void nina_info_overlay_set_camera_data(const camera_detail_data_t *data) {
    if (!info_overlay || !data) return;
    if (lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (info_current_type != INFO_OVERLAY_CAMERA) return;

    populate_camera_data(data);

    if (info_loading_lbl) lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_HIDDEN);
}

void nina_info_overlay_set_mount_data(const mount_detail_data_t *data) {
    if (!info_overlay || !data) return;
    if (lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (info_current_type != INFO_OVERLAY_MOUNT) return;

    populate_mount_data(data);

    if (info_loading_lbl) lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_HIDDEN);
}

void nina_info_overlay_set_imagestats_data(const imagestats_detail_data_t *data) {
    if (!info_overlay || !data) return;
    if (lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (info_current_type != INFO_OVERLAY_IMAGESTATS) return;
    populate_imagestats_data(data);
    if (info_loading_lbl) lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_HIDDEN);
}

void nina_info_overlay_set_sequence_data(const sequence_detail_data_t *data) {
    if (!info_overlay || !data) return;
    if (lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (info_current_type != INFO_OVERLAY_SEQUENCE) return;
    populate_sequence_data(data);
    if (info_loading_lbl) lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_HIDDEN);
}

void nina_info_overlay_set_filter_data(const filter_detail_data_t *data) {
    if (!info_overlay || !data) return;
    if (lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (info_current_type != INFO_OVERLAY_FILTER) return;
    populate_filter_data(data);
    if (info_loading_lbl) lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_HIDDEN);
}

void nina_info_overlay_set_autofocus_data(const autofocus_data_t *data) {
    if (!info_overlay || !data) return;
    if (lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (info_current_type != INFO_OVERLAY_AUTOFOCUS) return;
    populate_autofocus_data(data);
    if (info_loading_lbl) lv_obj_add_flag(info_loading_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* ── Theme ───────────────────────────────────────────────────────── */

void nina_info_overlay_apply_theme(void) {
    if (!info_overlay || !current_theme) return;

    int gb = app_config_get()->color_brightness;

    /* Overlay background */
    lv_obj_set_style_bg_color(info_overlay, lv_color_hex(current_theme->bg_main), 0);

    /* Title */
    if (info_lbl_title) {
        lv_obj_set_style_text_color(info_lbl_title,
            lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    }

    /* Instance label */
    if (info_lbl_instance) {
        lv_obj_set_style_text_color(info_lbl_instance,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    /* Loading label */
    if (info_loading_lbl) {
        lv_obj_set_style_text_color(info_loading_lbl,
            lv_color_hex(info_get_text_color(gb)), 0);
    }

    /* Back button */
    if (info_btn_back) {
        lv_obj_set_style_bg_color(info_btn_back, lv_color_hex(current_theme->bento_border), 0);
        lv_obj_set_style_bg_color(info_btn_back,
            lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);
    }
    if (info_btn_back_lbl) {
        lv_obj_set_style_text_color(info_btn_back_lbl,
            lv_color_hex(info_get_text_color(gb)), 0);
    }

    /* Dispatch theme update to active content builder */
    if (!lv_obj_has_flag(info_overlay, LV_OBJ_FLAG_HIDDEN)) {
        switch (info_current_type) {
        case INFO_OVERLAY_CAMERA:    theme_camera_content();    break;
        case INFO_OVERLAY_MOUNT:     theme_mount_content();     break;
        case INFO_OVERLAY_AUTOFOCUS: theme_autofocus_content(); break;
        default: break;
        }
    }
}
