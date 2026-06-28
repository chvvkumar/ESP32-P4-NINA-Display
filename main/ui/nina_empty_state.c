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
#include "nina_dashboard_internal.h"  /* current_theme, SCREEN_SIZE */
#include "app_config.h"               /* app_config_apply_brightness, app_config_get */
#include "display_defs.h"             /* SCREEN_SIZE */
#include <string.h>
#include <stdio.h>

/* Font exported by the generated lv_font_material_icons_idle.c */
extern const lv_font_t lv_font_material_icons_idle;

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
    uint32_t  icon_color_override; /* Non-zero overrides accent color for icon */
} empty_state_labels_t;

/* ── Static helper forward declarations ────────────────────────────── */

static void apply_colors(empty_state_labels_t *lbls,
                         const theme_t *theme,
                         int color_brightness);

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
    lv_obj_set_style_pad_row(cont, 12, 0);
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
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_lbl, LV_PCT(100));
    lbls->title = title_lbl;

    /* ── Remedy subtitle label ──────────────────────────────────────── */
    if (remedy && remedy[0]) {
        lv_obj_t *remedy_lbl = lv_label_create(cont);
        lv_label_set_text(remedy_lbl, remedy);
        lv_obj_set_style_text_font(remedy_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_align(remedy_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(remedy_lbl, LV_PCT(100));
        lbls->remedy = remedy_lbl;
    }

    /* ── Apply initial theme colors ─────────────────────────────────── */
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        apply_colors(lbls, current_theme, gb);
    }

    /* ── Optical centering at ~42% height (D-03, Finding 3) ─────────── *
     *                                                                    *
     * Align the TOP of the container at y = SCREEN_SIZE * 42 / 100.   *
     * After layout, lv_obj_get_height() gives the rendered height, so  *
     * shift up by half that to place the visual midpoint at 42%.       *
     * All arithmetic uses integer (P4 FPU is single-precision only;    *
     * integer division is safe here and avoids any float path).        *
     * ────────────────────────────────────────────────────────────────── */
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, SCREEN_SIZE * 42 / 100);
    lv_obj_update_layout(cont);
    int cont_h = (int)lv_obj_get_height(cont);
    lv_obj_set_y(cont, SCREEN_SIZE * 42 / 100 - cont_h / 2);

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
    if (!lv_obj_has_flag(cont, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    /* Set opacity to transparent BEFORE removing HIDDEN, then fade in.  *
     * Pitfall 3: lv_obj_fade_in() on a HIDDEN object is a no-op --      *
     * remove the flag first.                                             */
    lv_obj_set_style_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_fade_in(cont, 250, 0);
}

void nina_empty_state_hide(lv_obj_t *cont)
{
    if (!cont) {
        return;
    }

    /* Instant hide -- no fade-out animation prevents reconnect flicker. */
    lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);
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

/* ── Static helpers ─────────────────────────────────────────────────── */

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
}
