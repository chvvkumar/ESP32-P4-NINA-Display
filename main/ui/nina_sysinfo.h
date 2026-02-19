#pragma once

#include "lvgl.h"

/**
 * @brief Create the system info page (IP, hostname, WiFi, CPU, memory, etc.)
 * @param parent Parent LVGL container (main_cont)
 * @return The page object (caller hides it initially)
 */
lv_obj_t *sysinfo_page_create(lv_obj_t *parent);

/**
 * @brief Refresh all system statistics on the page.
 * Call this each time the page becomes visible.
 */
void sysinfo_page_refresh(void);

/**
 * @brief Apply the current theme to the system info page.
 */
void sysinfo_page_apply_theme(void);
