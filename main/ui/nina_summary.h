#pragma once

#include "lvgl.h"
#include "nina_client.h"

/**
 * @brief Create the summary page showing connected NINA instances at a glance.
 *
 * Pre-creates cards for ALL MAX_NINA_INSTANCES fixed-identity slots (hidden by
 * default). cards[i] always corresponds to NINA instance i, regardless of which
 * slots are currently enabled. Visibility is driven at runtime by
 * nina_slot_available[] and live connection state in summary_page_update().
 * An empty-state message is displayed when no instances are connected.
 *
 * @param parent Parent LVGL container (main_cont)
 * @return The page object (caller hides it initially if needed)
 */
lv_obj_t *summary_page_create(lv_obj_t *parent);

/**
 * @brief Re-evaluate card visibility for the current nina_slot_available[] set.
 *
 * Hides cards for unavailable slots and resets the layout epoch so the next
 * summary_page_update() forces a full layout pass. Must be called under the
 * LVGL display lock. Called by nina_dashboard_rebuild_slot (Task 1.4) after a
 * slot is created or destroyed.
 */
void summary_page_rebuild(void);

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
