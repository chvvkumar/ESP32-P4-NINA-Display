#ifndef NINA_DASHBOARD_H
#define NINA_DASHBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "nina_client.h"
#include "app_config.h"

/**
 * @brief Initialize the multi-page Bento NINA Dashboard (720x720px grid layout)
 * @param parent The parent LVGL object (usually the screen)
 * @param instance_count Number of NINA instances to create pages for (1-3)
 */
void create_nina_dashboard(lv_obj_t * parent, int instance_count);

/**
 * @brief Update a single dashboard page with live NINA client data
 * @param page_index Index of the page to update (0-2)
 * @param data Pointer to the NINA client data for this page
 */
void update_nina_dashboard_page(int page_index, const nina_client_t *data);

/**
 * @brief Switch the visible dashboard page
 * @param page_index Index of the page to show (0-2)
 * @param instance_count Total number of configured instances
 */
void nina_dashboard_show_page(int page_index, int instance_count);

/**
 * @brief Get the currently active page index
 * @return Active page index (0-2)
 */
int nina_dashboard_get_active_page(void);

/**
 * @brief Apply a specific theme to all dashboard pages
 * @param theme_index Index of the theme to apply
 */
void nina_dashboard_apply_theme(int theme_index);

/**
 * @brief Callback type for page change events (triggered by swipe gestures)
 * @param new_page The page index that was switched to
 */
typedef void (*nina_page_change_cb_t)(int new_page);

/**
 * @brief Set callback for page change events from swipe gestures
 * @param cb Callback function to invoke on page change
 */
void nina_dashboard_set_page_change_cb(nina_page_change_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif // NINA_DASHBOARD_H
