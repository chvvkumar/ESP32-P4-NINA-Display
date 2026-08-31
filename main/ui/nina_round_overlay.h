#pragma once

/**
 * @file nina_round_overlay.h
 * @brief The picture overlay every round capture layout shares.
 *
 * Round family only (nina_round_srcs). One module draws everything that sits on
 * top of the capture on every round capture layout (0, 2 and 4), so the boards read the same over a
 * picture and only their READINGS-ONLY page differs:
 *
 *   crown   the safety shield glyph, centred at twelve, just inside the rim
 *   rim arc exposure progress, flush with the glass, filter coloured, with an
 *           equal gap either side of the crown. Stored in p->alt.arc_progress,
 *           so the spine's 200 ms tick drives it and the stale rule dims it
 *   stack   a flat 30 % black plate across the bottom of the disc carrying the
 *           target name, then RMS | elapsed seconds | HFR, then the filter name
 *           and the sub counter
 *
 * The overlay owns the interpolated seconds: it registers p->alt.elapsed_cb,
 * writes its own digits and then calls the layout's optional p->alt.elapsed_hook
 * with the same value, so a layout's readings-only hero has one writer and one
 * clock. Its widget handles live in p->alt.ov and belong to this module alone.
 *
 * The spine creates p->alt.cap_img and calls create() AFTER the layout's own
 * create(), so the overlay is above the layout's objects. All entry points run
 * with the LVGL display lock already held by the caller.
 */

#include "lvgl.h"
#include "nina_client.h"
#include "nina_dashboard_internal.h"
#include "nina_layout_alt.h"

/** @brief Build the crown, the rim arc and the bottom stack inside @p parent. */
void nina_round_overlay_create(dashboard_page_t *p, lv_obj_t *parent, int page_index);

/** @brief Push one poll's data into the overlay. */
void nina_round_overlay_update(dashboard_page_t *p, const nina_client_t *d,
                               int instance_idx, int gb);

/** @brief Re-tone the overlay in place. */
void nina_round_overlay_apply_theme(dashboard_page_t *p);

/**
 * @brief Show the composition for @p mode.
 *
 * FULL shows everything; ARC keeps the crown and the rim arc and drops the
 * plate and its rows; PICTURE hides everything the overlay owns; NUMBERS drops
 * the plate and its rows, keeps the crown, and hides the rim arc only when the
 * layout supplied its own readings-only ring in p->alt.arc_progress_num.
 * Idempotent, and it never creates or deletes a widget.
 */
void nina_round_overlay_set_view(dashboard_page_t *p, nina_view_mode_t mode);
