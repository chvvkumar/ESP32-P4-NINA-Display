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
 *   cloud      U+E2BD  (Weather Radar / Cloud Cover loading placeholder)
 *
 * RESEARCH.md assumed values were wrong for cloud_off (U+E2BE) and
 * music_off (U+E7F4) -- those map to cloud_circle and notifications
 * respectively.  The verified values above are the correct glyphs.
 *
 * cloud (U+E2BD) added 2026-09-03, verified against the same TTF via
 * fontTools getBestCmap() the same day: U+E2BD -> "cloud" (plain, no
 * strike-through). cloud_off stays reserved for offline states; the
 * loading placeholder uses the plain cloud instead.
 * ──────────────────────────────────────────────────────────────────── */
#define ICON_CLOUD_OFF  "\xee\x8b\x81"   /* U+E2C1 cloud_off  */
#define ICON_MUSIC_OFF  "\xee\x91\x80"   /* U+E440 music_off  */
#define ICON_CLOUD      "\xee\x8a\xbd"   /* U+E2BD cloud (plain, loading states) */

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
 * (TOP_MID aligned, y-offset = screen_size() * 42 / 100 minus half the
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
 * @brief Turn the "working on it" pulse on the icon on or off.
 *
 * While busy is true the icon pulses between 40% and full opacity on a
 * ~1.2 s infinite animation.  When busy is false the animation stops and
 * the icon returns to full opacity.  Busy also hides the remedy subtitle
 * (it describes a failure, not a wait); clearing busy shows it again.  No-op
 * on the remedy when the container was created without one.  Idempotent:
 * calling it repeatedly with the same value does not restart the pulse or
 * re-touch the labels (safe to call every poll cycle).  The pulse only runs
 * while the container is visible; hide() stops it and show() re-arms it when
 * busy is still set.
 *
 * Caller holds the display lock.  No-op when cont is NULL or the container
 * was created without an icon.
 *
 * @param cont  Container returned by nina_empty_state_create.
 * @param busy  true to pulse, false to stop.
 */
void nina_empty_state_set_busy(lv_obj_t *cont, bool busy);

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

/**
 * @brief Show a done-of-total segmented progress bar under the title.
 *
 * Used by the Weather Radar and Cloud Cover loading placeholders to show how
 * many loop frames have been downloaded out of the total the loop will hold,
 * so a multi-minute wait reads as work rather than a hang.  Any other
 * placeholder consumer may call it; those that never do are unaffected.
 *
 * @p total <= 0 hides the row and its caption (the default state at create
 * time).  Otherwise the row shows one segment per unit of @p total with the
 * first @p done of them lit (@p done is clamped into 0..total), and the
 * caption beneath it reads "<done> of <total>".  A total above 16 is drawn as
 * 16 segments with the lit count scaled down to match.  Idempotent: repeating
 * the same pair does not invalidate anything, so it is safe to call every poll
 * cycle.
 *
 * Caller holds the display lock.  No-op when cont is NULL.
 *
 * @param cont   Container returned by nina_empty_state_create.
 * @param done   Items completed so far.
 * @param total  Items expected in total; <= 0 hides the row.
 */
void nina_empty_state_set_progress(lv_obj_t *cont, int done, int total);

/**
 * @brief Swap a connecting/loading title for "Waiting for WiFi" while the
 *        station link has no IP address.
 *
 * Returns the literal "Waiting for WiFi" when net_sta_has_ip() is false, and
 * @p normal otherwise.  Callers pass their connecting/loading title through it
 * on EVERY refresh, not once at create time, so the text follows the link
 * state: a page opened before the radio has an address reads "Waiting for
 * WiFi" and switches to its own wording the moment the link comes up.
 *
 * Takes no lock and makes no LVGL call; the returned pointer is a string
 * literal or @p normal itself, so it borrows the caller's lifetime.
 *
 * @param normal  The title to use once the link is up (may be NULL).
 * @return "Waiting for WiFi" while offline, else @p normal.
 */
const char *nina_empty_state_wait_title(const char *normal);
