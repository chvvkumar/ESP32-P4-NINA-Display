#pragma once

/**
 * @file ui_helpers.h
 * @brief Shared widget factory functions and style references for the dashboard UI.
 *
 * Included by nina_dashboard_internal.h so all dashboard sub-modules have access.
 */

#include <stdint.h>

#include "lvgl.h"

#include "themes.h"
#include "ui_styles.h"

/* Active theme — defined in nina_dashboard.c, NULL before the first theme apply. */
extern const theme_t *current_theme;

/** Sentinel colour: "no theme active", leave the widget colour untouched. */
#define UI_COLOR_NONE 0xFFFFFFFFu

/**
 * @brief NULL-safe theme colour read, e.g. UI_THEME_COLOR(label_color).
 *
 * Yields UI_COLOR_NONE while no theme is applied, which the ui_* helpers below
 * treat as "do not set a colour" — the same result as the old
 * `if (current_theme) { ... }` guard around each call.
 */
#define UI_THEME_COLOR(field) (current_theme ? (uint32_t)current_theme->field : UI_COLOR_NONE)

/**
 * @brief Apply a theme colour (dimmed by the configured colour brightness) as text colour.
 * @param obj         Target object
 * @param theme_color Colour from UI_THEME_COLOR(); UI_COLOR_NONE is a no-op
 */
void ui_set_theme_text_color(lv_obj_t *obj, uint32_t theme_color);

/**
 * @brief Create a label with text, font and themed text colour in one call.
 * @param parent      Parent LVGL object
 * @param text        Label text (NULL leaves the default empty text)
 * @param font        Font to apply (NULL keeps the inherited font)
 * @param theme_color Colour from UI_THEME_COLOR(); UI_COLOR_NONE leaves the colour alone
 * @return Newly created label object
 */
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t theme_color);

/**
 * @brief Create a bento-box card: full width, content height, column flex, not scrollable.
 * @param parent  Parent LVGL object
 * @param pad     Padding on all sides
 * @param row_gap Vertical gap between children
 * @return Newly created card object
 */
lv_obj_t *ui_card(lv_obj_t *parent, int pad, int row_gap);

/** @brief Section header label: font_16, letter_space 2, themed label colour. */
lv_obj_t *ui_section_label(lv_obj_t *parent, const char *title);

/**
 * @brief Create a "key ....... value" row inside a card.
 * @param parent   Parent LVGL object
 * @param key      Key text (themed label colour)
 * @param key_font Font for the key
 * @param val_font Font for the value
 * @return The value label (initial text "--"); the row itself needs no further handling
 */
lv_obj_t *ui_kv(lv_obj_t *parent, const char *key, const lv_font_t *key_font,
                const lv_font_t *val_font);

/**
 * @brief Create a bento-box container with the shared bento box style.
 * @param parent Parent LVGL object
 * @return Newly created container object
 */
lv_obj_t *create_bento_box(lv_obj_t *parent);

/**
 * @brief Create a small label with the shared small label style.
 * @param parent Parent LVGL object
 * @param text   Initial text
 * @return Newly created label object
 */
lv_obj_t *create_small_label(lv_obj_t *parent, const char *text);

/**
 * @brief Create a large value label with the shared large value style.
 * @param parent Parent LVGL object
 * @return Newly created label object (initial text "--")
 */
lv_obj_t *create_value_label(lv_obj_t *parent);
