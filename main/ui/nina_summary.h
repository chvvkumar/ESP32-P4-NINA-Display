#pragma once

#include "lvgl.h"
#include "nina_client.h"

/**
 * @brief Create the summary page showing all NINA instances at a glance.
 * @param parent Parent LVGL container (main_cont)
 * @param instance_count Number of configured NINA instances
 * @return The page object (caller hides it initially if needed)
 */
lv_obj_t *summary_page_create(lv_obj_t *parent, int instance_count);

/**
 * @brief Update all summary cards with live NINA data.
 * Call this each polling cycle (or when the page is visible).
 * @param instances Array of nina_client_t data for all instances
 * @param count Number of instances
 */
void summary_page_update(const nina_client_t *instances, int count);

/**
 * @brief Apply the current theme to the summary page.
 */
void summary_page_apply_theme(void);
