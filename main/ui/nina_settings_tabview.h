#pragma once

#include "lvgl.h"

/**
 * @brief Create the 4-tab settings tabview page.
 * @param parent The dashboard main container
 * @return The root tabview object
 */
lv_obj_t *settings_tabview_create(lv_obj_t *parent);

/** Destroy all settings widgets and free memory. Call when leaving settings page. */
void settings_tabview_destroy(void);

/** Refresh all tab contents from current config. */
void settings_tabview_refresh(void);

/** Re-apply current theme colors to all settings widgets. */
void settings_tabview_apply_theme(void);

/** @return true if the on-screen keyboard is currently visible. */
bool settings_tabview_keyboard_active(void);

/**
 * @brief Get or create the shared keyboard overlay for text input.
 * Tabs call this to get the keyboard reference.
 * @return The lv_keyboard object
 */
lv_obj_t *settings_get_keyboard(void);

/**
 * @brief Show the keyboard and assign it to a textarea.
 * @param ta The textarea to associate with the keyboard
 */
void settings_show_keyboard(lv_obj_t *ta);

/** Hide the shared keyboard. */
void settings_hide_keyboard(void);

/* Shared widget helpers for settings tabs */
lv_obj_t *settings_make_card(lv_obj_t *parent, const char *title);
lv_obj_t *settings_make_row(lv_obj_t *parent);
lv_obj_t *settings_make_divider(lv_obj_t *parent);
lv_obj_t *settings_make_stepper(lv_obj_t *parent, lv_obj_t **out_minus,
                                 lv_obj_t **out_label, lv_obj_t **out_plus);
lv_obj_t *settings_make_toggle_row(lv_obj_t *parent, const char *text,
                                    lv_obj_t **out_sw);
lv_obj_t *settings_make_textarea_row(lv_obj_t *parent, const char *label_text,
                                      const char *placeholder, bool password,
                                      lv_obj_t **out_ta);

/** Mark config as dirty — call from tab callbacks when values change. */
void settings_mark_dirty(bool reboot_required);

/** Shared recursive theme walker for all settings widgets. */
void settings_apply_theme_recursive(lv_obj_t *obj);
