#pragma once

#include "lvgl.h"

/**
 * @brief Create the display settings page.
 * @param parent Parent LVGL container (main_cont)
 * @return The page object (caller hides it initially)
 */
lv_obj_t *settings_page_create(lv_obj_t *parent);

/**
 * @brief Load current config values into all settings widgets.
 * Call this each time the page becomes visible.
 */
void settings_page_refresh(void);

/**
 * @brief Apply the current theme to the settings page.
 */
void settings_page_apply_theme(void);
