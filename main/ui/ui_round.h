/**
 * @file ui_round.h
 * @brief Round layout geometry, derived from the runtime panel geometry.
 *
 * Header-only, pure and free of ESP-IDF, FreeRTOS and LVGL, so
 * test/host/test_ui_round.c compiles it directly. Every phase 2 round builder
 * takes its pixels from these five helpers plus screen_size() and
 * screen_center(); no builder carries a literal panel width and none of them
 * branches on the family.
 *
 * The mockups pin the rim to Rs = 0.95 R, which is 342 px at 720 and 380 px at
 * 800. Every rim ring and rim text radius is ui_rim_radius() minus an absolute
 * pixel offset taken from the mockup (12, 30, 40 or 68), so the two round sizes
 * share one stroke and one font and spend the extra diameter on chord width.
 */
#pragma once

#include <math.h>
#include "screen_geom.h"

#define UI_SQUARE_INSET 16   /* == OUTER_PADDING in nina_dashboard_internal.h */

/* Pad a page applies to its own root: 16 on square, the inscribed-square inset on round. */
static inline int ui_page_inset(void)
{
    return SCREEN_ROUND ? screen_safe_inset() : UI_SQUARE_INSET;
}

/* Root edge for an inset page: 688 on the 4B, 510 on the 4C, 564 on the 3.4C. */
static inline int ui_page_root_size(void)
{
    return screen_size() - 2 * ui_page_inset();
}

/* Rim radius Rs = 0.95 R: 342 at 720, 380 at 800. 0 on square. */
static inline int ui_rim_radius(void)
{
    return screen_safe_radius() * 19 / 20;
}

/* Half chord of the rim circle at vertical offset dy from the centre. 0 outside. */
static inline int ui_chord_half(int dy)
{
    int rs = ui_rim_radius();
    if (dy < 0) dy = -dy;
    if (dy >= rs) return 0;
    return (int)sqrtf((float)(rs * rs - dy * dy));
}

/* Full chord width at absolute panel y (0 = top edge). */
static inline int ui_chord_at_y(int y)
{
    return 2 * ui_chord_half(y - screen_center());
}
