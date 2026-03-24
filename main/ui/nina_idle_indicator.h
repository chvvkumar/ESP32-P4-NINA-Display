#pragma once

/**
 * @file nina_idle_indicator.h
 * @brief Minimal dot-in-circle connection status indicator for idle pages.
 *
 * Renders a 12px ring with a 4px glowing dot, shown when idle page
 * override is active.  Red = disconnected, green = reconnecting.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * Create the indicator on a page.  Call once per target page during page creation.
 * Uses LV_OBJ_FLAG_FLOATING so it overlays any layout (flex or absolute).
 *
 * @param parent    LVGL parent object for the indicator.
 * @param align     Alignment within parent (e.g. LV_ALIGN_BOTTOM_MID, LV_ALIGN_TOP_MID).
 * @param bare_dot  true = bare dot only (no circle container), false = dot inside circle.
 */
void nina_idle_indicator_create(lv_obj_t *parent, lv_align_t align, bool bare_dot);

/**
 * Show/hide indicator based on idle override state.
 * Called from tasks.c (non-UI thread — acquires LVGL lock internally).
 *
 * When idle_active=true and config idle_indicator_enabled=true, shows red dot.
 * When idle_active=false, hides all indicators.
 */
void nina_idle_indicator_set_active(bool idle_active);

/**
 * Reset all indicator state (zero pointers and count).
 * Call during page teardown before pages are recreated.
 * Caller must hold the LVGL lock.
 */
void nina_idle_indicator_reset(void);

/**
 * Briefly flash green before idle override restores previous page.
 * Called from tasks.c (non-UI thread — acquires LVGL lock internally).
 */
void nina_idle_indicator_set_reconnecting(void);

#ifdef __cplusplus
}
#endif
