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
 * Returns the lv_arclabel; the caller sets colour and text through the lv_arclabel API. */
lv_obj_t *ui_arclabel_create(lv_obj_t *parent, const lv_font_t *font, int radius,
                             int angle_start_deg, int angle_size_deg, bool dir_cw,
                             lv_arclabel_text_align_t halign);

/* Convenience for the two rim conventions every G1 page uses. */
lv_obj_t *ui_arclabel_top(lv_obj_t *parent, const lv_font_t *font, int radius);     /* centred on twelve o'clock, clockwise */
lv_obj_t *ui_arclabel_bottom(lv_obj_t *parent, const lv_font_t *font, int radius);  /* centred on six o'clock, counter-clockwise */
