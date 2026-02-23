#pragma once

#include "lvgl.h"
#include "nina_client.h"

/**
 * @brief Create the summary page showing connected NINA instances at a glance.
 *
 * Pre-creates cards for all configured instances (hidden by default).
 * Cards are shown/hidden dynamically based on connectivity in summary_page_update().
 * An empty-state message is displayed when no instances are connected.
 *
 * @param parent Parent LVGL container (main_cont)
 * @param instance_count Number of configured NINA instances
 * @return The page object (caller hides it initially if needed)
 */
lv_obj_t *summary_page_create(lv_obj_t *parent, int instance_count);

/**
 * @brief Update summary cards with live NINA data.
 *
 * Only connected instances are shown. Cards auto-resize via flex_grow
 * with 3-tier font scaling (1, 2, or 3 visible cards).
 * Shows empty state when all instances are disconnected.
 *
 * @param instances Array of nina_client_t data for all instances
 * @param count Number of instances
 */
void summary_page_update(const nina_client_t *instances, int count);

/**
 * @brief Apply the current theme to the summary page.
 */
void summary_page_apply_theme(void);
