#pragma once

/**
 * @file nina_empty_state.h
 * @brief Shared empty/idle-state LVGL component (IDLE-01).
 *
 * Renders a Material Symbols icon (accent color), a cause title
 * (primary text color), and a muted remedy subtitle, optically
 * centered at ~42% of the parent height with a 250ms fade-in.
 *
 * All public functions MUST be called with the display lock held
 * by the caller (bsp_display_lock / lvgl_port_lock).  This module
 * does not take the lock itself -- mirrors the nina_wait_overlay
 * convention.
 */

#include "lvgl.h"
#include "themes.h"
#include <stdbool.h>

/* ── Icon UTF-8 macros ─────────────────────────────────────────────────
 * Codepoints verified against MaterialSymbolsOutlined.ttf via fontTools
 * getBestCmap() on 2026-06-28.
 *
 *   cloud_off  U+E2C1  (no-NINA / node-offline states)
 *   music_off  U+E440  (Spotify nothing-playing state)
 *   image      U+E251  (image-loading / image display context)
 *
 * RESEARCH.md assumed values were wrong for cloud_off (U+E2BE) and
 * music_off (U+E7F4) -- those map to cloud_circle and notifications
 * respectively.  The verified values above are the correct glyphs.
 * ──────────────────────────────────────────────────────────────────── */
#define ICON_CLOUD_OFF  "\xee\x8b\x81"   /* U+E2C1 cloud_off  */
#define ICON_MUSIC_OFF  "\xee\x91\x80"   /* U+E440 music_off  */
#define ICON_IMAGE      "\xee\x89\x91"   /* U+E251 image      */

/**
 * @brief Create an empty-state widget as a child of @p parent.
 *
 * The container holds:
 *   - An icon label rendered with the Material Symbols idle font
 *     (omitted when @p icon_codepoint is NULL).
 *   - A title label (cause text) in the primary text color.
 *   - A remedy subtitle label in the muted secondary color.
 *
 * The container is created HIDDEN; call nina_empty_state_show() to
 * fade it in.  Position: optically centered at ~42% of parent height
 * (TOP_MID aligned, y-offset = SCREEN_SIZE * 42 / 100 minus half the
 * container's rendered height so the visual midpoint lands at 42%).
 *
 * The container does NOT have LV_OBJ_FLAG_CLICKABLE -- it must not
 * consume tap events intended for bento-grid overlays beneath it.
 *
 * Label pointers (icon, title, remedy) are stored in a small
 * heap-allocated struct (PSRAM via LVGL custom allocator) and
 * attached via lv_obj_set_user_data() on the returned container so
 * apply_theme and set_title can retrieve them without storing
 * additional file-scope state.
 *
 * @param parent              Parent LVGL object (must not be NULL).
 * @param icon_codepoint      UTF-8 Material Symbols codepoint string,
 *                            or NULL to omit the icon.
 * @param title               Cause text (e.g. "No NINA Connection").
 * @param remedy              Remedy subtitle text (may be NULL).
 * @param icon_color_override If non-zero, override the accent color
 *                            for the icon label only (D-01).
 * @return Pointer to the container, or NULL on failure.
 */
lv_obj_t *nina_empty_state_create(lv_obj_t *parent,
                                  const char *icon_codepoint,
                                  const char *title,
                                  const char *remedy,
                                  uint32_t icon_color_override);

/**
 * @brief Show the empty state with a 250ms fade-in.
 *
 * Sets opacity to transparent, removes LV_OBJ_FLAG_HIDDEN, then calls
 * lv_obj_fade_in(cont, 250, 0).  The HIDDEN flag must be removed first
 * or the fade is a no-op on invisible objects (LVGL pitfall).
 *
 * @param cont  Container returned by nina_empty_state_create.
 *              No-op when NULL.
 */
void nina_empty_state_show(lv_obj_t *cont);

/**
 * @brief Hide the empty state immediately (no fade-out animation).
 *
 * Instant hide prevents flicker on reconnect.  No-op when NULL.
 *
 * @param cont  Container returned by nina_empty_state_create.
 */
void nina_empty_state_hide(lv_obj_t *cont);

/**
 * @brief Re-apply theme colors to icon, title, and remedy labels.
 *
 * Call from the consumer page's apply_theme function whenever the
 * theme or color_brightness changes.  No-op when cont or theme is NULL.
 *
 * @param cont             Container returned by nina_empty_state_create.
 * @param theme            Active theme (accent, text, label tokens).
 * @param color_brightness Color brightness 0-100 (from app_config_t).
 */
void nina_empty_state_apply_theme(lv_obj_t *cont,
                                  const theme_t *theme,
                                  int color_brightness);

/**
 * @brief Update the title label text without recreating the widget.
 *
 * Used by the disconnected-NINA consumer to refresh the hostname after
 * a URL change.  No-op when cont or title is NULL.
 *
 * @param cont   Container returned by nina_empty_state_create.
 * @param title  New title text.
 */
void nina_empty_state_set_title(lv_obj_t *cont, const char *title);
