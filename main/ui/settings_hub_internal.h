#pragma once

/**
 * @file settings_hub_internal.h
 * @brief Layout seam for the Panel Mode settings hub.
 *
 * settings_hub.c owns every screen's data, callbacks, persistence and colours
 * and builds the square composition. A round fit pass, compiled only on the
 * round family, re-places the widgets that screen already built. The two
 * handles below are the contract: the builder writes them, the fit pass reads
 * them, and they are NULL on a screen that has no such object.
 *
 * Display lock held by the caller.
 */

#include "settings_hub.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern lv_obj_t *hub_header_obj;   /* header row of the screen just built, else NULL */
extern lv_obj_t *hub_grid_obj;     /* hub tile grid or theme card grid, else NULL */

/** One hub tile: big name label (child 0) + status line (child 1), whole tile
 *  is the target. Square passes HUB_TILE_W / HUB_TILE_H. */
lv_obj_t *settings_hub_make_tile(lv_obj_t *parent, const char *name, const char *status,
                                 lv_event_cb_t cb, int w, int h);

/** One theme preview card painted in theme @p idx's own palette. @p active is
 *  the currently applied theme index; @p red_only forces the Red Night
 *  single-hue preview. Square passes HUB_CARD_W / HUB_CARD_H. */
lv_obj_t *settings_hub_make_theme_card(lv_obj_t *parent, int idx, int active,
                                       bool red_only, int w, int h);

/** Round composition pass, called once at the end of settings_hub_goto().
 *  Re-places what the screen builder produced; creates nothing new except the
 *  theme screen's BACK pill styling. */
void settings_hub_round_fit(lv_obj_t *screen, hub_screen_t which);

/** Raise the shared header's BACK label to the 27 px round floor (it ships at
 *  lv_font_montserrat_24). Called by every round fit pass that keeps a header,
 *  including the WiFi ones in settings_wifi_round.c. */
void settings_hub_round_header_font(lv_obj_t *header);

/** Raise every direct-child label of @p cont still using an under-floor
 *  Montserrat face to lv_font_montserrat_28 (addendum section 5). Compares font
 *  pointers, not line heights. Shared with settings_wifi_round.c. */
void settings_hub_round_raise_small_labels(lv_obj_t *cont);

#ifdef __cplusplus
}
#endif
