/**
 * @file nina_summary.c
 * @brief Summary page — adaptive cards showing connected NINA instances at a glance.
 *
 * Displays one card per CONNECTED NINA instance with: instance name, filter badge,
 * target name, progress bar with percentage, sequence info (1-card mode),
 * and key stats (RMS, HFR, time to flip).
 *
 * Cards expand to fill the available 688×688 area using flex_grow.
 * 3-tier font scaling adapts to 1, 2, or 3 visible cards.
 * An empty-state message is shown when no instances are connected.
 */

#include "nina_summary.h"
#include "nina_dashboard_internal.h"
#include "nina_dashboard.h"
#include "nina_connection.h"
#include "app_config.h"
#include "themes.h"

#include <stdio.h>
#include <string.h>

/* ── Change-detection helpers ──────────────────────────────────────── */
/* Set label text only if it actually changed (avoids marking objects dirty) */
static inline void set_label_if_changed(lv_obj_t *label, const char *text) {
    const char *cur = lv_label_get_text(label);
    if (strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

/* Set label text (printf-style) only if it actually changed */
#define SET_LABEL_FMT_IF_CHANGED(label, bufsize, fmt, ...) do { \
    char _buf[bufsize]; \
    snprintf(_buf, sizeof(_buf), fmt, __VA_ARGS__); \
    const char *_cur = lv_label_get_text(label); \
    if (strcmp(_cur, _buf) != 0) lv_label_set_text(label, _buf); \
} while (0)

/* Set bar value only if it actually changed */
static inline void set_bar_if_changed(lv_obj_t *bar, int32_t val, lv_anim_enable_t anim) {
    if (lv_bar_get_value(bar) != val) lv_bar_set_value(bar, val, anim);
}

/* ── Safety icon glyphs (Material Symbols, UTF-8) ─────────────────── */
#define ICON_VERIFIED_USER  "\xee\xa3\xa8"  /* U+E8E8 — shield + check */
#define ICON_GPP_BAD        "\xef\x80\x92"  /* U+F012 — shield + X     */
#define ICON_GPP_MAYBE      "\xef\x80\x94"  /* U+F014 — shield + ?     */

/* ── Layout Constants ──────────────────────────────────────────────── */
#define CARD_RADIUS      20
#define CARD_GAP         16
#define STAT_COLS         3

/* ── 3-tier font/layout presets ───────────────────────────────────── */
typedef struct {
    const lv_font_t *name;
    const lv_font_t *filter;
    const lv_font_t *target;
    const lv_font_t *stat_label;
    const lv_font_t *stat_value;
    const lv_font_t *pct_label;
    const lv_font_t *seq_label;
    const lv_font_t *seq_value;
    int bar_height;
    int card_pad;
    int card_row_gap;
    int stat_pad_row;
} card_layout_preset_t;

static const card_layout_preset_t layout_presets[3] = {
    /* 1 card visible — maximum size */
    {
        .name       = &lv_font_montserrat_36,
        .filter     = &lv_font_montserrat_28,
        .target     = &lv_font_montserrat_48,
        .stat_label = &lv_font_montserrat_22,
        .stat_value = &lv_font_montserrat_48,
        .pct_label  = &lv_font_montserrat_28,
        .seq_label  = &lv_font_montserrat_18,
        .seq_value  = &lv_font_montserrat_28,
        .bar_height = 20,
        .card_pad   = 28,
        .card_row_gap = 16,
        .stat_pad_row = 12,
    },
    /* 2 cards visible — medium */
    {
        .name       = &lv_font_montserrat_28,
        .filter     = &lv_font_montserrat_24,
        .target     = &lv_font_montserrat_32,
        .stat_label = &lv_font_montserrat_18,
        .stat_value = &lv_font_montserrat_36,
        .pct_label  = &lv_font_montserrat_22,
        .seq_label  = &lv_font_montserrat_14,
        .seq_value  = &lv_font_montserrat_22,
        .bar_height = 12,
        .card_pad   = 22,
        .card_row_gap = 10,
        .stat_pad_row = 8,
    },
    /* 3 cards visible — compact */
    {
        .name       = &lv_font_montserrat_20,
        .filter     = &lv_font_montserrat_18,
        .target     = &lv_font_montserrat_24,
        .stat_label = &lv_font_montserrat_14,
        .stat_value = &lv_font_montserrat_28,
        .pct_label  = &lv_font_montserrat_18,
        .seq_label  = &lv_font_montserrat_12,
        .seq_value  = &lv_font_montserrat_14,
        .bar_height = 6,
        .card_pad   = 16,
        .card_row_gap = 6,
        .stat_pad_row = 4,
    },
};

/* ── Per-card widget references ────────────────────────────────────── */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *lbl_name;
    lv_obj_t *lbl_filter;
    lv_obj_t *filter_box;
    lv_obj_t *lbl_target;
    lv_obj_t *bar_progress;
    lv_obj_t *lbl_pct;          /* progress percentage label */
    lv_obj_t *seq_row;          /* sequence info row (visible in 1-2 card mode) */
    lv_obj_t *lbl_seq_title;    /* "SEQUENCE" label */
    lv_obj_t *lbl_seq_name;
    lv_obj_t *lbl_step_title;   /* "STEP" label */
    lv_obj_t *lbl_seq_step;
    lv_obj_t *stats_row;
    lv_obj_t *lbl_rms_label;
    lv_obj_t *lbl_rms_val;
    lv_obj_t *lbl_hfr_label;
    lv_obj_t *lbl_hfr_val;
    lv_obj_t *lbl_flip_label;
    lv_obj_t *lbl_flip_val;
    lv_obj_t *detail_row;       /* exposure detail line (visible in 1-card mode) */
    lv_obj_t *lbl_detail;
    lv_obj_t *lbl_safety;       /* safety monitor icon (floating, bottom-left) */
    int instance_index;         /* which NINA instance this card represents */
} summary_card_t;

/* ── Module state ──────────────────────────────────────────────────── */
static lv_obj_t *sum_page = NULL;
static summary_card_t cards[MAX_NINA_INSTANCES];
static int card_count = 0;
static int prev_visible_count = -1;

/* Empty state widgets */
static lv_obj_t *empty_cont = NULL;
static lv_obj_t *empty_msg = NULL;
static lv_obj_t *empty_sub = NULL;

/* Glass card style — semi-transparent with subtle border */
static lv_style_t style_glass_card;
static bool styles_initialized = false;

/* Red Night theme forces all colors to the red palette */
static bool theme_forces_colors(void) {
    return current_theme && strcmp(current_theme->name, "Red Night") == 0;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static void summary_card_click_cb(lv_event_t *e) {
    int instance_index = (int)(intptr_t)lv_event_get_user_data(e);
    /* Map instance index to page index (returns 1-based, or -1 if disabled) */
    int page = nina_dashboard_instance_to_page(instance_index);
    if (page > 0)
        nina_dashboard_show_page(page, 0);
}

static void init_glass_styles(void) {
    if (styles_initialized) return;
    styles_initialized = true;

    lv_style_init(&style_glass_card);
    lv_style_set_radius(&style_glass_card, CARD_RADIUS);
    lv_style_set_border_width(&style_glass_card, 1);

    /* Colors are applied in apply_glass_theme() */
}

static void apply_glass_theme(void) {
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;

    lv_style_set_bg_color(&style_glass_card,
        lv_color_hex(app_config_apply_brightness(current_theme->bento_bg, gb)));
    lv_style_set_bg_opa(&style_glass_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_glass_card,
        lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)));
    lv_style_set_border_opa(&style_glass_card, LV_OPA_30);
}

/**
 * @brief Build a single stat column (label above, value below).
 */
static void create_stat_block(lv_obj_t *parent,
                              const char *label_text,
                              lv_obj_t **out_label,
                              lv_obj_t **out_value)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_size(block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(block, 4, 0);

    /* Label (small, dim) */
    lv_obj_t *lbl = lv_label_create(block);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    /* Value (large) */
    lv_obj_t *val = lv_label_create(block);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);

    *out_label = lbl;
    *out_value = val;
}

/**
 * @brief Build one glassmorphism instance card with all widgets.
 *        Some widgets (seq_row, detail_row) are hidden by default
 *        and shown only when visible_count is low enough.
 */
static void create_card(summary_card_t *sc, lv_obj_t *parent, int instance_index) {
    memset(sc, 0, sizeof(summary_card_t));
    sc->instance_index = instance_index;

    /* ── Card container ── */
    sc->card = lv_obj_create(parent);
    lv_obj_remove_style_all(sc->card);
    lv_obj_add_style(sc->card, &style_glass_card, 0);
    lv_obj_set_width(sc->card, LV_PCT(100));
    lv_obj_set_flex_grow(sc->card, 1);
    lv_obj_set_flex_flow(sc->card, LV_FLEX_FLOW_COLUMN);
    /* START packs header/target/bar at top; stats_row with flex_grow=1
     * absorbs remaining vertical space, centering stats in the middle */
    lv_obj_set_flex_align(sc->card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(sc->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(sc->card, 16, 0);
    lv_obj_set_style_pad_row(sc->card, 6, 0);

    /* Make card clickable for navigation */
    lv_obj_add_flag(sc->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sc->card, summary_card_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)instance_index);

    /* ── Instance name (full width, truncated with dots) ── */
    sc->lbl_name = lv_label_create(sc->card);
    lv_obj_set_width(sc->lbl_name, LV_PCT(100));
    lv_label_set_long_mode(sc->lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sc->lbl_name, &lv_font_montserrat_20, 0);
    {
        const char *url = app_config_get_instance_url(instance_index);
        char host[64] = {0};
        extract_host_from_url(url, host, sizeof(host));
        lv_label_set_text(sc->lbl_name, host[0] ? host : "N.I.N.A.");
    }

    /* ── Target name ── */
    sc->lbl_target = lv_label_create(sc->card);
    lv_obj_set_width(sc->lbl_target, LV_PCT(100));
    lv_obj_set_style_text_align(sc->lbl_target, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(sc->lbl_target, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_24, 0);
    lv_label_set_text(sc->lbl_target, "----");

    /* ── Progress bar row: filter | bar | pct ── */
    lv_obj_t *bar_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(bar_row);
    lv_obj_set_width(bar_row, LV_PCT(100));
    lv_obj_set_height(bar_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar_row, 10, 0);

    /* Filter badge (left of progress bar) */
    sc->filter_box = lv_obj_create(bar_row);
    lv_obj_remove_style_all(sc->filter_box);
    lv_obj_set_size(sc->filter_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sc->filter_box, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(sc->filter_box, lv_color_black(), 0);
    lv_obj_set_style_radius(sc->filter_box, 6, 0);
    lv_obj_set_style_pad_hor(sc->filter_box, 8, 0);
    lv_obj_set_style_pad_ver(sc->filter_box, 4, 0);

    sc->lbl_filter = lv_label_create(sc->filter_box);
    lv_obj_set_style_text_font(sc->lbl_filter, &lv_font_montserrat_14, 0);
    lv_label_set_text(sc->lbl_filter, "--");

    sc->bar_progress = lv_bar_create(bar_row);
    lv_obj_set_flex_grow(sc->bar_progress, 1);
    lv_obj_set_height(sc->bar_progress, 6);
    lv_bar_set_range(sc->bar_progress, 0, 100);
    lv_bar_set_value(sc->bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(sc->bar_progress, 3, 0);
    lv_obj_set_style_radius(sc->bar_progress, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sc->bar_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(sc->bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);

    sc->lbl_pct = lv_label_create(bar_row);
    lv_obj_set_style_text_font(sc->lbl_pct, &lv_font_montserrat_14, 0);
    lv_label_set_text(sc->lbl_pct, "");

    /* ── Sequence info row (shown in 1-2 card mode) ── */
    sc->seq_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(sc->seq_row);
    lv_obj_set_width(sc->seq_row, LV_PCT(100));
    lv_obj_set_height(sc->seq_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc->seq_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sc->seq_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN); /* hidden by default */

    lv_obj_t *seq_left = lv_obj_create(sc->seq_row);
    lv_obj_remove_style_all(seq_left);
    lv_obj_clear_flag(seq_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(seq_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_left, LV_FLEX_FLOW_COLUMN);

    sc->lbl_seq_title = lv_label_create(seq_left);
    lv_label_set_text(sc->lbl_seq_title, "SEQUENCE");
    lv_obj_set_style_text_font(sc->lbl_seq_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sc->lbl_seq_title, 1, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(sc->lbl_seq_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    sc->lbl_seq_name = lv_label_create(seq_left);
    lv_obj_set_style_text_font(sc->lbl_seq_name, &lv_font_montserrat_18, 0);
    lv_label_set_text(sc->lbl_seq_name, "----");

    lv_obj_t *seq_right = lv_obj_create(sc->seq_row);
    lv_obj_remove_style_all(seq_right);
    lv_obj_clear_flag(seq_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(seq_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(seq_right, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    sc->lbl_step_title = lv_label_create(seq_right);
    lv_label_set_text(sc->lbl_step_title, "STEP");
    lv_obj_set_style_text_font(sc->lbl_step_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sc->lbl_step_title, 1, 0);
    lv_obj_set_style_text_align(sc->lbl_step_title, LV_TEXT_ALIGN_RIGHT, 0);
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(sc->lbl_step_title,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }

    sc->lbl_seq_step = lv_label_create(seq_right);
    lv_obj_set_style_text_font(sc->lbl_seq_step, &lv_font_montserrat_18, 0);
    lv_label_set_text(sc->lbl_seq_step, "----");

    /* ── Stats row: RMS | HFR | FLIP ── */
    sc->stats_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(sc->stats_row);
    lv_obj_set_width(sc->stats_row, LV_PCT(100));
    lv_obj_set_height(sc->stats_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sc->stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(sc->stats_row, 1);
    lv_obj_set_flex_align(sc->stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Safety icon column (first in stats row) */
    {
        extern const lv_font_t lv_font_material_safety;
        lv_obj_t *safety_block = lv_obj_create(sc->stats_row);
        lv_obj_remove_style_all(safety_block);
        lv_obj_set_size(safety_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(safety_block, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(safety_block, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        sc->lbl_safety = lv_label_create(safety_block);
        lv_obj_set_style_text_font(sc->lbl_safety, &lv_font_material_safety, 0);
        lv_obj_set_style_text_color(sc->lbl_safety, lv_color_hex(0x999999), 0);
        lv_label_set_text(sc->lbl_safety, ICON_GPP_MAYBE);
        lv_obj_add_flag(sc->lbl_safety, LV_OBJ_FLAG_HIDDEN);
    }

    create_stat_block(sc->stats_row, "RMS",  &sc->lbl_rms_label,  &sc->lbl_rms_val);
    create_stat_block(sc->stats_row, "HFR",  &sc->lbl_hfr_label,  &sc->lbl_hfr_val);
    create_stat_block(sc->stats_row, "FLIP", &sc->lbl_flip_label, &sc->lbl_flip_val);

    /* ── Exposure detail line (shown in 1-card mode only) ── */
    sc->detail_row = lv_obj_create(sc->card);
    lv_obj_remove_style_all(sc->detail_row);
    lv_obj_set_width(sc->detail_row, LV_PCT(100));
    lv_obj_set_height(sc->detail_row, LV_SIZE_CONTENT);
    lv_obj_add_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN); /* hidden by default */

    sc->lbl_detail = lv_label_create(sc->detail_row);
    lv_obj_set_width(sc->lbl_detail, LV_PCT(100));
    lv_obj_set_style_text_font(sc->lbl_detail, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(sc->lbl_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(sc->lbl_detail, "");
    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(sc->lbl_detail,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

}

/**
 * @brief Create the empty-state container (shown when no instances connected).
 */
static void create_empty_state(lv_obj_t *parent) {
    empty_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(empty_cont);
    lv_obj_set_size(empty_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(empty_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(empty_cont,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(empty_cont, 16, 0);
    lv_obj_clear_flag(empty_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(empty_cont, LV_OBJ_FLAG_HIDDEN);

    empty_msg = lv_label_create(empty_cont);
    lv_label_set_text(empty_msg, "No Connections");
    lv_obj_set_style_text_font(empty_msg, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(empty_msg, LV_TEXT_ALIGN_CENTER, 0);

    empty_sub = lv_label_create(empty_cont);
    lv_label_set_text(empty_sub, "Waiting for N.I.N.A. instances...");
    lv_obj_set_style_text_font(empty_sub, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(empty_sub, LV_TEXT_ALIGN_CENTER, 0);

    if (current_theme) {
        int gb = app_config_get()->color_brightness;
        lv_obj_set_style_text_color(empty_msg,
            lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
        lv_obj_set_style_text_color(empty_sub,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
}

/* ── Page Creation ─────────────────────────────────────────────────── */

lv_obj_t *summary_page_create(lv_obj_t *parent, int instance_count) {
    init_glass_styles();
    apply_glass_theme();

    sum_page = lv_obj_create(parent);
    lv_obj_remove_style_all(sum_page);
    lv_obj_set_size(sum_page, SCREEN_SIZE - 2 * OUTER_PADDING,
                              SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_flex_flow(sum_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sum_page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sum_page, CARD_GAP, 0);
    lv_obj_clear_flag(sum_page, LV_OBJ_FLAG_SCROLLABLE);

    card_count = instance_count;
    if (card_count > MAX_NINA_INSTANCES) card_count = MAX_NINA_INSTANCES;

    for (int i = 0; i < card_count; i++) {
        create_card(&cards[i], sum_page, i);
        /* Start hidden — will be shown by summary_page_update for connected instances */
        lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
    }

    /* Empty state — shown when no instances are connected */
    create_empty_state(sum_page);

    prev_visible_count = -1;

    return sum_page;
}

/* ── Layout Update ─────────────────────────────────────────────────── */

static void update_card_layout(summary_card_t *sc, int visible_count) {
    int idx = (visible_count <= 1) ? 0 : (visible_count == 2) ? 1 : 2;
    const card_layout_preset_t *p = &layout_presets[idx];

    /* Card flex strategy: 1-card uses SPACE_BETWEEN to pin header at top
     * and detail at bottom, spreading items across the full height.
     * 2-3 cards pack to top with stats_row absorbing remaining space. */
    if (visible_count <= 1) {
        lv_obj_set_flex_align(sc->card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_flex_grow(sc->stats_row, 0);
    } else {
        lv_obj_set_flex_align(sc->card, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_flex_grow(sc->stats_row, 1);
    }

    /* Card padding and spacing */
    lv_obj_set_style_pad_all(sc->card, p->card_pad, 0);
    lv_obj_set_style_pad_row(sc->card, p->card_row_gap, 0);

    /* Instance name */
    lv_obj_set_style_text_font(sc->lbl_name, p->name, 0);

    /* Filter badge */
    lv_obj_set_style_text_font(sc->lbl_filter, p->filter, 0);

    /* Target name */
    lv_obj_set_style_text_font(sc->lbl_target, p->target, 0);

    /* Progress bar height */
    lv_obj_set_height(sc->bar_progress, p->bar_height);
    lv_obj_set_style_radius(sc->bar_progress, p->bar_height / 2, 0);
    lv_obj_set_style_radius(sc->bar_progress, p->bar_height / 2, LV_PART_INDICATOR);

    /* Progress percentage label */
    lv_obj_set_style_text_font(sc->lbl_pct, p->pct_label, 0);

    /* Sequence row: shown in 1-2 card mode, hidden in 3-card mode */
    if (visible_count <= 2) {
        lv_obj_clear_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(sc->lbl_seq_title, p->seq_label, 0);
        lv_obj_set_style_text_font(sc->lbl_seq_name, p->seq_value, 0);
        lv_obj_set_style_text_font(sc->lbl_step_title, p->seq_label, 0);
        lv_obj_set_style_text_font(sc->lbl_seq_step, p->seq_value, 0);
    } else {
        lv_obj_add_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN);
    }

    /* Stats (Labels and Values) */
    lv_obj_t *labels[] = { sc->lbl_rms_label, sc->lbl_hfr_label, sc->lbl_flip_label };
    lv_obj_t *values[] = { sc->lbl_rms_val, sc->lbl_hfr_val, sc->lbl_flip_val };

    for (int i = 0; i < 3; i++) {
        if (labels[i]) {
            lv_obj_set_style_text_font(labels[i], p->stat_label, 0);
            /* Update stat block padding */
            lv_obj_t *block = lv_obj_get_parent(labels[i]);
            if (block) lv_obj_set_style_pad_row(block, p->stat_pad_row, 0);
        }
        if (values[i]) lv_obj_set_style_text_font(values[i], p->stat_value, 0);
    }

    /* Detail row: shown only in 1-card mode */
    if (visible_count <= 1) {
        lv_obj_clear_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(sc->lbl_detail, p->pct_label, 0);
    } else {
        lv_obj_add_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Card Transition Animations ────────────────────────────────────── */

#define ANIM_DURATION_MS  400

static void anim_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void anim_translate_y_cb(void *obj, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

static void anim_translate_y_done_cb(lv_anim_t *a) {
    lv_obj_set_style_translate_y((lv_obj_t *)a->var, 0, 0);
}

/**
 * @brief Fade-in + slide-up entrance for a newly appearing card.
 */
static void animate_card_in(summary_card_t *sc) {
    lv_obj_set_style_opa(sc->card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(sc->card, 40, 0);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, sc->card);
    lv_anim_set_values(&a_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_opa, ANIM_DURATION_MS);
    lv_anim_set_exec_cb(&a_opa, anim_opa_cb);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_start(&a_opa);

    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, sc->card);
    lv_anim_set_values(&a_y, 40, 0);
    lv_anim_set_duration(&a_y, ANIM_DURATION_MS);
    lv_anim_set_exec_cb(&a_y, anim_translate_y_cb);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_out);
    lv_anim_start(&a_y);
}

/**
 * @brief Smoothly glide a card from its old position to its new position.
 * Uses translate_y to offset from the flex-assigned position, then animate to 0.
 */
static void animate_card_move(summary_card_t *sc, int32_t dy) {
    lv_obj_set_style_translate_y(sc->card, dy, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sc->card);
    lv_anim_set_values(&a, dy, 0);
    lv_anim_set_duration(&a, ANIM_DURATION_MS);
    lv_anim_set_exec_cb(&a, anim_translate_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, anim_translate_y_done_cb);
    lv_anim_start(&a);
}

/* ── Data Update ───────────────────────────────────────────────────── */

void summary_page_update(const nina_client_t *instances, int count) {
    if (!sum_page) return;

    int gb = app_config_get()->color_brightness;

    /* Count connected instances (use centralized connection state) */
    int connected_count = nina_connection_connected_count();

    /* Empty state: show message when nothing is connected */
    if (connected_count == 0) {
        for (int i = 0; i < card_count; i++) {
            lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
        }
        if (empty_cont) {
            lv_obj_clear_flag(empty_cont, LV_OBJ_FLAG_HIDDEN);
        }
        prev_visible_count = 0;
        return;
    }

    /* Hide empty state, show cards */
    if (empty_cont) {
        lv_obj_add_flag(empty_cont, LV_OBJ_FLAG_HIDDEN);
    }

    bool layout_changed = (connected_count != prev_visible_count);

    if (layout_changed) {
        /* ── FLIP animation: First, Last, Invert, Play ───────────── */

        /* FIRST: snapshot current visibility and Y positions */
        bool was_visible[MAX_NINA_INSTANCES];
        int32_t old_y[MAX_NINA_INSTANCES];
        for (int i = 0; i < card_count; i++) {
            was_visible[i] = !lv_obj_has_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            old_y[i] = was_visible[i] ? lv_obj_get_y(cards[i].card) : 0;
        }

        /* LAST: apply show/hide and layout preset changes */
        for (int i = 0; i < card_count && i < count; i++) {
            if (nina_connection_is_connected(i)) {
                lv_obj_clear_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
                update_card_layout(&cards[i], connected_count);
            } else {
                lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            }
        }

        /* Force synchronous layout so new positions are available */
        lv_obj_update_layout(sum_page);

        /* INVERT + PLAY: animate transitions */
        for (int i = 0; i < card_count && i < count; i++) {
            if (!nina_connection_is_connected(i)) continue;
            int32_t new_y = lv_obj_get_y(cards[i].card);
            if (!was_visible[i]) {
                /* New card — fade in + slide up */
                animate_card_in(&cards[i]);
            } else if (old_y[i] != new_y) {
                /* Existing card moved — smooth glide to new position */
                animate_card_move(&cards[i], old_y[i] - new_y);
            }
        }
    } else {
        /* No layout change — just ensure correct visibility */
        for (int i = 0; i < card_count && i < count; i++) {
            if (nina_connection_is_connected(i)) {
                lv_obj_clear_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    prev_visible_count = connected_count;

    /* ── Update card data ────────────────────────────────────────── */
    for (int i = 0; i < card_count && i < count; i++) {
        summary_card_t *sc = &cards[i];
        const nina_client_t *d = &instances[i];

        if (!nina_connection_is_connected(i)) continue;

        /* Instance name — telescope + camera, fallback to profile, then host */
        if (d->telescope_name[0] && d->camera_name[0]) {
            char combined[128];
            snprintf(combined, sizeof(combined), "%s | %s", d->telescope_name, d->camera_name);
            set_label_if_changed(sc->lbl_name, combined);
        } else if (d->telescope_name[0]) {
            set_label_if_changed(sc->lbl_name, d->telescope_name);
        } else if (d->camera_name[0]) {
            set_label_if_changed(sc->lbl_name, d->camera_name);
        } else if (d->profile_name[0]) {
            set_label_if_changed(sc->lbl_name, d->profile_name);
        } else {
            const char *url = app_config_get_instance_url(i);
            char host[64] = {0};
            extract_host_from_url(url, host, sizeof(host));
            set_label_if_changed(sc->lbl_name, host[0] ? host : "N.I.N.A.");
        }

        /* Name color: theme header color (always connected at this point) */
        if (current_theme) {
            uint32_t name_color = app_config_apply_brightness(
                current_theme->header_text_color, gb);
            lv_obj_set_style_text_color(sc->lbl_name, lv_color_hex(name_color), 0);
        }

        /* Filter */
        if (d->current_filter[0]) {
            set_label_if_changed(sc->lbl_filter, d->current_filter);
            uint32_t fc = theme_forces_colors()
                ? 0 : app_config_get_filter_color(d->current_filter, i);
            if (fc != 0) {
                lv_obj_set_style_text_color(sc->lbl_filter,
                    lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
                lv_obj_set_style_bg_color(sc->filter_box,
                    lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
                lv_obj_set_style_bg_opa(sc->filter_box, LV_OPA_20, 0);
            } else if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_filter,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->filter_text_color, gb)), 0);
                lv_obj_set_style_bg_color(sc->filter_box,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->bento_border, gb)), 0);
                lv_obj_set_style_bg_opa(sc->filter_box, LV_OPA_50, 0);
            }
        } else {
            set_label_if_changed(sc->lbl_filter, "--");
            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_filter,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->label_color, gb)), 0);
            }
            lv_obj_set_style_bg_color(sc->filter_box,
                lv_color_hex(current_theme ? current_theme->bento_bg : 0x000000), 0);
            lv_obj_set_style_bg_opa(sc->filter_box, LV_OPA_50, 0);
        }

        /* Target name */
        if (d->target_name[0]) {
            set_label_if_changed(sc->lbl_target, d->target_name);
        } else {
            set_label_if_changed(sc->lbl_target, "Idle");
        }

        if (current_theme) {
            uint32_t tgt_color = app_config_apply_brightness(
                current_theme->target_name_color, gb);
            lv_obj_set_style_text_color(sc->lbl_target, lv_color_hex(tgt_color), 0);
        }

        /* Progress bar + percentage */
        if (d->exposure_total > 0) {
            int pct = (int)((d->exposure_current / d->exposure_total) * 100.0f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            set_bar_if_changed(sc->bar_progress, pct, LV_ANIM_ON);

            SET_LABEL_FMT_IF_CHANGED(sc->lbl_pct, 8, "%d%%", pct);
        } else {
            set_bar_if_changed(sc->bar_progress, 0, LV_ANIM_OFF);
            set_label_if_changed(sc->lbl_pct, "");
        }

        /* Progress bar color: filter color or theme progress */
        {
            uint32_t bar_col = 0;
            if (!theme_forces_colors() && d->current_filter[0]) {
                bar_col = app_config_get_filter_color(d->current_filter, i);
            }
            if (bar_col == 0 && current_theme) {
                bar_col = current_theme->progress_color;
            }
            if (bar_col != 0) {
                lv_obj_set_style_bg_color(sc->bar_progress,
                    lv_color_hex(app_config_apply_brightness(bar_col, gb)),
                    LV_PART_INDICATOR);
            }
            if (current_theme) {
                lv_obj_set_style_bg_color(sc->bar_progress,
                    lv_color_hex(current_theme->bento_border), 0);
            }
        }

        /* Percentage label color */
        if (current_theme) {
            lv_obj_set_style_text_color(sc->lbl_pct,
                lv_color_hex(app_config_apply_brightness(
                    current_theme->text_color, gb)), 0);
        }

        /* Sequence info (visible in 1-2 card mode) */
        if (connected_count <= 2 && !lv_obj_has_flag(sc->seq_row, LV_OBJ_FLAG_HIDDEN)) {
            if (d->container_name[0]) {
                set_label_if_changed(sc->lbl_seq_name, d->container_name);
            } else {
                set_label_if_changed(sc->lbl_seq_name, "----");
            }
            if (d->container_step[0]) {
                set_label_if_changed(sc->lbl_seq_step, d->container_step);
            } else {
                set_label_if_changed(sc->lbl_seq_step, "----");
            }

            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_seq_name,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->header_text_color, gb)), 0);
                lv_obj_set_style_text_color(sc->lbl_seq_step,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->text_color, gb)), 0);
            }
        }

        /* RMS */
        if (d->guider.rms_total > 0.001f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f\"", d->guider.rms_total);
            set_label_if_changed(sc->lbl_rms_val, buf);
            uint32_t rms_col = theme_forces_colors()
                ? 0 : app_config_get_rms_color(d->guider.rms_total, i);
            if (rms_col != 0) {
                lv_obj_set_style_text_color(sc->lbl_rms_val,
                    lv_color_hex(app_config_apply_brightness(rms_col, gb)), 0);
            } else if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_rms_val,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->rms_color, gb)), 0);
            }
        } else {
            set_label_if_changed(sc->lbl_rms_val, "--");
            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_rms_val,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->text_color, gb)), 0);
            }
        }

        /* HFR */
        if (d->hfr > 0.001f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", d->hfr);
            set_label_if_changed(sc->lbl_hfr_val, buf);
            uint32_t hfr_col = theme_forces_colors()
                ? 0 : app_config_get_hfr_color(d->hfr, i);
            if (hfr_col != 0) {
                lv_obj_set_style_text_color(sc->lbl_hfr_val,
                    lv_color_hex(app_config_apply_brightness(hfr_col, gb)), 0);
            } else if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_hfr_val,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->hfr_color, gb)), 0);
            }
        } else {
            set_label_if_changed(sc->lbl_hfr_val, "--");
            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_hfr_val,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->text_color, gb)), 0);
            }
        }

        /* Time to flip — format "HH:MM:SS" as "Xh XXm" */
        if (d->meridian_flip[0] && strcmp(d->meridian_flip, "--") != 0
            && strcmp(d->meridian_flip, "FLIPPING") != 0) {
            int hh = 0, mm = 0;
            if (sscanf(d->meridian_flip, "%d:%d", &hh, &mm) >= 2) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%dh %02dm", hh, mm);
                set_label_if_changed(sc->lbl_flip_val, buf);
            } else {
                set_label_if_changed(sc->lbl_flip_val, d->meridian_flip);
            }
        } else if (d->meridian_flip[0]) {
            set_label_if_changed(sc->lbl_flip_val, d->meridian_flip);
        } else {
            set_label_if_changed(sc->lbl_flip_val, "--");
        }
        if (current_theme) {
            lv_obj_set_style_text_color(sc->lbl_flip_val,
                lv_color_hex(app_config_apply_brightness(
                    current_theme->text_color, gb)), 0);
        }

        /* Exposure detail line (1-card mode only) */
        if (connected_count <= 1 &&
            !lv_obj_has_flag(sc->detail_row, LV_OBJ_FLAG_HIDDEN)) {
            char detail[128] = "";
            int len = 0;

            if (d->exposure_total > 0) {
                len += snprintf(detail + len, sizeof(detail) - len,
                    "%ds %s", (int)d->exposure_total,
                    d->current_filter[0] ? d->current_filter : "");
            }
            if (d->exposure_iterations > 0 && len > 0) {
                len += snprintf(detail + len, sizeof(detail) - len,
                    "  |  %d / %d exp",
                    d->exposure_count, d->exposure_iterations);
            }
            if (d->stars > 0 && len > 0) {
                len += snprintf(detail + len, sizeof(detail) - len,
                    "  |  %d stars", d->stars);
            }
            if (d->target_time_remaining[0] && len > 0) {
                if (d->target_time_reason[0] &&
                    strcmp(d->target_time_reason, "TIME LEFT") != 0) {
                    // Show constraint-specific label: "sets in 2:30" / "dawn in 1:45"
                    char reason_lower[16];
                    strlcpy(reason_lower, d->target_time_reason, sizeof(reason_lower));
                    for (int c = 0; reason_lower[c]; c++)
                        if (reason_lower[c] >= 'A' && reason_lower[c] <= 'Z')
                            reason_lower[c] += 32;
                    len += snprintf(detail + len, sizeof(detail) - len,
                        "  |  %s %s", reason_lower, d->target_time_remaining);
                } else {
                    len += snprintf(detail + len, sizeof(detail) - len,
                        "  |  %s left", d->target_time_remaining);
                }
            }

            set_label_if_changed(sc->lbl_detail, detail);

            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_detail,
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->text_color, gb)), 0);
            }
        }

        /* Safety monitor icon */
        if (sc->lbl_safety) {
            lv_obj_clear_flag(sc->lbl_safety, LV_OBJ_FLAG_HIDDEN);
            if (d->safety_connected) {
                if (d->safety_is_safe) {
                    set_label_if_changed(sc->lbl_safety, ICON_VERIFIED_USER);
                    lv_obj_set_style_text_color(sc->lbl_safety,
                        lv_color_hex(theme_forces_colors() ? 0x7f1d1d : 0x4CAF50), 0);
                } else {
                    set_label_if_changed(sc->lbl_safety, ICON_GPP_BAD);
                    lv_obj_set_style_text_color(sc->lbl_safety,
                        lv_color_hex(theme_forces_colors() ? 0xff0000 : 0xF44336), 0);
                }
            } else {
                set_label_if_changed(sc->lbl_safety, ICON_GPP_MAYBE);
                lv_obj_set_style_text_color(sc->lbl_safety, lv_color_hex(0x999999), 0);
            }
        }
    }
}

/* ── Theme Application ─────────────────────────────────────────────── */

void summary_page_apply_theme(void) {
    if (!sum_page || !current_theme) return;

    int gb = app_config_get()->color_brightness;

    /* Update glass card style */
    apply_glass_theme();
    lv_obj_report_style_change(&style_glass_card);

    /* Update per-card widgets */
    for (int i = 0; i < card_count; i++) {
        summary_card_t *sc = &cards[i];

        /* Stat labels + sequence title labels */
        lv_obj_t *labels[] = {
            sc->lbl_rms_label, sc->lbl_hfr_label, sc->lbl_flip_label,
            sc->lbl_seq_title, sc->lbl_step_title
        };
        for (int j = 0; j < 5; j++) {
            if (labels[j]) {
                lv_obj_set_style_text_color(labels[j],
                    lv_color_hex(app_config_apply_brightness(
                        current_theme->label_color, gb)), 0);
            }
        }

        /* Bar background */
        if (sc->bar_progress) {
            lv_obj_set_style_bg_color(sc->bar_progress,
                lv_color_hex(current_theme->bento_border), 0);
        }
    }

    /* Update empty state theme */
    if (empty_msg) {
        lv_obj_set_style_text_color(empty_msg,
            lv_color_hex(app_config_apply_brightness(
                current_theme->header_text_color, gb)), 0);
    }
    if (empty_sub) {
        lv_obj_set_style_text_color(empty_sub,
            lv_color_hex(app_config_apply_brightness(
                current_theme->label_color, gb)), 0);
    }

    lv_obj_invalidate(sum_page);
}
