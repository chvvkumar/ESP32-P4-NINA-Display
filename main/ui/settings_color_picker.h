#pragma once

#include "lvgl.h"

/**
 * @brief Show a modal color palette popup.
 * @param current_color The currently selected color (hex, e.g. 0xFF0000)
 * @param cb Callback invoked when a color is selected: cb(selected_color_hex, user_data)
 * @param user_data Opaque pointer passed to callback
 */
void color_picker_show(uint32_t current_color,
                       void (*cb)(uint32_t color, void *user_data),
                       void *user_data);

/** Hide the color picker popup if visible. */
void color_picker_hide(void);

/** Re-apply theme to the color picker popup. */
void color_picker_apply_theme(void);
