#pragma once

/**
 * @file ui_text_fit.h
 * @brief Shared font-ladder fitter and write guard for text labels.
 *
 * Compiled on both families. Lifted out of nina_dashboard_update.c, where the
 * ladder + memo used to be private statics, so the round NINA layouts can size
 * a name row against a chord width instead of a parent's content width.
 *
 * The memo is keyed on the label object; a slot is released on that object's
 * LV_EVENT_DELETE, so a page rebuild can never inherit a previous label's pick.
 *
 * All entry points must be called with the LVGL display lock already held.
 */

#include "lvgl.h"

/* Name-row ladder: the largest built-in Montserrat faces down to the 24 px
 * text floor, plus 20 for a name no 24 px face can fit. Every size here is
 * enabled in sdkconfig.defaults; none of them is a glyph subset. */
#define UI_FIT_LADDER_NAME_N 9
extern const lv_font_t *const UI_FIT_LADDER_NAME[UI_FIT_LADDER_NAME_N];

/* Same ladder starting at 28, for a name row that has to share its band with
 * other rows and must not tower over them: Montserrat 28, 26, 24, 20. It is the
 * tail of UI_FIT_LADDER_NAME, kept as its own symbol so a caller does not have
 * to index into the other one by a magic offset. */
#define UI_FIT_LADDER_NAME_28_N 4
extern const lv_font_t *const UI_FIT_LADDER_NAME_28[UI_FIT_LADDER_NAME_28_N];

/**
 * @brief Apply the largest ladder face whose text width fits @p avail_px.
 *
 * Font only: width, height and long mode are left exactly as the caller set
 * them. This is the behaviour the square bento page has always had.
 *
 * @param label     Label to measure and re-font
 * @param ladder    Faces, LARGEST FIRST
 * @param n         Face count
 * @param avail_px  Width the text has to fit into
 */
void ui_fit_label_font(lv_obj_t *label, const lv_font_t *const *ladder, int n,
                       int avail_px);

/**
 * @brief ui_fit_label_font() plus a bounded single-line box that ellipsises.
 *
 * After picking the face it sets the label to @p avail_px wide, one line of the
 * picked face tall and LV_LABEL_LONG_DOT, so a name that overflows even the
 * smallest face ends in dots instead of wrapping into whatever sits below it.
 * The height must be explicit: LVGL only inserts dots when the height is
 * bounded, so a content-sized label never dots.
 */
void ui_fit_label(lv_obj_t *label, const lv_font_t *const *ladder, int n,
                  int avail_px);

/**
 * @brief Write @p text to @p label only when it differs from the last write.
 *
 * The guard compares against a per-label shadow copy (63 chars), NOT against
 * lv_label_get_text(): once LVGL has ellipsised a label it has rewritten the
 * label's own text in place, so a strcmp against it never matches again and the
 * full panel repaints on every poll.
 */
void ui_label_set_text(lv_obj_t *label, const char *text);
