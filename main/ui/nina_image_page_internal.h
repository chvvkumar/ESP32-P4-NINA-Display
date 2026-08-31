#pragma once

/**
 * @file nina_image_page_internal.h
 * @brief Layout seam for the image page spine.
 *
 * The page (nina_image_page.c) owns the frames, the timers, the config reads,
 * the formatting and every colour. A builder creates and places the caption
 * widgets and the Moon labels and writes the handles into image_page_t, and
 * does nothing else. Exactly one builder is compiled per shape family; the
 * square one is a whole-function #if in nina_image_page.c, the round one lives
 * in nina_image_page_round.c and is listed in nina_round_srcs.
 *
 * Every entry point here runs on the LVGL task with the display lock already
 * held by the caller. Nothing here takes the lock.
 */

#include "nina_image_page.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply the caption text style: transparent background, historic chip padding
 * and the theme's caption colour (white, or the theme red under Red Night).
 * An lv_arclabel gets the colour only, never the padding or the background
 * (guideline C1: no rectangular background behind rim text).
 */
void image_page_caption_style(lv_obj_t *lbl);

/* Caption slots for set_caption_if_changed() / p->caption_shadow. */
#define IMG_CAP_REGION 0
#define IMG_CAP_STAMP  1

/**
 * Build overlay_bar, lbl_region, lbl_timestamp and, on the Moon instance, the
 * four moon labels plus any shape that replaces a number. @p page_container is
 * p->root; the two crossfade images already exist as its children 0 and 1, so
 * everything created here stacks above them.
 *
 * A builder sets sizes, angles, ranges and alignment only. Colours come from
 * the page: image_page_caption_style() for text, moon_arc_apply_theme() for the
 * lunar age arc and its tick (spec B.2).
 */
void image_page_build_overlay_square(image_page_t *p, lv_obj_t *page_container);
void image_page_build_overlay_round(image_page_t *p, lv_obj_t *page_container);

#ifdef __cplusplus
}
#endif
