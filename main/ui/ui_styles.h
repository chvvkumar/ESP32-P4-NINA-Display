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

/**
 * @brief Attach or update custom draw event callbacks on a bento box widget.
 *
 * For styles that need custom rendering (wireframe corner accents, chamfered
 * polygon), this attaches or updates the necessary LV_EVENT_DRAW_MAIN /
 * LV_EVENT_DRAW_POST callbacks. Safe to call repeatedly; callbacks are
 * added only once per object.
 *
 * @param obj The bento box LVGL object
 */
void ui_styles_set_widget_draw_cbs(lv_obj_t *obj);

/**
 * @brief Get the display name of a widget style by index.
 * @param index Style index (0..WIDGET_STYLE_COUNT-1)
 * @return Static string name, or "Default" for out-of-range
 */
const char *ui_styles_get_widget_style_name(int index);
