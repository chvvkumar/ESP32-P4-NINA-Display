#pragma once

/**
 * @file display_defs.h
 * @brief Display-related constants shared across modules.
 *
 * Provides constants that are about the display hardware or LVGL usage,
 * not about application configuration.
 */

/** Standardized LVGL display lock timeout (ms).
 *  All callers should use this instead of 0 (infinite) to prevent deadlocks. */
#define LVGL_LOCK_TIMEOUT_MS 1000

/* Panel geometry is a runtime value: screen_size(), screen_center(),
 * screen_safe_inset(), screen_safe_radius() and SCREEN_ROUND. */
#include "screen_geom.h"

/**
 * @brief Set the screen rotation (0-3 = 0/90/180/270 degrees).
 *
 * The single entry point for changing rotation. As well as calling
 * lv_display_set_rotation() it re-binds LVGL's draw buffers for the PPA
 * hardware-rotation layout, so setting the rotation any other way leaves the
 * buffers bound for the wrong mode. Safe to call repeatedly and in either
 * direction (0 <-> non-0).
 *
 * @note The caller must already hold the display lock. Never call this from an
 *       ISR or from the display flush callback.
 */
void display_rotation_apply(int rot);
