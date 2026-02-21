#pragma once

/**
 * @file ui_styles.h
 * @brief Shared LVGL styles for the dashboard UI.
 *
 * Styles are initialized by ui_styles_update() and updated on theme change.
 * All widget factories in ui_helpers.h use these styles.
 */

#include "lvgl.h"

/* Shared style variables — defined in ui_styles.c */
extern lv_style_t style_bento_box;
extern lv_style_t style_label_small;
extern lv_style_t style_value_large;
extern lv_style_t style_header_gradient;

/**
 * @brief Initialize or re-apply styles from the given theme.
 * Called during dashboard creation and on theme change.
 */
void ui_styles_update(const void *theme);
