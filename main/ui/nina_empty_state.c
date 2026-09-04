/**
 * @file nina_empty_state.c
 * @brief Shared empty/idle-state LVGL component (IDLE-01).
 *
 * All functions run under the display lock held by the CALLER.
 * Do NOT call bsp_display_lock / lvgl_port_lock from within this
 * module -- mirrors the nina_wait_overlay convention.
 *
 * Label pointers are stored in a small heap struct attached to the
 * container via lv_obj_set_user_data().  The struct is allocated
 * through lv_malloc() which routes to PSRAM via lv_mem_psram.c
 * (no MALLOC_CAP_INTERNAL; D-05).
 */

#include "nina_empty_state.h"
#include "nina_dashboard_internal.h"  /* current_theme, screen_size() */
#include "app_config.h"               /* app_config_apply_brightness, app_config_get */
#include "display_defs.h"             /* screen_size() */
#include "net_diag.h"                 /* net_sta_has_ip() */
#include <string.h>
#include <stdio.h>

/* Font exported by the generated lv_font_material_icons_idle.c */
extern const lv_font_t lv_font_material_icons_idle;

/* ── Progress row geometry ─────────────────────────────────────────── *
 *                                                                       *
 * The row spans EMPTY_STATE_ROW_W overall: one segment per unit of the  *
 * total, EMPTY_STATE_SEG_GAP apart, each segment                        *
 * (ROW_W - (n-1)*GAP) / n wide.  420 px fits the round panels' safe box *
 * (720 px disc: 720 - 2*105 = 510 px; 800 px disc: 800 - 2*118 = 564    *
 * px) and the 80%-wide container (576 px on a 720 px panel).            *
 * ───────────────────────────────────────────────────────────────────── */
#define EMPTY_STATE_ROW_W   420
#define EMPTY_STATE_SEG_H   12
#define EMPTY_STATE_SEG_GAP 6
#define EMPTY_STATE_SEG_MAX 16   /* a bigger total is clamped to this, done scaled */

/* The face the title label is set to. Named once so the row gap below cannot
 * drift from the text it clears. */
#define EMPTY_STATE_TITLE_FONT (&lv_font_montserrat_32)

/* Gaps above the row and above the caption (the column's own pad_row is zero;
 * every child carries its own top margin, so these are exact). The row also
 * clears one blank title line: its margin is this figure plus the title font's
 * line height, added at create time. */
#define EMPTY_STATE_ROW_GAP_TOP 18
#define EMPTY_STATE_CAP_GAP_TOP 8
/* The spacing the icon/title/remedy stack had from the container's pad_row. */
#define EMPTY_STATE_TEXT_GAP 12

/* ── Internal widget-pointer struct ────────────────────────────────── */

/**
 * @brief Pointers to the three child labels inside the container.
 *
 * Stored in an lv_malloc'd block attached via lv_obj_set_user_data().
 * This keeps all state local to the container object -- no file-scope
 * static arrays needed.
 */
typedef struct {
    lv_obj_t *icon;    /* Material Symbols label (may be NULL when omitted) */
    lv_obj_t *title;   /* Cause text label */
    lv_obj_t *remedy;  /* Remedy subtitle label (may be NULL) */
    lv_obj_t *row;     /* Segmented progress row (hidden until used) */
    lv_obj_t *segs[EMPTY_STATE_SEG_MAX];  /* Segment blocks, realized once */
    lv_obj_t *bar_lbl; /* "<done> of <total>" caption under the row */
    int       prog_done;    /* Last done value passed to set_progress */
    int       prog_total;   /* Last total passed; 0 = row hidden */
    int       seg_lit;      /* Segments currently lit */
    uint32_t  seg_on;       /* Lit segment color, resolved by apply_colors */
    uint32_t  seg_off;      /* Unlit segment color, resolved by apply_colors */
    uint32_t  icon_color_override; /* Non-zero overrides accent color for icon */
    bool      busy;         /* Icon should pulse while the container is shown */
    bool      pulsing;      /* An icon pulse animation is currently running */
} empty_state_labels_t;

/* Half of the pulse cycle; up + down gives the ~1.2 s round trip. */
#define EMPTY_STATE_PULSE_MS 600

/* ── Static helper forward declarations ────────────────────────────── */

static void apply_colors(empty_state_labels_t *lbls,
                         const theme_t *theme,
                         int color_brightness);
static void icon_opa_anim_cb(void *var, int32_t value);
static void pulse_start(empty_state_labels_t *lbls);
static void pulse_stop(empty_state_labels_t *lbls);
static void recenter(lv_obj_t *cont);
static void paint_segments(empty_state_labels_t *lbls);

/* ── Public API ─────────────────────────────────────────────────────── */

lv_obj_t *nina_empty_state_create(lv_obj_t *parent,
                                  const char *icon_codepoint,
                                  const char *title,
                                  const char *remedy,
                                  uint32_t icon_color_override)
{
    if (!parent) {
        return NULL;
    }

    /* Allocate label-pointer struct through LVGL allocator (PSRAM). */
    empty_state_labels_t *lbls = lv_malloc(sizeof(empty_state_labels_t));
    if (!lbls) {
        return NULL;
    }
    memset(lbls, 0, sizeof(empty_state_labels_t));
    lbls->icon_color_override = icon_color_override;

    /* ── Container ──────────────────────────────────────────────────── */
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);

    /* Width 80% of parent; height sized to content. */
    lv_obj_set_size(cont, LV_PCT(80), LV_SIZE_CONTENT);

    /* Vertical flex column, centered cross-axis, small row gap. */
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* Zero flex gap: every child below sets its own top margin, so the two
     * spacings the progress row needs (18 above it, 8 above the caption) are
     * exact and the text stack keeps the 12 px it always had. */
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* NOT clickable -- must not eat bento tap events (D-01, Pitfall 2). */
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);

    /* Transparent background. */
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);

    /* ── Icon label ─────────────────────────────────────────────────── */
    if (icon_codepoint && icon_codepoint[0]) {
        lv_obj_t *icon = lv_label_create(cont);
        lv_obj_set_style_text_font(icon, &lv_font_material_icons_idle, 0);
        lv_label_set_text(icon, icon_codepoint);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        lbls->icon = icon;
    }

    /* ── Title label ────────────────────────────────────────────────── */
    lv_obj_t *title_lbl = lv_label_create(cont);
    lv_label_set_text(title_lbl, title ? title : "");
    lv_obj_set_style_text_font(title_lbl, EMPTY_STATE_TITLE_FONT, 0);
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_lbl, LV_PCT(100));
    if (lbls->icon) {
        lv_obj_set_style_margin_top(title_lbl, EMPTY_STATE_TEXT_GAP, 0);
    }
    lbls->title = title_lbl;

    /* ── Remedy subtitle label ──────────────────────────────────────── */
    if (remedy && remedy[0]) {
        lv_obj_t *remedy_lbl = lv_label_create(cont);
        lv_label_set_text(remedy_lbl, remedy);
        lv_obj_set_style_text_font(remedy_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_align(remedy_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(remedy_lbl, LV_PCT(100));
        lv_obj_set_style_margin_top(remedy_lbl, EMPTY_STATE_TEXT_GAP, 0);
        lbls->remedy = remedy_lbl;
    }

    /* ── Progress row + caption (hidden until set_progress) ─────────── *
     * Built here rather than lazily so there is no allocation failure    *
     * path at update time; flex layout skips HIDDEN children, so an      *
     * unused row costs nothing in height.  The row is a horizontal flex  *
     * of EMPTY_STATE_SEG_MAX segments realized once; set_progress shows  *
     * the first n and hides the rest.                                    */
    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, EMPTY_STATE_SEG_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, EMPTY_STATE_SEG_GAP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    /* One blank title line plus the fixed gap, so the row sits clear of the
     * "Loading ..." text at whatever size that face is. */
    lv_obj_set_style_margin_top(row,
                                EMPTY_STATE_ROW_GAP_TOP +
                                    lv_font_get_line_height(EMPTY_STATE_TITLE_FONT), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* Same rule as the container: never eat a tap meant for the page beneath
     * (lv_obj is clickable by default; lv_label is not). */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    lbls->row = row;

    for (int i = 0; i < EMPTY_STATE_SEG_MAX; i++) {
        lv_obj_t *seg = lv_obj_create(row);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, EMPTY_STATE_ROW_W / EMPTY_STATE_SEG_MAX, EMPTY_STATE_SEG_H);
        lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
        lbls->segs[i] = seg;
    }

    lv_obj_t *bar_lbl = lv_label_create(cont);
    lv_label_set_text(bar_lbl, "");
    lv_obj_set_style_text_font(bar_lbl, &lv_font_montserrat_34, 0);  /* one Montserrat step above the 32 px title */
    lv_obj_set_style_text_align(bar_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(bar_lbl, EMPTY_STATE_CAP_GAP_TOP, 0);
    lv_obj_add_flag(bar_lbl, LV_OBJ_FLAG_HIDDEN);
    lbls->bar_lbl = bar_lbl;

    /* ── Apply initial theme colors ─────────────────────────────────── */
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        apply_colors(lbls, current_theme, gb);
    }

    /* ── Optical centering at ~42% height (D-03, Finding 3) ─────────── *
     *                                                                    *
     * Align the TOP of the container at y = screen_size() * 42 / 100.   *
     * After layout, lv_obj_get_height() gives the rendered height, so  *
     * shift up by half that to place the visual midpoint at 42%.       *
     * All arithmetic uses integer (P4 FPU is single-precision only;    *
     * integer division is safe here and avoids any float path).        *
     * ────────────────────────────────────────────────────────────────── */
    recenter(cont);

    /* ── Attach label pointers to container via user_data ───────────── */
    lv_obj_set_user_data(cont, lbls);

    /* ── Start hidden; caller calls nina_empty_state_show() ─────────── */
    lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);

    return cont;
}

void nina_empty_state_show(lv_obj_t *cont)
{
    if (!cont) {
        return;
    }

    /* Idempotency guard: already visible -- do NOT restart the fade.    *
     * Callers (update_disconnected_state, nina_spotify_set_idle) invoke *
     * show() every poll cycle; re-triggering the 250 ms fade each cycle *
     * causes visible flicker (BUG-2 fix).                               */
    if (lv_obj_has_flag(cont, LV_OBJ_FLAG_HIDDEN)) {
        /* Set opacity to transparent BEFORE removing HIDDEN, then fade  *
         * in.  Pitfall 3: lv_obj_fade_in() on a HIDDEN object is a      *
         * no-op -- remove the flag first.                               */
        lv_obj_set_style_opa(cont, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_fade_in(cont, 250, 0);
    }

    /* Re-arm the pulse hide() stopped (pulse_start is a no-op when it is
     * already running, so the phase is not reset on repeat calls). */
    empty_state_labels_t *lbls = (empty_state_labels_t *)lv_obj_get_user_data(cont);
    if (lbls && lbls->busy) {
        pulse_start(lbls);
    }
}

void nina_empty_state_hide(lv_obj_t *cont)
{
    if (!cont) {
        return;
    }

    /* Stop the pulse while hidden; show() re-arms it when still busy. */
    empty_state_labels_t *lbls = (empty_state_labels_t *)lv_obj_get_user_data(cont);
    if (lbls) {
        pulse_stop(lbls);
    }

    /* Instant hide -- no fade-out animation prevents reconnect flicker. */
    lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);
}

void nina_empty_state_set_busy(lv_obj_t *cont, bool busy)
{
    if (!cont) {
        return;
    }

    empty_state_labels_t *lbls = (empty_state_labels_t *)lv_obj_get_user_data(cont);
    if (!lbls || lbls->busy == busy) {
        return;
    }
    lbls->busy = busy;

    if (busy) {
        /* The remedy line is the failure story ("Check the ... settings.");
         * it must not sit under a "Connecting to X..." title. */
        if (lbls->remedy) {
            lv_obj_add_flag(lbls->remedy, LV_OBJ_FLAG_HIDDEN);
        }
        /* Only pulse while visible; show() arms it otherwise. */
        if (!lv_obj_has_flag(cont, LV_OBJ_FLAG_HIDDEN)) {
            pulse_start(lbls);
        }
    } else {
        if (lbls->remedy) {
            lv_obj_remove_flag(lbls->remedy, LV_OBJ_FLAG_HIDDEN);
        }
        pulse_stop(lbls);
    }
}

void nina_empty_state_apply_theme(lv_obj_t *cont,
                                  const theme_t *theme,
                                  int color_brightness)
{
    if (!cont || !theme) {
        return;
    }

    empty_state_labels_t *lbls = (empty_state_labels_t *)lv_obj_get_user_data(cont);
    if (!lbls) {
        return;
    }

    apply_colors(lbls, theme, color_brightness);
}

void nina_empty_state_set_title(lv_obj_t *cont, const char *title)
{
    if (!cont || !title) {
        return;
    }

    empty_state_labels_t *lbls = (empty_state_labels_t *)lv_obj_get_user_data(cont);
    if (!lbls || !lbls->title) {
        return;
    }

    /* No-op when text is unchanged -- avoids per-cycle label invalidation  *
     * that forces LVGL to redraw the label and re-layout its parent.       */
    if (strcmp(lv_label_get_text(lbls->title), title) == 0) {
        return;
    }
    lv_label_set_text(lbls->title, title);
}

const char *nina_empty_state_wait_title(const char *normal)
{
    return net_sta_has_ip() ? normal : "Waiting for WiFi";
}

void nina_empty_state_set_progress(lv_obj_t *cont, int done, int total)
{
    if (!cont) {
        return;
    }

    empty_state_labels_t *lbls = (empty_state_labels_t *)lv_obj_get_user_data(cont);
    if (!lbls || !lbls->row || !lbls->bar_lbl) {
        return;
    }

    if (total <= 0) {
        if (lbls->prog_total == 0) {
            return;                       /* already hidden -- nothing to do */
        }
        lbls->prog_total = 0;
        lbls->prog_done  = 0;
        lv_obj_add_flag(lbls->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbls->bar_lbl, LV_OBJ_FLAG_HIDDEN);
        recenter(cont);
        return;
    }

    if (done < 0) {
        done = 0;
    }
    if (done > total) {
        done = total;
    }
    if (lbls->prog_total == total && lbls->prog_done == done) {
        return;                           /* unchanged -- no invalidation */
    }

    bool was_hidden = (lbls->prog_total == 0);
    lbls->prog_total = total;
    lbls->prog_done  = done;

    /* One segment per unit, capped at EMPTY_STATE_SEG_MAX; past the cap each
     * segment stands for several units and the lit count is scaled down with
     * it (floor), so the row still fills as the work completes. */
    int n = (total > EMPTY_STATE_SEG_MAX) ? EMPTY_STATE_SEG_MAX : total;
    int lit = (total > EMPTY_STATE_SEG_MAX) ? (int)(((long)done * n) / total) : done;
    int seg_w = (EMPTY_STATE_ROW_W - (n - 1) * EMPTY_STATE_SEG_GAP) / n;
    if (seg_w < 1) {
        seg_w = 1;
    }
    for (int i = 0; i < EMPTY_STATE_SEG_MAX; i++) {
        if (i < n) {
            lv_obj_set_size(lbls->segs[i], seg_w, EMPTY_STATE_SEG_H);
            lv_obj_remove_flag(lbls->segs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbls->segs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lbls->seg_lit = lit;
    paint_segments(lbls);

    char caption[32];
    int cap_len = snprintf(caption, sizeof(caption), "%d of %d", done, total);
    if (cap_len < 0 || cap_len >= (int)sizeof(caption)) {
        caption[0] = '\0';
    }
    lv_label_set_text(lbls->bar_lbl, caption);

    if (was_hidden) {
        lv_obj_remove_flag(lbls->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbls->bar_lbl, LV_OBJ_FLAG_HIDDEN);
        recenter(cont);
    }
}

/* ── Static helpers ─────────────────────────────────────────────────── */

/**
 * @brief Put the container's visual midpoint back at ~42% of the panel.
 *
 * The container is LV_SIZE_CONTENT, so its height changes whenever a child
 * is shown or hidden (flex layout skips HIDDEN children).  Re-run the same
 * integer arithmetic create() uses.
 */
static void recenter(lv_obj_t *cont)
{
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, screen_size() * 42 / 100);
    lv_obj_update_layout(cont);
    int cont_h = (int)lv_obj_get_height(cont);
    lv_obj_set_y(cont, screen_size() * 42 / 100 - cont_h / 2);
}

/**
 * @brief Colour every segment: lit ones the progress tone, the rest the track.
 *
 * All EMPTY_STATE_SEG_MAX are painted, hidden ones included, so a later
 * total that reveals more segments never shows a stale colour.  Cheap: at
 * most 16 style writes, only on a progress change or a theme change.
 */
static void paint_segments(empty_state_labels_t *lbls)
{
    for (int i = 0; i < EMPTY_STATE_SEG_MAX; i++) {
        if (!lbls->segs[i]) {
            continue;
        }
        uint32_t c = (i < lbls->seg_lit) ? lbls->seg_on : lbls->seg_off;
        lv_obj_set_style_bg_color(lbls->segs[i], lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(lbls->segs[i],
                                (i < lbls->seg_lit) ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

/** Animation callback: drive the icon label's opacity. */
static void icon_opa_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/** Start the 40% <-> 100% infinite pulse. No-op if already running. */
static void pulse_start(empty_state_labels_t *lbls)
{
    if (!lbls->icon || lbls->pulsing) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbls->icon);
    lv_anim_set_exec_cb(&a, icon_opa_anim_cb);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_duration(&a, EMPTY_STATE_PULSE_MS);
    lv_anim_set_reverse_duration(&a, EMPTY_STATE_PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lbls->pulsing = true;
}

/** Stop the pulse and restore full icon opacity. No-op if not running. */
static void pulse_stop(empty_state_labels_t *lbls)
{
    if (!lbls->icon || !lbls->pulsing) {
        return;
    }

    lv_anim_delete(lbls->icon, icon_opa_anim_cb);
    lv_obj_set_style_opa(lbls->icon, LV_OPA_COVER, 0);
    lbls->pulsing = false;
}

/**
 * @brief Apply D-02 theme token mapping to icon, title, and remedy.
 *
 * Token mapping (per Research Finding 1 / PATTERNS.md):
 *   icon   -> theme->header_text_color (accent)
 *   title  -> theme->text_color        (primary readable body tone)
 *   remedy -> theme->label_color       (muted secondary)
 *
 * Each color is scaled by app_config_apply_brightness(color, brightness).
 * icon_color_override (if non-zero) replaces the accent for the icon only.
 */
static void apply_colors(empty_state_labels_t *lbls,
                         const theme_t *theme,
                         int color_brightness)
{
    uint32_t icon_color   = (lbls->icon_color_override != 0)
                            ? lbls->icon_color_override
                            : app_config_apply_brightness(theme->header_text_color,
                                                          color_brightness);
    uint32_t title_color  = app_config_apply_brightness(theme->text_color,
                                                        color_brightness);
    uint32_t remedy_color = app_config_apply_brightness(theme->label_color,
                                                        color_brightness);

    if (lbls->icon) {
        lv_obj_set_style_text_color(lbls->icon,
                                    lv_color_hex(icon_color), 0);
    }
    if (lbls->title) {
        lv_obj_set_style_text_color(lbls->title,
                                    lv_color_hex(title_color), 0);
    }
    if (lbls->remedy) {
        lv_obj_set_style_text_color(lbls->remedy,
                                    lv_color_hex(remedy_color), 0);
    }
    if (lbls->row) {
        lbls->seg_on  = app_config_apply_brightness(theme->progress_color,
                                                    color_brightness);
        lbls->seg_off = remedy_color;
        paint_segments(lbls);
    }
    if (lbls->bar_lbl) {
        lv_obj_set_style_text_color(lbls->bar_lbl,
                                    lv_color_hex(remedy_color), 0);
    }
}
