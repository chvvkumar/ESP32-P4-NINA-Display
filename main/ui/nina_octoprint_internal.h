#pragma once

/**
 * @file nina_octoprint_internal.h
 * @brief Layout seam for the OctoPrint page — shared widget library + ops table.
 *
 * TWO user-selectable layouts (app_config_t::octoprint_layout) share ONE widget
 * library. A layout ARRANGES widgets; it never owns data logic, polling, or the
 * update path. The contract is deliberately one-way:
 *
 *   nina_octoprint.c  owns: create/destroy/update/theme, the client snapshot,
 *                           the image handoff, the empty state, the ops table.
 *   octoprint_layout_*.c own: geometry only (two files; the ops table has three
 *                           further retired slots aliased to Bento). Each
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

/* Fonts available to layouts beyond the ones lvgl.h already declares. All of
 * these already exist in the project — no new font faces were added for this
 * page. */
LV_FONT_DECLARE(lv_font_montserrat_64);

/** Card corner radius shared by every layout (mockup: 24 px bento). */
#define OCTO_CARD_RADIUS  24

/** Cold baseline of the fill-to-target temperature bars, in degrees C. */
#define OCTO_TEMP_COLD_C  25.0f

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
 */
typedef enum {
    OCTO_TEMP_BAR_GRADIENT = 0,
    OCTO_TEMP_TILE,
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

    /* time ------------------------------------------------------------- */
    lv_obj_t *lbl_elapsed;
    lv_obj_t *lbl_remaining;
    lv_obj_t *finish_cell;      /* hidden wholesale when !dlp_available     */
    lv_obj_t *lbl_finish;       /* "20:46" (DLP estimatedEndTime)           */

    /* identity / state ------------------------------------------------- */
    lv_obj_t *lbl_state;        /* "PRINTING"                               */
    lv_obj_t *state_dot;        /* accent dot beside the state line         */
    lv_obj_t *conn_chip;        /* link-down indicator; hidden when healthy  */
    lv_obj_t *conn_dot;
    lv_obj_t *lbl_conn;         /* "PRINTER OFFLINE"/error; hidden when OK   */
    lv_obj_t *error_strip;      /* fault chip/strip; hidden when no fault   */
    lv_obj_t *error_dot;
    lv_obj_t *lbl_error;        /* fault text; hidden when no fault         */

    /* image ------------------------------------------------------------ */
    lv_obj_t *img_hero;         /* lv_image bound to the UI-owned RGB565 copy */
    lv_obj_t *img_placeholder;  /* shown while no frame is held             */
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
    /** Build the layout under @p page. @p w is pre-zeroed; fill what you draw. */
    void (*build)(lv_obj_t *page, octoprint_widgets_t *w);
} octoprint_layout_ops_t;

/* Indexed by app_config_t::octoprint_layout, 0..OCTO_LAYOUT_COUNT-1.
 *
 * Slots 1 ("Instrument dial"), 3 ("Large numerals") and 4 ("Layer timeline")
 * are retired and reserved aliases of the Bento layout: those values are still
 * legal in NVS and still pass validate_config, they simply resolve to Bento.
 * The web UI no longer offers them. Do not renumber the remaining slots --
 * stored configs name them by index. */
#define OCTO_LAYOUT_COUNT 5
extern const octoprint_layout_ops_t octoprint_layout_bento;
extern const octoprint_layout_ops_t octoprint_layout_glass;

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

/**
 * Temperature element, in one of the two presentations of octo_temp_variant_t
 * (BAR_GRADIENT or TILE).
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
