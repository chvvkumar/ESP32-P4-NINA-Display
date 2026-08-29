#pragma once

/**
 * @file nina_octoprint_internal.h
 * @brief Layout seam for the OctoPrint page — shared widget library + ops table.
 *
 * FOUR user-selectable layouts (app_config_t::octoprint_layout) share ONE widget
 * library: bento, glass, overlay and letterbox. A layout ARRANGES widgets; it
 * never owns data logic, polling, or the update path. The contract is
 * deliberately one-way:
 *
 *   nina_octoprint.c  owns: create/destroy/update/theme, the client snapshot,
 *                           the image handoff, the empty state, the ops table.
 *   octoprint_layout_*.c own: geometry only (four files; the ops table has
 *                           three further retired slots aliased to Bento). Each
 *                           build() fills in whichever
 *                           handles of octoprint_widgets_t it actually creates
 *                           and leaves the rest NULL. The update path null-checks
 *                           every handle, so a layout may omit anything.
 *
 * A layout must NOT: read octoprint_data_t, call app_config_get(), set widget
 * text from live values, or store file-scope state that the update path needs.
 * Everything it builds is reachable through the widgets struct.
 *
 * All functions here run with the LVGL display lock held by the CALLER
 * (matches nina_json.c / nina_allsky.c). Nothing here takes the lock.
 */

#include "lvgl.h"
#include "themes.h"
#include "ui_helpers.h"     /* current_theme, ui_label, UI_THEME_COLOR */
#include <stdbool.h>
#include <stdint.h>

/* Fonts available to layouts beyond the ones lvgl.h already declares. The bold
 * faces are generated for this page (ASCII plus the degree sign); the rest
 * already existed in the project. */
LV_FONT_DECLARE(lv_font_montserrat_64);
LV_FONT_DECLARE(lv_font_montserrat_bold_22);
LV_FONT_DECLARE(lv_font_montserrat_bold_28);
LV_FONT_DECLARE(lv_font_montserrat_bold_56);

/** Card corner radius shared by every layout (mockup: 24 px bento). */
#define OCTO_CARD_RADIUS  24

/** Cold baseline of the fill-to-target temperature bars, in degrees C. */
#define OCTO_TEMP_COLD_C  25.0f

/** Half-width of the "at temperature" band around the target, in degrees C. */
#define OCTO_TEMP_HOLD_BAND_C  3.0f

/** With the heater off, above this reading the tool is still cooling down. */
#define OCTO_TEMP_COLD_LINE_C  40.0f

/**
 * Derived heating stage of one tool, from its actual/target pair alone (there
 * is no such field in the OctoPrint payload). Drives the OCTO_TEMP_COMPACT
 * value colour; the other variants ignore it.
 *
 *  IDLE     heater off, at or below OCTO_TEMP_COLD_LINE_C.
 *  HEATING  target set and the reading is more than OCTO_TEMP_HOLD_BAND_C below it.
 *  HOLDING  target set and the reading is within the band, or above the target.
 *  COOLING  heater off but the reading is still above OCTO_TEMP_COLD_LINE_C.
 */
typedef enum {
    OCTO_HEAT_IDLE = 0,
    OCTO_HEAT_HEATING,
    OCTO_HEAT_HOLDING,
    OCTO_HEAT_COOLING,
} octo_heat_stage_t;

/* ── Shared styles ─────────────────────────────────────────────────────
 * Re-resolved in one place on a theme change and pushed to every widget with
 * lv_obj_report_style_change(). Layouts attach them and never hard-code a
 * colour, which is what makes all 9 themes work for free. Colours a layout does
 * bake in at build time (gradients, reticles) are covered as well, because
 * octoprint_page_apply_theme() rebuilds the content tree.
 * ──────────────────────────────────────────────────────────────────── */
extern lv_style_t octo_style_label;   /* micro uppercase caption: label_color   */
extern lv_style_t octo_style_value;   /* value text: text_color                 */
extern lv_style_t octo_style_accent;  /* accent text: progress_color            */

/* ── Temperature element ──────────────────────────────────────────────── */

/**
 * Presentation of a temperature element. One handle shape, one update path;
 * the variant only decides what the factory builds.
 *
 *  BAR_GRADIENT  name + "214.9 / 215 °C" on one line, heat-gradient bar
 *                stacked below it with a bright tick at the target.
 *  TILE          compact caption over "214.9/215 C" for a dock metrics tile.
 *  COMPACT       caption ("NOZZLE"/"BED", octo_w_caption style, montserrat_14)
 *                stacked over the CURRENT reading only ("23.8°" — no target,
 *                no "C"), value in lv_font_montserrat_bold_22 carrying
 *                octo_style_value. No bar, no tick. The root is a plain column
 *                sized LV_SIZE_CONTENT both ways, so the layout places it
 *                wherever it likes. The update path overrides the value
 *                colour per octo_heat_stage_t with a LOCAL style, which beats
 *                the shared one.
 */
typedef enum {
    OCTO_TEMP_BAR_GRADIENT = 0,
    OCTO_TEMP_TILE,
    OCTO_TEMP_COMPACT,
} octo_temp_variant_t;

/**
 * One tool's temperature readout. Built by octo_w_temp(), driven by the single
 * update path in nina_octoprint.c. Handles a variant does not build stay NULL
 * and the update path skips them, so both share one code path.
 */
typedef struct {
    lv_obj_t           *root;      /* element container                        */
    lv_obj_t           *lbl_name;  /* "NOZZLE" / "BED"                         */
    lv_obj_t           *lbl_value; /* "214.9 / 215 °C"                         */
    lv_obj_t           *bar;       /* BAR_GRADIENT only: lv_bar, range 0..1000 */
    lv_obj_t           *tick;      /* BAR_GRADIENT only: target marker         */
    octo_temp_variant_t variant;
    bool                hot;       /* true = hot end, false = heated bed       */
} octo_temp_el_t;

/* ── Widget handle set ────────────────────────────────────────────────
 * EVERY updatable element on the page. A layout fills the ones it draws;
 * the update path null-checks each one before touching it.
 * ──────────────────────────────────────────────────────────────────── */
typedef struct {
    /* progress -------------------------------------------------------- */
    lv_obj_t *arc_completion;   /* progress arc, OctoPrint job completion   */
    lv_obj_t *bar_progress;     /* linear alternative to the arc            */
    lv_obj_t *lbl_pct;          /* "61.8"                                   */
    lv_obj_t *lbl_pct_unit;     /* "%"                                      */
    lv_obj_t *lbl_pct_sub;      /* "COMPLETE"                               */

    /* layer (DisplayLayerProgress) ------------------------------------- */
    lv_obj_t *layer_cell;       /* hidden wholesale when !dlp_available     */
    lv_obj_t *lbl_layer_cur;    /* "52"                                     */
    lv_obj_t *lbl_layer_total;  /* "/ 84"                                   */

    /* temperatures ----------------------------------------------------- */
    octo_temp_el_t nozzle;
    octo_temp_el_t bed;
    /* Round family only. One merged "215 / 60" reading for layouts that have
     * chord for ONE temperature cell rather than two. When it is non-NULL the
     * update path writes it; the two octo_temp_el_t elements stay unbuilt. */
    lv_obj_t *lbl_temps;

    /* time ------------------------------------------------------------- */
    lv_obj_t *lbl_elapsed;
    lv_obj_t *lbl_remaining;
    lv_obj_t *finish_cell;      /* hidden wholesale when !dlp_available     */
    lv_obj_t *lbl_finish;       /* "20:46" (DLP estimatedEndTime)           */

    /* identity / state ------------------------------------------------- */
    lv_obj_t *lbl_state;        /* "PRINTING"                               */
    lv_obj_t *state_dot;        /* accent dot beside the state line         */
    /* Round family only (NULL on every square layout). rim_crown is a fixed
     * 40 degree arc at twelve o'clock painted with the state colour, and
     * rim_state_arclabel carries the state TEXT on the top rim instead of
     * lbl_state. The update path drives both only when they are non-NULL. */
    lv_obj_t *rim_crown;
    lv_obj_t *rim_state_arclabel;
    lv_obj_t *conn_chip;        /* link-down indicator; hidden when healthy  */
    lv_obj_t *conn_dot;
    lv_obj_t *lbl_conn;         /* "PRINTER OFFLINE"/error; hidden when OK   */
    lv_obj_t *error_strip;      /* fault chip/strip; hidden when no fault   */
    lv_obj_t *error_dot;
    lv_obj_t *lbl_error;        /* fault text; hidden when no fault         */

    /* image ------------------------------------------------------------ */
    lv_obj_t *img_hero;         /* lv_image bound to the UI-owned RGB565 copy */
    lv_obj_t *img_placeholder;  /* shown while no frame is held             */

    /* readings toggle -------------------------------------------------- */
    /**
     * Full-page transparent container holding everything a layout draws OVER or
     * beside the picture (text, panels, bars, error strip, conn chip). Hidden /
     * shown as one object by the readings toggle. Layouts that want the toggle
     * build it via octo_w_overlay_layer() and parent their widgets to it; Grid
     * leaves it NULL = no toggle.
     */
    lv_obj_t *overlay_layer;
} octoprint_widgets_t;

/* ── Layout ops ───────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    /**
     * True when the layout owns the whole 720x720 screen instead of the usual
     * 688 px content box inside the dashboard's 16 px outer padding. The page
     * root grows and is shifted to negate that padding (same trick as the image,
     * clock and Spotify pages). Only edge-to-edge designs set this; a layout
     * that leaves it false is unaffected.
     */
    bool full_bleed;
    /**
     * true = the staged frame is scaled once to COVER the hero box (aspect
     * kept, excess cropped by the host); false = aspect-FIT (whole frame
     * visible, bands). Only edge-to-edge picture layouts set it.
     */
    bool image_cover;
    /** Build the layout under @p page. @p w is pre-zeroed; fill what you draw. */
    void (*build)(lv_obj_t *page, octoprint_widgets_t *w);
} octoprint_layout_ops_t;

/* Indexed by app_config_t::octoprint_layout, 0..OCTO_LAYOUT_COUNT-1.
 *
 * Slots 1 ("Instrument dial"), 3 ("Large numerals") and 4 ("Layer timeline")
 * are retired and reserved aliases of the Grid layout: those values are still
 * legal in NVS and still pass validate_config, they simply resolve to Grid.
 * The web UI no longer offers them. Do not renumber the remaining slots --
 * stored configs name them by index.
 *
 * Live slots: 0 Grid, 2 Glass ("Immersive image"), 5 "Floating overlay",
 * 6 "Letterbox". */
#define OCTO_LAYOUT_COUNT 7
extern const octoprint_layout_ops_t octoprint_layout_bento;
extern const octoprint_layout_ops_t octoprint_layout_glass;
extern const octoprint_layout_ops_t octoprint_layout_overlay;
extern const octoprint_layout_ops_t octoprint_layout_letterbox;

/* Round-family builders. Defined in ui/octoprint_layout_bento_round.c and
 * ui/octoprint_layout_glass_round.c, which are compiled into nina_round_srcs
 * only (phase 2 sub-plan D). Declared here so the round dispatch in
 * nina_octoprint.c has one place to name them. Grid is the inscribed pick and
 * Immersive the radial one; layouts 5 and 6 have no round composition and
 * resolve to Immersive (spec addendum section 7). */
extern const octoprint_layout_ops_t octoprint_layout_bento_round;
extern const octoprint_layout_ops_t octoprint_layout_glass_round;

/* ── Shared widget factories (implemented in nina_octoprint.c) ─────────
 * These are the whole widget library. Layouts compose them; nothing else.
 * ──────────────────────────────────────────────────────────────────── */

/** Theme colour tokens. Layouts ask for these, never for a literal hex. */
typedef enum {
    OCTO_COL_TEXT,     /* primary value text     */
    OCTO_COL_LABEL,    /* muted caption / resting state */
    OCTO_COL_ACCENT,   /* progress, active state */
    OCTO_COL_BORDER,   /* card borders, off segments */
    OCTO_COL_CARDBG,   /* card fill              */
    OCTO_COL_BG,       /* page ground           */
    OCTO_COL_HOT,      /* hot end               */
    OCTO_COL_ALERT,    /* fault                 */
} octo_color_id_t;

/**
 * Resolve a theme token to an RGB value already dimmed by the configured
 * colour brightness. This is the ONLY colour source a layout may use — it is
 * what keeps layouts free of app_config and correct across all nine themes.
 */
uint32_t octo_color(octo_color_id_t id);

/** Bento card: bento_bg, 1 px bento_border, OCTO_CARD_RADIUS, clipped, no scroll. */
lv_obj_t *octo_w_card(lv_obj_t *parent);

/** Plain container with no style at all (rows/columns/spacers). */
lv_obj_t *octo_w_row(lv_obj_t *parent, bool horizontal, int gap);

/** Label carrying one of the shared styles. @p style may be NULL. */
lv_obj_t *octo_w_label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, lv_style_t *style);

/** Uppercase micro caption ("PROGRESS", "LAYER"): font 14, letter-space 2. */
lv_obj_t *octo_w_caption(lv_obj_t *parent, const char *text);

/** Pixel width of @p sample in @p font (letter_space 0). For fixed-width labels
 * that must not jiggle. */
int32_t octo_text_width(const lv_font_t *font, const char *sample);

/**
 * Temperature element, in one of the presentations of octo_temp_variant_t
 * (BAR_GRADIENT, TILE or COMPACT).
 *
 * Only BAR_GRADIENT builds (and therefore drives) a fill. Its colours are THEME
 * TOKENS, not literals: the gradient runs OCTO_COL_ACCENT -> OCTO_COL_ALERT for
 * a hot end and OCTO_COL_ACCENT -> OCTO_COL_HOT for the bed, which is what lets
 * apply_theme recolour it after the layout was built and makes all 9 themes
 * work. The bar spans a cold OCTO_TEMP_COLD_C baseline to target + 10 %
 * headroom, with a bright tick at the target so an overshoot reads as one.
 *
 * @param variant Presentation; see octo_temp_variant_t
 * @param hot     true = hot end, false = heated bed
 * @param out     Handles written here; pass &w->nozzle or &w->bed
 * @return        The element root (already added to @p parent)
 */
lv_obj_t *octo_w_temp(lv_obj_t *parent, const char *name,
                      octo_temp_variant_t variant, bool hot,
                      octo_temp_el_t *out);

/**
 * Image hero: a BARE frame. Letterboxed (LV_IMAGE_ALIGN_CONTAIN) view of the
 * client's decoded RGB565 frame plus a centred placeholder shown while no frame
 * is held -- no source tag, no caption, no corner marks. A layout that wants a
 * label over the image draws its own.
 * Sets w->img_hero and w->img_placeholder.
 */
lv_obj_t *octo_w_image_hero(lv_obj_t *parent, octoprint_widgets_t *w);

/**
 * Readings layer: a full-page, fully transparent, FLOATING, LV_LAYOUT_NONE
 * container at (0,0) sized 100%x100% of @p page. Parent to it everything the
 * layout draws over or beside the picture; leave the image hero host parented
 * to @p page so it stays visible when the readings are hidden. Sets
 * w->overlay_layer; the spine applies the current readings visibility right
 * after build() returns, so a layout never has to.
 *
 * A layout that does not call this leaves w->overlay_layer NULL, and the page
 * tap toggle is then a no-op (Grid).
 *
 * TAP PASS-THROUGH. The layer itself is NOT clickable, so a tap over it hit-
 * tests through to the page root, which owns the toggle handler. The library
 * factories (octo_w_card, octo_w_row) already drop LV_OBJ_FLAG_CLICKABLE, so
 * anything built from them passes taps through as well. A layout that creates a
 * container with a raw lv_obj_create() MUST remove LV_OBJ_FLAG_CLICKABLE from
 * it (lv_obj is clickable by default; lv_label and lv_image are not), or taps
 * landing on that container will not reach the page and will not toggle.
 * LV_OBJ_FLAG_GESTURE_BUBBLE is left at its default so page swipes still reach
 * the dashboard.
 *
 * @return The layer (already added to @p page).
 */
lv_obj_t *octo_w_overlay_layer(lv_obj_t *page, octoprint_widgets_t *w);

/**
 * Fault strip: an empty chip, created HIDDEN. The update path shows it (with
 * text and alert colour) only while there is a fault or the printer is offline,
 * and hides it again otherwise. Layouts lay it out as usual and must not set its
 * visibility or a resting placeholder text.
 */
lv_obj_t *octo_w_status_strip(lv_obj_t *parent, octoprint_widgets_t *w);

/** Connection indicator chip. Sets w->conn_chip / conn_dot / lbl_conn. */
lv_obj_t *octo_w_conn_chip(lv_obj_t *parent, octoprint_widgets_t *w);

/** Caption + big value tile (elapsed / remaining / finish). */
lv_obj_t *octo_w_time_tile(lv_obj_t *parent, const char *caption,
                           const lv_font_t *font, lv_obj_t **out_value);

/** Linear progress bar primitive (range 0..1000). Sets w->bar_progress. */
lv_obj_t *octo_w_progress_bar(lv_obj_t *parent, octoprint_widgets_t *w);

/**
 * Progress arc primitive (range 0..1000, 135 deg start, 270 deg sweep).
 * Sets w->arc_completion.
 */
lv_obj_t *octo_w_progress_arc(lv_obj_t *parent, int size, int arc_width,
                              octoprint_widgets_t *w);

/** State line: accent dot + uppercase state label. Sets state_dot / lbl_state. */
lv_obj_t *octo_w_state_line(lv_obj_t *parent, octoprint_widgets_t *w);
