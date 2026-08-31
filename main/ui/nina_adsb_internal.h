#pragma once

/**
 * @file nina_adsb_internal.h
 * @brief Layout seam for the ADS-B page.
 *
 * nina_adsb.c owns EVERYTHING except widget construction and placement: the
 * snapshot, the gestures, the rotation, the projections, the declutter pass,
 * every string and every colour. A family builder creates widgets, positions
 * them, writes their handles through adsb_slots_t and fills adsb_geom_t with
 * the numbers the page's maths needs to agree with what was drawn.
 *
 * A builder may leave any slot untouched. Slots are addresses of the page's own
 * file statics, which are zero at first create, so an unbuilt widget stays NULL
 * and every write site in nina_adsb.c null-checks before it touches a handle.
 *
 * A builder sets NO colour: apply_colors() paints every handle it knows about,
 * and it runs before the first recompute().
 *
 * Everything here runs with the LVGL display lock held by the CALLER.
 */

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define ADSB_TAG_COUNT   3      /* Sky Dome boxed tags (rank 0-2) */
#define ADSB_BOARD_ROWS  5

#define ADSB_NO_GO_MAX   4      /* declutter exclusion rectangles */

/**
 * Family geometry. Indices [0] and [1] are the two disc modes (Sky, Scope);
 * the Board mode has no disc. Every value is absolute pixels: the 800 panel
 * keeps the same strokes and the same distances from the rim.
 */
typedef struct {
    int16_t card_off_v[2];   /* cardinal pull-in from the disc, near-vertical letters */
    int16_t card_off_h[2];   /* the same, near-horizontal letters */
    int16_t card_off_diag;   /* Scope only: a letter carried 15..75 degrees either
                              * side of up sits in the two rim arclabel spans
                              * (range, CONTACTS) and steps in to this instead.
                              * Square has no rim arclabels: 24, same as the rest */
    int16_t rim_w[2];        /* outer ring stroke width */
    int16_t ring_inset;      /* pull-in of the OUTERMOST ring number (slot 2) from
                              * its ring. The two inner numbers always use
                              * ADSB_RING_INSET_INNER. */
    int16_t ring_lbl_w;      /* width of a ring-number no-go rectangle, anchored
                              * 24 px left of the label position (same for all
                              * three slots). 84 covers the 22 px square face;
                              * the 28 px round face needs 100. */
    int16_t tag_h;           /* tag block height used by the declutter pass */
    /* The two tag text lines, shared by the Sky tag boxes and the Scope contact
     * labels the draw callback paints, so the declutter footprint and the drawn
     * text stay the same size. Pointers and ints only: the draw callback reads
     * them and still does no maths. */
    const lv_font_t *tag_font1;
    const lv_font_t *tag_font2;
    int16_t tag_l1_y;        /* line 1 offset inside the tag block */
    int16_t tag_l2_y;
    /* Vertical reserve for tag placement and the cardinal clamp, per disc mode
     * ([0] Sky, [1] Scope, indexed the same way as card_off_v/h). On round
     * these are the two 72 px chord caps on Sky; the round Scope draws no caps
     * so its reserve is 0. Square keeps the same 44 px header/strip reserve on
     * both modes. */
    int16_t scrim_top[2];
    int16_t scrim_bot[2];
    int16_t scope_lbl_r;     /* Scope: push a contact label outward to at most this
                              * radius. 0 keeps the plain quadrant search. */
    bool    short_caps;      /* Sky Dome mount/strip text uses the short forms that
                              * fit the round chord instead of the square sentence. */
    bool    board_marks;     /* Board mode also draws s_mark glyphs (heading arrows) */
    bool    ring_lbl_west;   /* Scope: ring numbers and the range label run along the
                              * nine o'clock radius instead of the NNW diagonal */
    uint8_t no_go_n;
    lv_area_t no_go[ADSB_NO_GO_MAX];
} adsb_geom_t;

/**
 * Where a builder writes its handles. Array members point at the first element
 * of the page's array; the counts are the ADSB_* constants above.
 */
typedef struct {
    lv_obj_t **disc;
    lv_obj_t **lbl_card;              /* [4] N E S W */
    lv_obj_t **lbl_ring;              /* [3] */
    lv_obj_t **tag_box;               /* [ADSB_TAG_COUNT] */
    lv_obj_t **tag_l1;
    lv_obj_t **tag_l2;
    lv_obj_t **hdr;
    lv_obj_t **lbl_title;
    lv_obj_t **lbl_mount;
    lv_obj_t **strip;
    lv_obj_t **lbl_strip;
    lv_obj_t **sc_within;
    lv_obj_t **sc_call;
    lv_obj_t **sc_alt;
    lv_obj_t **sc_dist;
    lv_obj_t **sc_rate;
    lv_obj_t **sc_cue;
    lv_obj_t **sc_contacts_ring;      /* Scope: within/tracked as a rim arc */
    lv_obj_t **sc_contacts_arclabel;  /* Scope: "CONTACTS n / m" on the bottom rim, an arclabel */
    lv_obj_t **sc_rate_arclabel;      /* Scope: "n msg/s" on the bottom rim, an arclabel */
    lv_obj_t **board;
    lv_obj_t **lbl_glance;
    lv_obj_t **lbl_gsub;
    lv_obj_t **row_panel;             /* [ADSB_BOARD_ROWS] */
    lv_obj_t **row_call;
    lv_obj_t **row_dot;
    lv_obj_t **row_rail;
    lv_obj_t **lbl_legend;
} adsb_slots_t;

/**
 * Build the round composition of all three views under @p content (data layer)
 * and @p root (scrims and anything that must not dim on STALE).
 *
 * Writes handles through @p s and geometry through @p g. Sets no colours and
 * no text.
 */
void adsb_round_build(lv_obj_t *root, lv_obj_t *content,
                      const adsb_slots_t *s, adsb_geom_t *g);
