#pragma once

/**
 * @file nina_allsky.h
 * @brief AllSky four-quadrant page — thermal, SQM, ambient, power monitoring.
 */

#include "lvgl.h"
#include "allsky_client.h"

/**
 * @brief Create the AllSky page with a 2x2 quadrant layout.
 * @param parent Parent LVGL container (main_cont)
 * @return The page object (caller hides it initially)
 */
lv_obj_t *allsky_page_create(lv_obj_t *parent);

/**
 * @brief Update all quadrant widgets with latest AllSky data.
 * @param data Pointer to the allsky_data_t struct (mutex must be held by caller)
 */
void allsky_page_update(const allsky_data_t *data);

/**
 * @brief Apply the current theme to all AllSky page widgets.
 */
void allsky_page_apply_theme(void);

/**
 * @brief Re-read config and rebuild threshold/field mappings.
 * Call after allsky_field_config or allsky_thresholds change.
 */
void allsky_page_refresh_config(void);
