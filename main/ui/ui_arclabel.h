/**
 * @file ui_arclabel.h
 * @brief Text on the panel rim, over the vendored LVGL lv_arclabel widget.
 *
 * Round-only source: main/CMakeLists.txt compiles ui_arclabel.c into
 * nina_round_srcs, so the square binary carries neither the wrapper nor the
 * widget's draw path.
 *
 * Guideline G1 puts caption-class text (names, step, state, region, timestamp,
 * the clock condition and stats rows) along the rim. Large ticking numerals
 * stay on chords, on a plain lv_label.
 */
#pragma once

#include "lvgl.h"

/* Text on an arc centred on the parent's centre. angle_start_deg follows lv_arclabel:
 * 0 = three o'clock, clockwise positive. radius is the text baseline radius in px.
 * dir_cw false draws the glyphs counter-clockwise (readable on the bottom rim).
 * Returns the lv_arclabel; the caller sets colour and text through the lv_arclabel API.
 * The wrapper sizes its own object to the full panel, so it never clips its own
 * glyphs regardless of an ancestor's padding, but every ancestor up to the screen
 * still clips at ITS OWN coords: the parent must be the full-panel page root or
 * lv_layer_top(), never a padded container, or rim text is clipped away. */
lv_obj_t *ui_arclabel_create(lv_obj_t *parent, const lv_font_t *font, int radius,
                             int angle_start_deg, int angle_size_deg, bool dir_cw,
                             lv_arclabel_text_align_t halign);

/* Convenience for the two rim conventions every G1 page uses. */
lv_obj_t *ui_arclabel_top(lv_obj_t *parent, const lv_font_t *font, int radius);     /* centred on twelve o'clock, clockwise */
lv_obj_t *ui_arclabel_bottom(lv_obj_t *parent, const lv_font_t *font, int radius);  /* centred on six o'clock, counter-clockwise */

/* Adds a translucent black band (a plain lv_arc) that sits directly behind
 * arclabel's glyph run and is refitted to the text's angular extent on every
 * ui_arclabel_set_text() call. pad_px pads the band beyond the glyph cell on
 * all sides (radially and at both angular ends). Returns the band, or NULL
 * when arclabel is NULL or the band could not be created.
 *
 * The band is a sibling of arclabel (same parent, centred the same way), not
 * a child, and it takes over arclabel's user_data to remember itself: from
 * this call on, arclabel's user_data belongs to this wrapper. The band is
 * created after arclabel, so it starts on top, then moved directly beneath
 * it. Because it is a sibling, deleting arclabel alone leaves the band
 * behind; every page that uses this deletes the whole page root, which takes
 * both. The band starts hidden and stays hidden until a non-empty
 * ui_arclabel_set_text() call sizes it and reveals it. */
lv_obj_t *ui_arclabel_add_band(lv_obj_t *arclabel, int pad_px);

/* Sets arclabel's text and refits its band (if ui_arclabel_add_band() was
 * called for it) to the new text's angular window. NULL-safe: a NULL text is
 * treated as empty. Empty text hides the band; the arclabel itself is always
 * updated. Safe to call on an arclabel with no band. */
void ui_arclabel_set_text(lv_obj_t *arclabel, const char *text);
