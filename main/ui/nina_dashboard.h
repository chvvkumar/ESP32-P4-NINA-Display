#ifndef NINA_DASHBOARD_H
#define NINA_DASHBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "nina_client.h"

/**
 * @brief Initialize the Modern Bento XL Dashboard
 * @param parent The parent LVGL object (usually the screen)
 */
void create_nina_dashboard(lv_obj_t * parent);

/**
 * @brief Update the dashboard metrics
 * @param data Pointer to the latest NINA client data
 */
void update_nina_dashboard_ui(const nina_client_t *data);

#ifdef __cplusplus
}
#endif

#endif // NINA_DASHBOARD_H
