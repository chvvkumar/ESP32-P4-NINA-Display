#ifndef NINA_DASHBOARD_H
#define NINA_DASHBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "nina_client.h"

/**
 * @brief Initialize the Bento NINA Dashboard (720x720px grid layout)
 * @param parent The parent LVGL object (usually the screen)
 */
void create_nina_dashboard(lv_obj_t * parent);

/**
 * @brief Update the dashboard with live NINA client data from both instances
 * @param data1 Pointer to instance 1 data (primary scope)
 * @param data2 Pointer to instance 2 data (secondary scope, may be NULL)
 */
void update_nina_dashboard_ui(const nina_client_t *data1, const nina_client_t *data2);

/**
 * @brief Apply a specific theme to the dashboard
 * @param theme_index Index of the theme to apply
 */
void nina_dashboard_apply_theme(int theme_index);

#ifdef __cplusplus
}
#endif

#endif // NINA_DASHBOARD_H