#pragma once

/**
 * @file ui_helpers.h
 * @brief Shared widget factory functions and style references for the dashboard UI.
 *
 * Included by nina_dashboard_internal.h so all dashboard sub-modules have access.
 */

#include "lvgl.h"

/* Shared styles — defined in nina_dashboard.c */
extern lv_style_t style_bento_box;
extern lv_style_t style_label_small;
extern lv_style_t style_value_large;
extern lv_style_t style_header_gradient;

/**
 * @brief Create a bento-box container with the shared bento box style.
 * @param parent Parent LVGL object
 * @return Newly created container object
 */
lv_obj_t *create_bento_box(lv_obj_t *parent);

/**
 * @brief Create a small label with the shared small label style.
 * @param parent Parent LVGL object
 * @param text   Initial text
 * @return Newly created label object
 */
lv_obj_t *create_small_label(lv_obj_t *parent, const char *text);

/**
 * @brief Create a large value label with the shared large value style.
 * @param parent Parent LVGL object
 * @return Newly created label object (initial text "--")
 */
lv_obj_t *create_value_label(lv_obj_t *parent);
