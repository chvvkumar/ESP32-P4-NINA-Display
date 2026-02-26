#pragma once

/**
 * @file nina_info_internal.h
 * @brief Shared internal state and helpers between info overlay modules.
 */

#include "nina_info_overlay.h"
#include "lvgl.h"
#include "themes.h"
#include "app_config.h"
#include "display_defs.h"
#include "ui_styles.h"

/* current_theme is defined in nina_dashboard.c */
extern const theme_t *current_theme;

/* Layout constants */
#define INFO_OUTER_PAD    16
#define INFO_CARD_PAD     14
#define INFO_CARD_ROW_GAP  8
#define INFO_BACK_BTN_W   84
#define INFO_BACK_BTN_H   108
#define INFO_BACK_BTN_ZONE (INFO_BACK_BTN_W + 8)  /* right-side exclusion for bottom rows */

/* ── Shared widget state (defined in nina_info_overlay.c) ─────────── */
extern lv_obj_t *info_overlay;
extern lv_obj_t *info_title_bar;
extern lv_obj_t *info_lbl_title;
extern lv_obj_t *info_lbl_instance;
extern lv_obj_t *info_content;
extern lv_obj_t *info_loading_lbl;
extern lv_obj_t *info_btn_back;
extern lv_obj_t *info_btn_back_lbl;

extern volatile bool info_requested;
extern info_overlay_type_t info_current_type;
extern int info_return_page;

/* ── Reusable widget factories ────────────────────────────────────── */

/** Card with bento_box style, column flex, 14px pad, 6px row gap. */
lv_obj_t *make_info_card(lv_obj_t *parent);

/** Section header label: font_14, letter_space=2, label_color. */
lv_obj_t *make_info_section(lv_obj_t *parent, const char *title);

/** Row with key (font_16, label_color) space-between value (font_18, text_color).
 *  Returns the row object. *out_val receives the value label pointer. */
lv_obj_t *make_info_kv(lv_obj_t *parent, const char *key, lv_obj_t **out_val);

/** Same as make_info_kv but value uses font_24 for emphasis. */
lv_obj_t *make_info_kv_wide(lv_obj_t *parent, const char *key, lv_obj_t **out_val);

/* ── Theme helpers ────────────────────────────────────────────────── */
bool info_is_red_night(void);
uint32_t info_get_text_color(int gb);

/* ── Content builder forward declarations (defined in per-type .c files) ─ */
void build_camera_content(lv_obj_t *content);
void populate_camera_data(const camera_detail_data_t *data);
void theme_camera_content(void);

void build_mount_content(lv_obj_t *content);
void populate_mount_data(const mount_detail_data_t *data);
void theme_mount_content(void);

void build_imagestats_content(lv_obj_t *content);
void populate_imagestats_data(const imagestats_detail_data_t *data);

void build_sequence_content(lv_obj_t *content);
void populate_sequence_data(const sequence_detail_data_t *data);

void build_filter_content(lv_obj_t *content);
void populate_filter_data(const filter_detail_data_t *data);

void build_autofocus_content(lv_obj_t *content);
void populate_autofocus_data(const autofocus_data_t *data);
void theme_autofocus_content(void);

void build_session_stats_content(lv_obj_t *content);
void populate_session_stats_data(int instance);
void theme_session_stats_content(void);
