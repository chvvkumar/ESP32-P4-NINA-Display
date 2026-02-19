/**
 * @file nina_summary.c
 * @brief Summary page — glassmorphism cards showing all NINA instances at a glance.
 *
 * Displays one card per configured NINA instance with: instance name, filter,
 * target name, progress bar, and key stats (RMS, HFR, time to flip).
 */

#include "nina_summary.h"
#include "nina_dashboard_internal.h"
#include "nina_dashboard.h"
#include "app_config.h"
#include "themes.h"

#include <stdio.h>
#include <string.h>

/* ── Layout Constants ──────────────────────────────────────────────── */
#define CARD_RADIUS      20
#define CARD_PAD         16
#define CARD_GAP         16
#define BAR_HEIGHT        6
#define STAT_COLS         3

/* ── Per-card widget references ────────────────────────────────────── */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *lbl_name;
    lv_obj_t *lbl_filter;
    lv_obj_t *filter_box;
    lv_obj_t *lbl_target;
    lv_obj_t *bar_progress;
    lv_obj_t *lbl_rms_label;
    lv_obj_t *lbl_rms_val;
    lv_obj_t *lbl_hfr_label;
    lv_obj_t *lbl_hfr_val;
    lv_obj_t *lbl_flip_label;
    lv_obj_t *lbl_flip_val;
} summary_card_t;

/* ── Module state ──────────────────────────────────────────────────── */
static lv_obj_t *sum_page = NULL;
static summary_card_t cards[MAX_NINA_INSTANCES];
static int card_count = 0;

/* Glass card style — semi-transparent with subtle border */
static lv_style_t style_glass_card;
static bool styles_initialized = false;

/* ── Helpers ───────────────────────────────────────────────────────── */

static void summary_card_click_cb(lv_event_t *e) {
    int instance_index = (int)(intptr_t)lv_event_get_user_data(e);
    /* Page 0 is summary. Page 1..N are NINA instances. */
    nina_dashboard_show_page(instance_index + 1, 0);
}

static void init_glass_styles(void) {
    if (styles_initialized) return;
    styles_initialized = true;

    lv_style_init(&style_glass_card);
    lv_style_set_radius(&style_glass_card, CARD_RADIUS);
    lv_style_set_pad_all(&style_glass_card, CARD_PAD);
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
    lv_obj_set_width(block, LV_PCT(33));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
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
#ifdef LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
#elif defined(LV_FONT_MONTSERRAT_24)
    lv_obj_set_style_text_font(val, &lv_font_montserrat_24, 0);
#else
    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
#endif

    *out_label = lbl;
    *out_value = val;
}

/**
 * @brief Build one glassmorphism instance card.
 */
static void create_card(summary_card_t *sc, lv_obj_t *parent, int instance_index) {
    memset(sc, 0, sizeof(summary_card_t));

    /* ── Card container ── */
    sc->card = lv_obj_create(parent);
    lv_obj_remove_style_all(sc->card);
    lv_obj_add_style(sc->card, &style_glass_card, 0);
    lv_obj_set_width(sc->card, LV_PCT(100));
    lv_obj_set_flex_grow(sc->card, 1);
    lv_obj_set_flex_flow(sc->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sc->card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(sc->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(sc->card, 6, 0);

    /* Make card clickable for navigation */
    lv_obj_add_flag(sc->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sc->card, summary_card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)instance_index);

    /* ── Header row: instance name + filter badge ── */
    lv_obj_t *header = lv_obj_create(sc->card);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Instance name */
    sc->lbl_name = lv_label_create(header);
    lv_obj_set_style_text_font(sc->lbl_name, &lv_font_montserrat_18, 0);
    {
        const char *url = app_config_get_instance_url(instance_index);
        char host[64] = {0};
        extract_host_from_url(url, host, sizeof(host));
        lv_label_set_text(sc->lbl_name, host[0] ? host : "N.I.N.A.");
    }

    /* Filter badge */
    sc->filter_box = lv_obj_create(header);
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

    /* ── Target name ── */
    sc->lbl_target = lv_label_create(sc->card);
    lv_obj_set_width(sc->lbl_target, LV_PCT(100));
    lv_obj_set_style_text_align(sc->lbl_target, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(sc->lbl_target, LV_LABEL_LONG_DOT);
#ifdef LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_24, 0);
#else
    lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_20, 0);
#endif
    lv_label_set_text(sc->lbl_target, "----");

    /* ── Progress bar ── */
    sc->bar_progress = lv_bar_create(sc->card);
    lv_obj_set_width(sc->bar_progress, LV_PCT(100));
    lv_obj_set_height(sc->bar_progress, BAR_HEIGHT);
    lv_bar_set_range(sc->bar_progress, 0, 100);
    lv_bar_set_value(sc->bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(sc->bar_progress, 3, 0);
    lv_obj_set_style_radius(sc->bar_progress, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sc->bar_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(sc->bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);

    /* ── Stats row: RMS | HFR | FLIP ── */
    lv_obj_t *stats = lv_obj_create(sc->card);
    lv_obj_remove_style_all(stats);
    lv_obj_set_width(stats, LV_PCT(100));
    lv_obj_set_height(stats, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);

    create_stat_block(stats, "RMS",  &sc->lbl_rms_label,  &sc->lbl_rms_val);
    create_stat_block(stats, "HFR",  &sc->lbl_hfr_label,  &sc->lbl_hfr_val);
    create_stat_block(stats, "FLIP", &sc->lbl_flip_label, &sc->lbl_flip_val);
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
    }

    return sum_page;
}

/* ── Layout Update ─────────────────────────────────────────────────── */

static void update_card_layout(summary_card_t *sc, bool large) {
    /* Instance Name */
    if (large) {
#ifdef LV_FONT_MONTSERRAT_28
        lv_obj_set_style_text_font(sc->lbl_name, &lv_font_montserrat_28, 0);
#elif defined(LV_FONT_MONTSERRAT_24)
        lv_obj_set_style_text_font(sc->lbl_name, &lv_font_montserrat_24, 0);
#else
        lv_obj_set_style_text_font(sc->lbl_name, &lv_font_montserrat_20, 0);
#endif
    } else {
        lv_obj_set_style_text_font(sc->lbl_name, &lv_font_montserrat_18, 0);
    }

    /* Filter Badge Text */
    if (large) {
#ifdef LV_FONT_MONTSERRAT_20
        lv_obj_set_style_text_font(sc->lbl_filter, &lv_font_montserrat_20, 0);
#elif defined(LV_FONT_MONTSERRAT_18)
        lv_obj_set_style_text_font(sc->lbl_filter, &lv_font_montserrat_18, 0);
#else
        lv_obj_set_style_text_font(sc->lbl_filter, &lv_font_montserrat_14, 0);
#endif
    } else {
        lv_obj_set_style_text_font(sc->lbl_filter, &lv_font_montserrat_14, 0);
    }

    /* Target Name */
    if (large) {
#ifdef LV_FONT_MONTSERRAT_32
        lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_32, 0);
#elif defined(LV_FONT_MONTSERRAT_28)
        lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_28, 0);
#else
        lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_24, 0);
#endif
    } else {
#ifdef LV_FONT_MONTSERRAT_24
        lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_24, 0);
#else
        lv_obj_set_style_text_font(sc->lbl_target, &lv_font_montserrat_20, 0);
#endif
    }

    /* Progress Bar Height */
    lv_obj_set_height(sc->bar_progress, large ? 12 : 6);

    /* Stats (Labels and Values) */
    lv_obj_t *labels[] = { sc->lbl_rms_label, sc->lbl_hfr_label, sc->lbl_flip_label };
    lv_obj_t *values[] = { sc->lbl_rms_val, sc->lbl_hfr_val, sc->lbl_flip_val };

    const lv_font_t *font_stat_label = large ? 
#ifdef LV_FONT_MONTSERRAT_18
        &lv_font_montserrat_18 
#else
        &lv_font_montserrat_14
#endif
        : &lv_font_montserrat_14;

    const lv_font_t *font_stat_value = large ?
#ifdef LV_FONT_MONTSERRAT_36
        &lv_font_montserrat_36
#elif defined(LV_FONT_MONTSERRAT_32)
        &lv_font_montserrat_32
#elif defined(LV_FONT_MONTSERRAT_28)
        &lv_font_montserrat_28
#else
        &lv_font_montserrat_24
#endif
        : 
#ifdef LV_FONT_MONTSERRAT_28
        &lv_font_montserrat_28;
#elif defined(LV_FONT_MONTSERRAT_24)
        &lv_font_montserrat_24;
#else
        &lv_font_montserrat_20;
#endif

    for (int i = 0; i < 3; i++) {
        if (labels[i]) lv_obj_set_style_text_font(labels[i], font_stat_label, 0);
        if (values[i]) lv_obj_set_style_text_font(values[i], font_stat_value, 0);
    }
}

/* ── Data Update ───────────────────────────────────────────────────── */

void summary_page_update(const nina_client_t *instances, int count) {
    if (!sum_page) return;

    int gb = app_config_get()->color_brightness;
    
    /* First count how many are connected */
    int connected_count = 0;
    for (int i = 0; i < count; i++) {
        if (instances[i].connected) connected_count++;
    }

    /* Determine visibility and layout mode */
    int visible_count = (connected_count > 0) ? connected_count : card_count;
    bool large_mode = (visible_count < 3);

    for (int i = 0; i < card_count && i < count; i++) {
        summary_card_t *sc = &cards[i];
        const nina_client_t *d = &instances[i];

        /* Visibility logic:
         * 1. If NO instances are connected, show all (placeholder mode).
         * 2. If ANY instance is connected, hide disconnected ones.
         */
        bool show = (connected_count == 0) || d->connected;
        
        if (show) {
            lv_obj_clear_flag(sc->card, LV_OBJ_FLAG_HIDDEN);
            update_card_layout(sc, large_mode);
        } else {
            lv_obj_add_flag(sc->card, LV_OBJ_FLAG_HIDDEN);
            continue; /* Skip updating hidden cards */
        }

        /* Instance name — update if profile available */
        if (d->connected && d->profile_name[0]) {
            lv_label_set_text(sc->lbl_name, d->profile_name);
        } else if (!d->connected) {
            const char *url = app_config_get_instance_url(i);
            char host[64] = {0};
            extract_host_from_url(url, host, sizeof(host));
            char buf[96];
            snprintf(buf, sizeof(buf), "%s", host[0] ? host : "N.I.N.A.");
            lv_label_set_text(sc->lbl_name, buf);
        }

        /* Name color: theme header color when connected, dimmed when not */
        if (current_theme) {
            uint32_t name_color = d->connected
                ? app_config_apply_brightness(current_theme->header_text_color, gb)
                : app_config_apply_brightness(current_theme->label_color, gb);
            lv_obj_set_style_text_color(sc->lbl_name, lv_color_hex(name_color), 0);
        }

        /* Filter */
        if (d->connected && d->current_filter[0]) {
            lv_label_set_text(sc->lbl_filter, d->current_filter);
            uint32_t fc = app_config_get_filter_color(d->current_filter, i);
            if (fc != 0) {
                lv_obj_set_style_text_color(sc->lbl_filter,
                    lv_color_hex(app_config_apply_brightness(fc, gb)), 0);
            } else if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_filter,
                    lv_color_hex(app_config_apply_brightness(current_theme->filter_text_color, gb)), 0);
            }
        } else {
            lv_label_set_text(sc->lbl_filter, "--");
            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_filter,
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
            }
        }

        /* Target name */
        if (d->connected && d->target_name[0]) {
            lv_label_set_text(sc->lbl_target, d->target_name);
        } else if (!d->connected) {
            lv_label_set_text(sc->lbl_target, "Not Connected");
        } else {
            lv_label_set_text(sc->lbl_target, "Idle");
        }

        /* Target name color */
        if (current_theme) {
            uint32_t tgt_color = d->connected
                ? app_config_apply_brightness(current_theme->target_name_color, gb)
                : app_config_apply_brightness(current_theme->label_color, gb);
            lv_obj_set_style_text_color(sc->lbl_target, lv_color_hex(tgt_color), 0);
        }

        /* Progress bar */
        if (d->connected && d->exposure_total > 0) {
            int pct = (int)((d->exposure_current / d->exposure_total) * 100.0f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            lv_bar_set_value(sc->bar_progress, pct, LV_ANIM_ON);
        } else {
            lv_bar_set_value(sc->bar_progress, 0, LV_ANIM_OFF);
        }

        /* Progress bar color: filter color or theme progress */
        {
            uint32_t bar_col = 0;
            if (d->connected && d->current_filter[0]) {
                bar_col = app_config_get_filter_color(d->current_filter, i);
            }
            if (bar_col == 0 && current_theme) {
                bar_col = current_theme->progress_color;
            }
            if (bar_col != 0) {
                lv_obj_set_style_bg_color(sc->bar_progress,
                    lv_color_hex(app_config_apply_brightness(bar_col, gb)), LV_PART_INDICATOR);
            }
            /* Bar background */
            if (current_theme) {
                lv_obj_set_style_bg_color(sc->bar_progress,
                    lv_color_hex(current_theme->bento_border), 0);
            }
        }

        /* RMS */
        if (d->connected && d->guider.rms_total > 0.001f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f\"", d->guider.rms_total);
            lv_label_set_text(sc->lbl_rms_val, buf);
            uint32_t rms_col = app_config_get_rms_color(d->guider.rms_total, i);
            if (rms_col != 0) {
                lv_obj_set_style_text_color(sc->lbl_rms_val,
                    lv_color_hex(app_config_apply_brightness(rms_col, gb)), 0);
            } else if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_rms_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->rms_color, gb)), 0);
            }
        } else {
            lv_label_set_text(sc->lbl_rms_val, "--");
            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_rms_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }
        }

        /* HFR */
        if (d->connected && d->hfr > 0.001f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", d->hfr);
            lv_label_set_text(sc->lbl_hfr_val, buf);
            uint32_t hfr_col = app_config_get_hfr_color(d->hfr, i);
            if (hfr_col != 0) {
                lv_obj_set_style_text_color(sc->lbl_hfr_val,
                    lv_color_hex(app_config_apply_brightness(hfr_col, gb)), 0);
            } else if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_hfr_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->hfr_color, gb)), 0);
            }
        } else {
            lv_label_set_text(sc->lbl_hfr_val, "--");
            if (current_theme) {
                lv_obj_set_style_text_color(sc->lbl_hfr_val,
                    lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
            }
        }

        /* Time to flip */
        if (d->connected && d->meridian_flip[0]) {
            lv_label_set_text(sc->lbl_flip_val, d->meridian_flip);
        } else {
            lv_label_set_text(sc->lbl_flip_val, "--");
        }
        if (current_theme) {
            lv_obj_set_style_text_color(sc->lbl_flip_val,
                lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
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

        /* Stat labels */
        lv_obj_t *labels[] = { sc->lbl_rms_label, sc->lbl_hfr_label, sc->lbl_flip_label };
        for (int j = 0; j < 3; j++) {
            if (labels[j]) {
                lv_obj_set_style_text_color(labels[j],
                    lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
            }
        }

        /* Bar background */
        if (sc->bar_progress) {
            lv_obj_set_style_bg_color(sc->bar_progress,
                lv_color_hex(current_theme->bento_border), 0);
        }
    }

    lv_obj_invalidate(sum_page);
}
