/**
 * @file nina_dashboard.c
 * @brief NINA dashboard UI - grid layout, page creation, theme application, gestures
 *
 * Each NINA instance gets its own page. Swipe left/right to switch pages.
 * Data updates and thumbnail overlay are in separate files.
 */

#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "nina_empty_state.h"
#include "nina_thumbnail.h"
#include "nina_graph_overlay.h"
#include "nina_info_overlay.h"
#include "nina_sysinfo.h"
#include "nina_summary.h"
#include "nina_allsky.h"
#include "nina_json.h"
#include "nina_ha.h"
#include "nina_spotify.h"
#include "nina_clock.h"
#include "page_registry.h"
#include "nina_image_display.h"
#include "moon_interaction.h"
#include "nina_settings_tabview.h"
#include "nina_toast.h"
#include "nina_event_log.h"
#include "nina_alerts.h"
#include "nina_safety.h"
#include "nina_idle_indicator.h"
#include "nina_nav_arbiter.h"
#include "nina_ota_prompt.h"
#include "nina_wait_overlay.h"
#include "ui_styles.h"
#include "app_config.h"
#include "themes.h"
#include "tasks.h"
#include "lvgl.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

/* Shared state — accessed by nina_dashboard_update.c and nina_thumbnail.c */
dashboard_page_t pages[MAX_NINA_INSTANCES];
int page_count = 0;
int active_page = 0;
const theme_t *current_theme = NULL;
bool nina_slot_available[MAX_NINA_INSTANCES] = { false, false, false };
int  nina_available_count = 0;

/* AllSky page — always at PAGE_IDX_ALLSKY (0), excluded from indicators */
lv_obj_t *allsky_obj = NULL;
static lv_obj_t *allsky_page_created = NULL;  /* Always holds the created page object */

/* ── AllSky page registry ops (Task 4.7 wave P7b) ──
 * get_obj reads allsky_obj (NOT allsky_page_created): allsky_obj is NULL
 * whenever the page is disabled, so is_available=NULL (derive from
 * get_obj()!=NULL) reproduces the enabled-AND-created guard used throughout
 * this file without a separate availability check. The page itself is never
 * destroyed once created (only hidden/tracked), so destroy stays NULL. */
static lv_obj_t *allsky_ops_get_obj(void) { return allsky_obj; }

static const page_ops_t s_allsky_page_ops = {
    .create       = allsky_page_create,
    .destroy      = NULL,
    .get_obj      = allsky_ops_get_obj,
    .show         = NULL,
    .hide         = NULL,
    .apply_theme  = allsky_page_apply_theme,
    .is_available = NULL,
};

/* JSON Display page — always at PAGE_IDX_JSON (4), excluded from indicators.
 * json_obj is NULL when disabled (same NULL-when-disabled pattern as AllSky). */
static lv_obj_t *json_obj = NULL;
static lv_obj_t *json_page_created = NULL;  /* Always holds the created page object */

static lv_obj_t *json_ops_get_obj(void) { return json_obj; }

static const page_ops_t s_json_page_ops = {
    .create       = json_page_create,
    .destroy      = NULL,
    .get_obj      = json_ops_get_obj,
    .show         = NULL,
    .hide         = NULL,
    .apply_theme  = json_page_apply_theme,
    .is_available = NULL,
};

/* Home Assistant page — always at PAGE_IDX_HA (5), excluded from indicators.
 * ha_obj is NULL when disabled (same NULL-when-disabled pattern as JSON). */
static lv_obj_t *ha_obj = NULL;
static lv_obj_t *ha_page_created = NULL;  /* Always holds the created page object */

static lv_obj_t *ha_ops_get_obj(void) { return ha_obj; }

static const page_ops_t s_ha_page_ops = {
    .create       = ha_page_create,
    .destroy      = NULL,
    .get_obj      = ha_ops_get_obj,
    .show         = NULL,
    .hide         = NULL,
    .apply_theme  = ha_page_apply_theme,
    .is_available = NULL,
};

/* Spotify page — always at PAGE_IDX_SPOTIFY (1), excluded from indicators */
lv_obj_t *spotify_obj = NULL;
static lv_obj_t *spotify_page_created = NULL;  /* Always holds the created page object */

/* ── Spotify page registry ops (Task 4.7 wave P7b) ──
 * Same NULL-when-disabled pattern as AllSky above; show/hide route to the
 * existing idle-timer lifecycle callbacks. */
static lv_obj_t *spotify_ops_get_obj(void) { return spotify_obj; }

static const page_ops_t s_spotify_page_ops = {
    .create       = spotify_page_create,
    .destroy      = NULL,
    .get_obj      = spotify_ops_get_obj,
    .show         = nina_spotify_on_show,
    .hide         = nina_spotify_on_hide,
    .apply_theme  = spotify_page_apply_theme,
    .is_available = NULL,
};

/* Clock page — always at PAGE_IDX_CLOCK (2), always present */
lv_obj_t *clock_obj = NULL;

/* ── Clock page registry ops (Task 4.7 / retro 4.7, wave P7a proof) ──
 * Thunks over the existing clock_page_* functions; get_obj reads the
 * clock_obj global above (clock_obj lives here, not in nina_clock.c). */
static lv_obj_t *clock_ops_get_obj(void) { return clock_obj; }

static const page_ops_t s_clock_page_ops = {
    .create       = clock_page_create,
    .destroy      = NULL,   /* clock page is never destroyed, always present */
    .get_obj      = clock_ops_get_obj,
    .show         = clock_page_on_show,
    .hide         = clock_page_on_hide,
    .apply_theme  = clock_page_apply_theme,
    .is_available = NULL,   /* NULL = derive from get_obj() != NULL (always available) */
};

/* Image Display page — always at PAGE_IDX_IMAGE_DISPLAY (3), excluded from indicators */
lv_obj_t *image_display_obj = NULL;
static lv_obj_t *image_display_page_created = NULL;

/* ── Image Display page registry ops (Task 4.7 wave P7b) ──
 * Registered under PAGE_REF_IMG_DEFAULT: the registry's generic "the Image
 * Display page itself" id (page_idx = PAGE_IDX_IMAGE_DISPLAY, img_src = -1),
 * distinct from the per-source ids (PAGE_REF_IMG_GOES/MOON/SOLAR/CUSTOM) that
 * share the same page_idx. Page lifecycle (create/get_obj/apply_theme) is
 * per-page, not per-source, so one ops registration covers all sources. */
static lv_obj_t *image_display_ops_get_obj(void) { return image_display_obj; }

static const page_ops_t s_image_display_page_ops = {
    .create       = nina_image_display_create,
    .destroy      = NULL,
    .get_obj      = image_display_ops_get_obj,
    .show         = NULL,
    .hide         = NULL,
    .apply_theme  = nina_image_display_apply_theme,
    .is_available = NULL,
};

/* Summary page — at PAGE_IDX_SUMMARY (5), excluded from indicators */
static lv_obj_t *summary_obj = NULL;

/* ── Summary page registry ops (Task 4.7 wave P7b) ──
 * Summary is always present (never disabled/NULL'd), so get_obj simply
 * returns summary_obj and is_available derives to "always true" once created.
 * show_page_at() has no summary-specific on-show behavior today beyond
 * clearing the hidden flag (summary_page_update() runs from data_update_task
 * on its own poll cadence, not on page-show), so show is left NULL. */
static lv_obj_t *summary_ops_get_obj(void) { return summary_obj; }

static const page_ops_t s_summary_page_ops = {
    .create       = summary_page_create,
    .destroy      = NULL,
    .get_obj      = summary_ops_get_obj,
    .show         = NULL,
    .hide         = NULL,
    .apply_theme  = summary_page_apply_theme,
    .is_available = NULL,
};

/* Settings page — at SETTINGS_PAGE_IDX(page_count), excluded from indicators */
static lv_obj_t *settings_obj = NULL;

/* System info page — at SYSINFO_PAGE_IDX(page_count), excluded from indicators */
static lv_obj_t *sysinfo_obj = NULL;

/* ── System info page registry ops (Task 4.7 wave P7b) ──
 * The pre-ops show_page_at() branch refreshed stats every time the page
 * became visible (sysinfo_page_refresh()); fold that into the show thunk so
 * ops-driven show reproduces the exact same on-show behavior. */
static lv_obj_t *sysinfo_ops_get_obj(void) { return sysinfo_obj; }
static void sysinfo_ops_show(void) { sysinfo_page_refresh(); }

static const page_ops_t s_sysinfo_page_ops = {
    .create       = sysinfo_page_create,
    .destroy      = NULL,
    .get_obj      = sysinfo_ops_get_obj,
    .show         = sysinfo_ops_show,
    .hide         = NULL,
    .apply_theme  = sysinfo_page_apply_theme,
    .is_available = NULL,
};
int total_page_count = 0;   /* page_count + EXTRA_PAGES (allsky + spotify + clock + image_display + json + summary + settings + sysinfo) */

/* Private state */
static lv_obj_t *scr_dashboard = NULL;
static lv_obj_t *main_cont = NULL;

// Page dots
static lv_obj_t *indicator_cont = NULL;
static lv_obj_t *indicator_dots[MAX_NINA_INSTANCES];

// Swipe callback
static nina_page_change_cb_t page_change_cb = NULL;

static void update_indicators(void);
static void rms_click_cb(lv_event_t *e);
static void hfr_click_cb(lv_event_t *e);
static void exposure_arc_click_cb(lv_event_t *e);
static void flip_click_cb(lv_event_t *e);
static void stars_click_cb(lv_event_t *e);
static void sequence_click_cb(lv_event_t *e);
static void filter_click_cb(lv_event_t *e);
static void autofocus_long_press_cb(lv_event_t *e);
static void session_stats_click_cb(lv_event_t *e);

/* Strip http(s)://, path, and domain from URL, then sentence case */
void extract_host_from_url(const char *url, char *out, size_t out_size) {
    if (out_size == 0) return;

    const char *start = url;
    if (strncmp(start, "https://", 8) == 0) start += 8;
    else if (strncmp(start, "http://", 7) == 0) start += 7;

    const char *end = start;
    while (*end && *end != '/' && *end != ':' && *end != '.') end++;

    int len = (int)(end - start);
    if (len <= 0) { out[0] = '\0'; return; }
    if ((size_t)len >= out_size) len = (int)(out_size - 1);

    memcpy(out, start, (size_t)len);
    out[len] = '\0';

    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
    for (int i = 1; i < len; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;
    }
}

/* (Re)init styles from the active theme */
static void update_styles(void) {
    ui_styles_update(current_theme);
}

lv_obj_t *create_bento_box(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &style_bento_box, 0);
    ui_styles_set_widget_draw_cbs(box);
    return box;
}

lv_obj_t *create_small_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &style_label_small, 0);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *create_value_label(lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &style_value_large, 0);
    lv_label_set_text(label, "--");
    return label;
}

/*
 * Page index convention (see PAGE_IDX_* / NINA_PAGE_OFFSET / EXTRA_PAGES in nina_dashboard_internal.h):
 *   PAGE_IDX_ALLSKY        (0)                  = AllSky page
 *   PAGE_IDX_SPOTIFY       (1)                  = Spotify page
 *   PAGE_IDX_CLOCK         (2)                  = Clock page (always present)
 *   PAGE_IDX_IMAGE_DISPLAY (3)                  = Image Display page
 *   PAGE_IDX_JSON          (4)                  = JSON Display page
 *   PAGE_IDX_HA            (5)                  = Home Assistant page
 *   PAGE_IDX_SUMMARY       (6)                  = summary page
 *   NINA_PAGE_OFFSET .. NINA_PAGE_OFFSET+pc-1   = NINA instance pages  (pages[idx - NINA_PAGE_OFFSET])
 *   SETTINGS_PAGE_IDX(pc)                       = settings page
 *   SYSINFO_PAGE_IDX(pc)                        = sysinfo page
 *   total_page_count                            = page_count + EXTRA_PAGES
 */

/* Absolute page index -> NINA instance index, or -1 if not a NINA page.
 * Fixed identity: page NINA_PAGE_OFFSET + i is always instance i. */
static int abs_page_to_instance(int idx) {
    int inst = idx - NINA_PAGE_OFFSET;
    if (inst < 0 || inst >= MAX_NINA_INSTANCES) return -1;
    return inst;
}

/* Absolute page index -> registry page_ref id, or PAGE_REF_ID_MAX if the page
 * has not (yet) opted into ops-based dispatch. Only pages registered via
 * page_registry_set_ops() need an entry here; unmapped indices fall through
 * to the hardcoded branches below unchanged. */
static page_ref_t page_idx_to_ref_id(int idx) {
    if (idx == PAGE_IDX_CLOCK) return PAGE_REF_CLOCK;
    if (idx == PAGE_IDX_SUMMARY) return PAGE_REF_SUMMARY;
    if (idx == PAGE_IDX_ALLSKY) return PAGE_REF_ALLSKY;
    if (idx == PAGE_IDX_SPOTIFY) return PAGE_REF_SPOTIFY;
    if (idx == PAGE_IDX_JSON) return PAGE_REF_JSON;
    if (idx == PAGE_IDX_HA) return PAGE_REF_HA;
    if (idx == PAGE_IDX_IMAGE_DISPLAY) return PAGE_REF_IMG_DEFAULT;
    if (idx == SYSINFO_PAGE_IDX(page_count)) return PAGE_REF_SYSINFO;
    return PAGE_REF_ID_MAX;
}

/* Ops-driven variants of the get_obj/hide/show dispatchers. Returns true if
 * the page at idx has registered ops and was handled; false to fall back to
 * the hardcoded branches. */
static bool ops_get_obj(int idx, lv_obj_t **obj_out) {
    page_ref_t rid = page_idx_to_ref_id(idx);
    if (rid >= PAGE_REF_ID_MAX) return false;
    const page_ops_t *ops = page_registry_get_ops(rid);
    if (!ops) return false;
    *obj_out = ops->get_obj();
    return true;
}

static bool ops_hide(int idx) {
    page_ref_t rid = page_idx_to_ref_id(idx);
    if (rid >= PAGE_REF_ID_MAX) return false;
    const page_ops_t *ops = page_registry_get_ops(rid);
    if (!ops) return false;
    /* Matches the pre-ops branch shape exactly: both the hide flag and the
     * on-hide callback are gated on the object actually existing. */
    lv_obj_t *obj = ops->get_obj();
    if (obj) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        if (ops->hide) ops->hide();
    }
    return true;
}

static bool ops_show(int idx) {
    page_ref_t rid = page_idx_to_ref_id(idx);
    if (rid >= PAGE_REF_ID_MAX) return false;
    const page_ops_t *ops = page_registry_get_ops(rid);
    if (!ops) return false;
    /* Matches the pre-ops branch shape exactly: both the clear-hidden flag
     * and the on-show callback are gated on the object actually existing. */
    lv_obj_t *obj = ops->get_obj();
    if (obj) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        if (ops->show) ops->show();
    }
    return true;
}

/* Ops-driven apply_theme dispatcher helper. Returns true (handled) if @p idx
 * has registered ops, calling ops->apply_theme() unconditionally — the
 * per-page apply_theme functions already NULL-guard their own module-level
 * page pointer internally, so re-theming a currently-disabled/hidden page is
 * a safe no-op, matching how summary/sysinfo already re-theme unconditionally
 * today. Returns false (not handled) if @p idx has no registered ops, so the
 * caller can fall back to its hardcoded branch. */
static bool ops_apply_theme(int idx) {
    page_ref_t rid = page_idx_to_ref_id(idx);
    if (rid >= PAGE_REF_ID_MAX) return false;
    const page_ops_t *ops = page_registry_get_ops(rid);
    if (!ops) return false;
    if (ops->apply_theme) ops->apply_theme();
    return true;
}

/* True if the page at @p idx has registered ops AND is currently available
 * (ops->is_available() if provided, else ops->get_obj() != NULL). False if
 * @p idx has no registered ops — callers only use this for indices that are
 * always mapped by page_idx_to_ref_id() once ported. */
static bool page_ops_is_available(int idx) {
    page_ref_t rid = page_idx_to_ref_id(idx);
    if (rid >= PAGE_REF_ID_MAX) return false;
    const page_ops_t *ops = page_registry_get_ops(rid);
    if (!ops) return false;
    if (ops->is_available) return ops->is_available();
    return ops->get_obj() != NULL;
}

/* Hide the page object at the given index */
static void hide_page_at(int idx) {
    /* Ported pages (AllSky, Spotify, Clock, Image Display, Summary, Sysinfo)
     * are handled entirely by their registered ops. Only the NINA instance
     * band and the Settings modal remain as hardcoded fallback branches. */
    if (ops_hide(idx)) return;
    if (abs_page_to_instance(idx) >= 0) {
        int inst = abs_page_to_instance(idx);
        if (nina_slot_available[inst] && pages[inst].page)
            lv_obj_add_flag(pages[inst].page, LV_OBJ_FLAG_HIDDEN);
    }
    else if (idx == SETTINGS_PAGE_IDX(page_count) && settings_obj) {
        settings_tabview_destroy();
        settings_obj = NULL;
        /* Settings is a modal surface — unfreeze the arbiter on destroy. */
        nav_arbiter_notify_modal_close(esp_timer_get_time() / 1000);
    }
}

/* Show the page object at the given index */
static void show_page_at(int idx) {
    /* Ported pages route through ops (see hide_page_at note above). */
    if (ops_show(idx)) return;
    if (abs_page_to_instance(idx) >= 0) {
        int inst = abs_page_to_instance(idx);
        if (nina_slot_available[inst] && pages[inst].page)
            lv_obj_clear_flag(pages[inst].page, LV_OBJ_FLAG_HIDDEN);
    }
    else if (idx == SETTINGS_PAGE_IDX(page_count)) {
        if (!settings_obj) {
            settings_obj = settings_tabview_create(main_cont);
            /* Settings is a modal surface — freeze the arbiter on create.
             * Paired with the close in hide_page_at()'s destroy branch. */
            nav_arbiter_notify_modal_open();
        }
        lv_obj_clear_flag(settings_obj, LV_OBJ_FLAG_HIDDEN);
        settings_tabview_refresh();
    }
}

static lv_obj_t *get_page_obj(int idx) {
    /* Ported pages route through ops (see hide_page_at note above); the ops
     * get_obj itself returns NULL when an optional page is disabled. */
    lv_obj_t *ops_obj = NULL;
    if (ops_get_obj(idx, &ops_obj)) return ops_obj;
    if (abs_page_to_instance(idx) >= 0) {
        int inst = abs_page_to_instance(idx);
        return (nina_slot_available[inst] && pages[inst].page) ? pages[inst].page : NULL;
    }
    if (idx == SETTINGS_PAGE_IDX(page_count) && settings_obj) return settings_obj;
    return NULL;
}

bool nina_dashboard_page_is_available(int page_idx) {
    /* A page is available iff its backing object exists. Optional pages
     * (AllSky/Spotify/Image Display) have NULL objects when disabled, and NINA
     * indices beyond page_count resolve to NULL as well. */
    return get_page_obj(page_idx) != NULL;
}

/* Set theme colors on all widgets in a page */
static void apply_theme_to_page(dashboard_page_t *p) {
    if (!p->page || !current_theme) return;

    int gb = app_config_get()->color_brightness;

    if (p->lbl_instance_name) {
        uint32_t glow_color;
        if (theme_is_red_night(current_theme)) {
            glow_color = app_config_apply_brightness(
                p->nina_connected ? current_theme->text_color : current_theme->label_color, gb);
        } else {
            glow_color = app_config_apply_brightness(
                p->nina_connected ? 0x4ade80 : 0xf87171, gb);
        }
        lv_obj_set_style_text_color(p->lbl_instance_name, lv_color_hex(glow_color), 0);
    }

    if (p->lbl_target_name) lv_obj_set_style_text_color(p->lbl_target_name, lv_color_hex(app_config_apply_brightness(current_theme->target_name_color, gb)), 0);

    if (p->lbl_seq_container) lv_obj_set_style_text_color(p->lbl_seq_container, lv_color_hex(app_config_apply_brightness(current_theme->header_text_color, gb)), 0);
    if (p->lbl_seq_step) lv_obj_set_style_text_color(p->lbl_seq_step, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    if (p->arc_exposure) {
        lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(current_theme->bg_main), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(p->arc_exposure, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(p->arc_exposure, 0, LV_PART_INDICATOR);
    }

    if (p->lbl_loop_count) lv_obj_set_style_text_color(p->lbl_loop_count, lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    if (p->lbl_filter_done_header) lv_obj_set_style_text_color(p->lbl_filter_done_header, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    if (p->lbl_rms_title) lv_obj_set_style_text_color(p->lbl_rms_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_hfr_title) lv_obj_set_style_text_color(p->lbl_hfr_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_flip_title) lv_obj_set_style_text_color(p->lbl_flip_title, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_stars_header) lv_obj_set_style_text_color(p->lbl_stars_header, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    if (p->lbl_target_time_header) lv_obj_set_style_text_color(p->lbl_target_time_header, lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);

    if (p->lbl_rms_value) lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(app_config_apply_brightness(current_theme->rms_color, gb)), 0);
    if (p->lbl_hfr_value) lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(app_config_apply_brightness(current_theme->hfr_color, gb)), 0);

    for (int i = 0; i < MAX_POWER_WIDGETS; i++) {
        if (p->lbl_pwr_title[i]) lv_obj_set_style_text_color(p->lbl_pwr_title[i], lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    if (p->empty_state_cont) {
        nina_empty_state_apply_theme(p->empty_state_cont, current_theme, gb);
    }
}

const theme_t *nina_dashboard_get_current_theme(void) {
    return current_theme;
}

const theme_t *nina_dashboard_get_theme(void) {
    return nina_dashboard_get_current_theme();
}

void nina_dashboard_apply_theme(int theme_index) {
    current_theme = themes_get(theme_index);
    update_styles();

    if (!scr_dashboard) return;

    if (main_cont) {
        lv_obj_set_style_bg_color(main_cont, lv_color_hex(current_theme->bg_main), 0);
    }

    lv_obj_report_style_change(&style_bento_box);
    lv_obj_report_style_change(&style_label_small);
    lv_obj_report_style_change(&style_value_large);
    lv_obj_report_style_change(&style_header_gradient);

    for (int i = 0; i < page_count; i++) {
        apply_theme_to_page(&pages[i]);
    }

    /* Ported pages re-theme through registry ops. Each page's apply_theme
     * NULL-guards its own module state internally, so re-theming a currently
     * disabled optional page (AllSky/Spotify/Image Display) is safe — and
     * keeps the hidden-but-created page current so a later re-enable shows
     * the correct theme. Settings stays hand-dispatched (modal, lazy). */
    ops_apply_theme(PAGE_IDX_ALLSKY);
    ops_apply_theme(PAGE_IDX_JSON);
    ops_apply_theme(PAGE_IDX_HA);
    ops_apply_theme(PAGE_IDX_SPOTIFY);
    ops_apply_theme(PAGE_IDX_CLOCK);
    ops_apply_theme(PAGE_IDX_IMAGE_DISPLAY);
    ops_apply_theme(PAGE_IDX_SUMMARY);
    if (settings_obj) settings_tabview_apply_theme();
    ops_apply_theme(SYSINFO_PAGE_IDX(page_count));
    nina_graph_overlay_apply_theme();
    nina_info_overlay_apply_theme();
    nina_thumbnail_apply_theme();
    nina_toast_apply_theme();
    nina_event_log_apply_theme();
    nina_alerts_apply_theme();
    nina_safety_apply_theme();
    nina_ota_prompt_apply_theme();
    nina_wait_overlay_apply_theme();

    update_indicators();

    lv_obj_invalidate(scr_dashboard);
}

/* Go back to summary page when bottom row is clicked */
static void bottom_row_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    /* Task 4.1 / 6.1: route this USER tap through the arbiter like the other
     * USER-nav sites (swipe, button, /api/page, summary card). Commit
     * immediately for instant feedback AND record a USER claim so the grace
     * window protects it from being overridden by the next resolve(). */
    nina_dashboard_show_page_animated(PAGE_IDX_SUMMARY, 0, 0);
    nav_arbiter_submit_user(PAGE_IDX_SUMMARY, esp_timer_get_time() / 1000, -1);
}

/* Build all widgets for one dashboard page */
static void create_dashboard_page(dashboard_page_t *p, lv_obj_t *parent, int page_index) {
    memset(p, 0, sizeof(dashboard_page_t));

    p->page = lv_obj_create(parent);
    lv_obj_remove_style_all(p->page);
    lv_obj_set_size(p->page, SCREEN_SIZE - 2 * OUTER_PADDING, SCREEN_SIZE - 2 * OUTER_PADDING);
    lv_obj_set_style_pad_gap(p->page, GRID_GAP, 0);

    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {
        LV_GRID_FR(2), LV_GRID_FR(1), LV_GRID_FR(2),
        LV_GRID_FR(2), LV_GRID_FR(2), LV_GRID_FR(2),
        LV_GRID_TEMPLATE_LAST
    };
    lv_obj_set_layout(p->page, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(p->page, col_dsc, row_dsc);

    // Header (row 0, spans 2 cols)
    p->header_box = create_bento_box(p->page);
    lv_obj_set_grid_cell(p->header_box, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_flex_flow(p->header_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p->header_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(p->header_box, 12, 0);

    lv_obj_t *top_row = lv_obj_create(p->header_box);
    lv_obj_remove_style_all(top_row);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(top_row, LV_PCT(100));
    lv_obj_set_height(top_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    p->instance_name_glow = NULL;
    p->lbl_instance_name = lv_label_create(top_row);
    {
        uint32_t glow_color = 0xf87171;
        if (theme_is_red_night(current_theme)) {
            glow_color = current_theme->label_color;
        }
        lv_obj_set_style_text_color(p->lbl_instance_name, lv_color_hex(glow_color), 0);
    }
    lv_obj_set_style_text_font(p->lbl_instance_name, &lv_font_montserrat_20, 0);
    {
        const char *init_url = app_config_get_instance_url(page_index);
        char host[64] = {0};
        extract_host_from_url(init_url, host, sizeof(host));
        if (host[0] != '\0') {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s - Not Connected", host);
            lv_label_set_text(p->lbl_instance_name, buf);
        } else {
            lv_label_set_text(p->lbl_instance_name, "N.I.N.A.");
        }
    }

    p->lbl_target_name = lv_label_create(p->header_box);
    lv_obj_add_style(p->lbl_target_name, &style_value_large, 0);
    lv_obj_set_width(p->lbl_target_name, LV_PCT(100));
    lv_label_set_long_mode(p->lbl_target_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(p->lbl_target_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(p->lbl_target_name, "----");

    // Sequence info (row 1, spans 2 cols)
    lv_obj_t *box_seq = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_seq, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_flex_flow(box_seq, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_seq, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box_seq, 12, 0);
    lv_obj_add_flag(box_seq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_seq, sequence_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *seq_left = lv_obj_create(box_seq);
    lv_obj_remove_style_all(seq_left);
    lv_obj_clear_flag(seq_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(seq_left, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(seq_left, 1);

    lv_obj_t *lbl_seq_title = create_small_label(seq_left, "SEQUENCE");
    lv_obj_set_style_text_font(lbl_seq_title, &lv_font_montserrat_14, 0);

    p->lbl_seq_container = lv_label_create(seq_left);
    lv_obj_set_style_text_color(p->lbl_seq_container, lv_color_hex(theme_is_red_night(current_theme) ? current_theme->header_text_color : 0x4FC3F7), 0);
    lv_obj_set_style_text_font(p->lbl_seq_container, &lv_font_montserrat_24, 0);
    lv_label_set_text(p->lbl_seq_container, "----");

    // Safety monitor icon (centered in sequence row)
    extern const lv_font_t lv_font_material_safety;
    p->safety_icon = lv_label_create(box_seq);
    lv_obj_set_style_text_font(p->safety_icon, &lv_font_material_safety, 0);
    lv_obj_set_style_text_color(p->safety_icon, lv_color_hex(theme_is_red_night(current_theme) ? current_theme->label_color : 0x999999), 0);
    lv_label_set_text(p->safety_icon, "");
    lv_obj_clear_flag(p->safety_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(p->safety_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->safety_icon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *seq_right = lv_obj_create(box_seq);
    lv_obj_remove_style_all(seq_right);
    lv_obj_clear_flag(seq_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(seq_right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(seq_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(seq_right, 1);
    lv_obj_set_flex_align(seq_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_t *lbl_step_title = create_small_label(seq_right, "STEP");
    lv_obj_set_style_text_font(lbl_step_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl_step_title, LV_TEXT_ALIGN_RIGHT, 0);

    p->lbl_seq_step = lv_label_create(seq_right);
    lv_obj_set_style_text_color(p->lbl_seq_step, lv_color_hex(current_theme->text_color), 0);
    lv_obj_set_style_text_font(p->lbl_seq_step, &lv_font_montserrat_24, 0);
    lv_label_set_text(p->lbl_seq_step, "----");

    // Exposure arc (col 0, rows 2-4)
    lv_obj_t *box_exposure = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_exposure, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 3);
    lv_obj_set_flex_flow(box_exposure, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_exposure, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box_exposure, 0, 0);
    lv_obj_add_flag(box_exposure, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_exposure, exposure_arc_click_cb, LV_EVENT_CLICKED, NULL);

    p->arc_exposure = lv_arc_create(box_exposure);
    lv_obj_set_size(p->arc_exposure, 300, 300);
    lv_arc_set_rotation(p->arc_exposure, 270);
    lv_arc_set_bg_angles(p->arc_exposure, 0, 360);
    lv_arc_set_value(p->arc_exposure, 0);
    lv_arc_set_range(p->arc_exposure, 0, ARC_RANGE);
    lv_obj_remove_style(p->arc_exposure, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(current_theme->bg_main), LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->arc_exposure, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(p->arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(p->arc_exposure, 12, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(p->arc_exposure, lv_color_hex(current_theme->progress_color), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(p->arc_exposure, 16, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_spread(p->arc_exposure, 10, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(p->arc_exposure, LV_OPA_30, LV_PART_INDICATOR);
    lv_obj_clear_flag(p->arc_exposure, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *arc_center = lv_obj_create(p->arc_exposure);
    lv_obj_remove_style_all(arc_center);
    lv_obj_clear_flag(arc_center, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc_center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(arc_center);
    lv_obj_set_flex_flow(arc_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(arc_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(arc_center, 10, 0);

    // Line 1: Filter name × cycle progress (horizontal row)
    lv_obj_t *row_filter_cycle = lv_obj_create(arc_center);
    lv_obj_remove_style_all(row_filter_cycle);
    lv_obj_set_size(row_filter_cycle, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row_filter_cycle, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_filter_cycle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_filter_cycle, 6, 0);
    lv_obj_set_style_margin_bottom(row_filter_cycle, -4, 0);

    p->lbl_exposure_total = lv_label_create(row_filter_cycle);
    lv_obj_set_style_text_color(p->lbl_exposure_total, lv_color_hex(current_theme->filter_text_color), 0);
    lv_obj_set_style_text_font(p->lbl_exposure_total, &lv_font_montserrat_32, 0);
    lv_label_set_text(p->lbl_exposure_total, "");
    lv_obj_add_flag(p->lbl_exposure_total, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->lbl_exposure_total, filter_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(p->lbl_exposure_total, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);

    p->lbl_loop_count = lv_label_create(row_filter_cycle);
    lv_obj_set_style_text_color(p->lbl_loop_count, lv_color_hex(current_theme->label_color), 0);
    lv_obj_set_style_text_font(p->lbl_loop_count, &lv_font_montserrat_28, 0);
    lv_label_set_text(p->lbl_loop_count, "-- / --");

    // Line 2: Exposure duration
    p->lbl_exposure_current = lv_label_create(arc_center);
    lv_obj_add_style(p->lbl_exposure_current, &style_value_large, 0);
    lv_label_set_text(p->lbl_exposure_current, "----s");

    // Lines 3-4: Completed header + count/integration time
    p->row_filter_total = lv_obj_create(arc_center);
    lv_obj_remove_style_all(p->row_filter_total);
    lv_obj_set_size(p->row_filter_total, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->row_filter_total, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p->row_filter_total, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p->row_filter_total, 0, 0);
    lv_obj_set_style_margin_top(p->row_filter_total, 4, 0);
    lv_obj_add_flag(p->row_filter_total, LV_OBJ_FLAG_HIDDEN);

    p->lbl_filter_done_header = lv_label_create(p->row_filter_total);
    lv_obj_set_style_text_color(p->lbl_filter_done_header, lv_color_hex(current_theme->text_color), 0);
    lv_obj_set_style_text_font(p->lbl_filter_done_header, &lv_font_montserrat_16, 0);
    lv_label_set_text(p->lbl_filter_done_header, "COMPLETED");

    p->lbl_filter_done_value = lv_label_create(p->row_filter_total);
    lv_obj_set_style_text_color(p->lbl_filter_done_value, lv_color_hex(current_theme->filter_text_color), 0);
    lv_obj_set_style_text_font(p->lbl_filter_done_value, &lv_font_montserrat_32, 0);
    lv_label_set_text(p->lbl_filter_done_value, "");

    // RMS + HFR (col 1, row 2)
    lv_obj_t *box_rms_hfr = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_rms_hfr, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_set_flex_flow(box_rms_hfr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_rms_hfr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box_rms_hfr, 8, 0);

    lv_obj_t *box_rms = lv_obj_create(box_rms_hfr);
    lv_obj_remove_style_all(box_rms);
    lv_obj_set_size(box_rms, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_rms, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_rms, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box_rms, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_rms, rms_click_cb, LV_EVENT_CLICKED, NULL);

    p->lbl_rms_title = create_small_label(box_rms, "RMS");
    lv_obj_set_width(p->lbl_rms_title, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_rms_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_rms_title, lv_color_hex(current_theme->text_color), 0);

    p->lbl_rms_value = create_value_label(box_rms);
    lv_obj_set_style_text_color(p->lbl_rms_value, lv_color_hex(current_theme->rms_color), 0);
    lv_label_set_text(p->lbl_rms_value, "--");

    lv_obj_t *box_hfr = lv_obj_create(box_rms_hfr);
    lv_obj_remove_style_all(box_hfr);
    lv_obj_set_size(box_hfr, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_hfr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_hfr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box_hfr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_hfr, hfr_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(box_hfr, autofocus_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    p->lbl_hfr_title = create_small_label(box_hfr, "HFR");
    lv_obj_set_width(p->lbl_hfr_title, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_hfr_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_hfr_title, lv_color_hex(current_theme->text_color), 0);

    p->lbl_hfr_value = create_value_label(box_hfr);
    lv_obj_set_style_text_color(p->lbl_hfr_value, lv_color_hex(current_theme->hfr_color), 0);
    lv_label_set_text(p->lbl_hfr_value, "--");

    // Time to flip (col 1, row 3)
    lv_obj_t *box_flip = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_flip, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_set_flex_flow(box_flip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_flip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box_flip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_flip, flip_click_cb, LV_EVENT_CLICKED, NULL);

    p->lbl_flip_title = create_small_label(box_flip, "TIME UNTIL FLIP");
    lv_obj_set_width(p->lbl_flip_title, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_flip_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_flip_title, lv_color_hex(current_theme->text_color), 0);

    p->lbl_flip_value = create_value_label(box_flip);
    lv_label_set_text(p->lbl_flip_value, "--");

    // Target time + stars (col 1, row 4)
    lv_obj_t *box_sat_stars = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_sat_stars, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 4, 1);
    lv_obj_set_flex_flow(box_sat_stars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_sat_stars, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box_sat_stars, 8, 0);
    lv_obj_set_style_pad_all(box_sat_stars, 10, 0);

    lv_obj_t *box_target_time = lv_obj_create(box_sat_stars);
    lv_obj_remove_style_all(box_target_time);
    lv_obj_set_size(box_target_time, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_target_time, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_target_time, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box_target_time, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_target_time, session_stats_click_cb, LV_EVENT_CLICKED, NULL);

    p->lbl_target_time_header = create_small_label(box_target_time, "TIME LIMIT");
    lv_obj_set_width(p->lbl_target_time_header, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_target_time_header, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_target_time_header, lv_color_hex(current_theme->text_color), 0);

    p->lbl_target_time_value = create_value_label(box_target_time);
    lv_label_set_text(p->lbl_target_time_value, "--");

    lv_obj_t *box_stars = lv_obj_create(box_sat_stars);
    lv_obj_remove_style_all(box_stars);
    lv_obj_set_size(box_stars, LV_PCT(50), LV_PCT(100));
    lv_obj_set_flex_flow(box_stars, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box_stars, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box_stars, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_stars, stars_click_cb, LV_EVENT_CLICKED, NULL);

    p->lbl_stars_header = create_small_label(box_stars, "STARS");
    lv_obj_set_width(p->lbl_stars_header, LV_PCT(100));
    lv_obj_set_style_text_align(p->lbl_stars_header, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(p->lbl_stars_header, lv_color_hex(current_theme->text_color), 0);

    p->lbl_stars_value = create_value_label(box_stars);
    lv_label_set_text(p->lbl_stars_value, "--");

    // Power row (row 5, spans 2 cols)
    lv_obj_t *box_power = create_bento_box(p->page);
    lv_obj_set_grid_cell(box_power, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 5, 1);
    lv_obj_add_flag(box_power, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box_power, bottom_row_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(box_power, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_power, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box_power, 8, 0);

    for (int i = 0; i < MAX_POWER_WIDGETS; i++) {
        p->box_pwr[i] = lv_obj_create(box_power);
        lv_obj_remove_style_all(p->box_pwr[i]);
        lv_obj_clear_flag(p->box_pwr[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_grow(p->box_pwr[i], 1);
        lv_obj_set_height(p->box_pwr[i], LV_PCT(100));
        lv_obj_set_flex_flow(p->box_pwr[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(p->box_pwr[i], LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        p->lbl_pwr_title[i] = create_small_label(p->box_pwr[i], "--");
        lv_obj_set_width(p->lbl_pwr_title[i], LV_PCT(100));
        lv_obj_set_style_text_align(p->lbl_pwr_title[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(p->lbl_pwr_title[i], lv_color_hex(current_theme->text_color), 0);

        p->lbl_pwr_value[i] = create_value_label(p->box_pwr[i]);
        lv_label_set_text(p->lbl_pwr_value[i], "--");
#ifdef LV_FONT_MONTSERRAT_36
        lv_obj_set_style_text_font(p->lbl_pwr_value[i], &lv_font_montserrat_36, 0);
#elif defined(LV_FONT_MONTSERRAT_28)
        lv_obj_set_style_text_font(p->lbl_pwr_value[i], &lv_font_montserrat_28, 0);
#elif defined(LV_FONT_MONTSERRAT_24)
        lv_obj_set_style_text_font(p->lbl_pwr_value[i], &lv_font_montserrat_24, 0);
#endif

        lv_obj_add_flag(p->box_pwr[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Stale data indicator — floating label in upper-right corner */
    p->lbl_stale = lv_label_create(p->page);
    lv_obj_add_flag(p->lbl_stale, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_color(p->lbl_stale, lv_color_hex(theme_is_red_night(current_theme) ? current_theme->text_color : 0xfbbf24), 0);  /* amber; bright red under Red Night */
    lv_obj_set_style_text_font(p->lbl_stale, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(p->lbl_stale, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p->lbl_stale, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(p->lbl_stale, 8, 0);
    lv_obj_set_style_pad_ver(p->lbl_stale, 4, 0);
    lv_obj_set_style_radius(p->lbl_stale, 6, 0);
    lv_obj_align(p->lbl_stale, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(p->lbl_stale, LV_OBJ_FLAG_HIDDEN);

    /* Stale overlay — semi-transparent dim for heavily stale data (> 2 min) */
    p->stale_overlay = lv_obj_create(p->page);
    lv_obj_remove_style_all(p->stale_overlay);
    lv_obj_add_flag(p->stale_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(p->stale_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p->stale_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p->stale_overlay, LV_OPA_40, 0);
    lv_obj_add_flag(p->stale_overlay, LV_OBJ_FLAG_HIDDEN);

    /* Disconnected full-screen branded overlay (IDLE-04, Plan 02).
     * Floats over the bento grid; hidden initially; shown by update_disconnected_state.
     * Title includes the configured hostname so the user knows which node is offline. */
    {
        char host[64] = {0};
        extract_host_from_url(app_config_get_instance_url(page_index), host, sizeof(host));
        char offline_title[96];
        if (host[0]) {
            snprintf(offline_title, sizeof(offline_title), "%s Offline", host);
        } else {
            snprintf(offline_title, sizeof(offline_title), "Node %d Offline", page_index + 1);
        }
        p->empty_state_cont = nina_empty_state_create(p->page,
                                                       ICON_CLOUD_OFF,
                                                       offline_title,
                                                       "Verify NINA is running on host",
                                                       0);
        if (p->empty_state_cont) {
            /* Make it a full-coverage floating sibling (mirrors stale_overlay pattern). */
            lv_obj_add_flag(p->empty_state_cont, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_size(p->empty_state_cont, LV_PCT(100), LV_PCT(100));
            lv_obj_set_pos(p->empty_state_cont, 0, 0);
            /* Opaque black backdrop so the bento grid does not bleed through
             * (BUG-1 fix: mirrors stale_overlay bg_color/bg_opa pattern). */
            lv_obj_set_style_bg_color(p->empty_state_cont, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(p->empty_state_cont, LV_OPA_COVER, 0);
            /* Must not consume bento-grid info-overlay taps on reconnect. */
            lv_obj_remove_flag(p->empty_state_cont, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    p->arc_completing = false;
    p->cached_end_epoch = 0;
    p->cached_total = 0;
    p->gap_start_epoch = 0;
    p->cached_nina_epoch = 0;
    p->cached_nina_mono_us = 0;
}

/* Page indicator dots at the bottom */
static void create_page_indicator(lv_obj_t *parent, int count) {
    /* Idempotent on re-call: tear down any prior strip first so repeated
     * enable/disable cycles do not leak internal heap. Deleting indicator_cont
     * deletes its child dots in one call. */
    if (indicator_cont) {
        lv_obj_delete(indicator_cont);   /* deletes children dots too */
        indicator_cont = NULL;
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) indicator_dots[i] = NULL;
    }

    if (count <= 1) return;

    indicator_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(indicator_cont);
    lv_obj_set_size(indicator_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(indicator_cont, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(indicator_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(indicator_cont, 8, 0);

    /* Start hidden — summary page (PAGE_IDX_SUMMARY) is the default boot page */
    lv_obj_add_flag(indicator_cont, LV_OBJ_FLAG_HIDDEN);

    int gb = app_config_get()->color_brightness;

    for (int i = 0; i < count; i++) {
        indicator_dots[i] = lv_obj_create(indicator_cont);
        lv_obj_remove_style_all(indicator_dots[i]);
        lv_obj_set_size(indicator_dots[i], 10, 10);
        lv_obj_set_style_radius(indicator_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(indicator_dots[i], LV_OPA_COVER, 0);

        /* All dots start inactive (no NINA page selected) */
        uint32_t dot_color = app_config_apply_brightness(current_theme->label_color, gb);
        lv_obj_set_style_bg_color(indicator_dots[i], lv_color_hex(dot_color), 0);
    }
}

void nina_dashboard_set_page_change_cb(nina_page_change_cb_t cb) {
    page_change_cb = cb;
}

/* Any touch on the screen — sets flag to wake from screen sleep */
static void screen_press_cb(lv_event_t *e) {
    LV_UNUSED(e);
    screen_touch_wake = true;
}

/* Swipe left/right to change pages (cycles through NINA + sysinfo) */
static void gesture_event_cb(lv_event_t *e) {
    if (total_page_count <= 1) return;
    /* If the wait/loading overlay is visible, a swipe cancels the load and
     * returns to the prior page via the arbiter.  Return immediately so the
     * normal page-swipe computation does not also run. */
    if (nina_wait_overlay_visible()) {
        nina_wait_overlay_cancel();
        return;
    }
    if (thumbnail_overlay && !lv_obj_has_flag(thumbnail_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (nina_graph_visible()) return;
    if (nina_info_overlay_visible()) return;

    /* On the Moon page, a deliberate drag-to-rotate must not also flip pages.
     * The Moon touch handlers (nina_image_display.c) only run when the active
     * page is the Image Display page AND the source is Moon, so moon_drag_was_rotate()
     * can only be true in that case. A clean quick flick (little finger travel)
     * leaves was_rotate false and still navigates. */
    if (active_page == PAGE_IDX_IMAGE_DISPLAY && image_display_obj != NULL &&
        image_source_get_effective() == 1 && moon_drag_was_rotate()) {
        return;
    }

    /* Block screen-level swipe on settings page — sliders need horizontal drag.
     * Only the header widget on the settings page handles page-switch gestures. */
    if (active_page == SETTINGS_PAGE_IDX(page_count)) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    int new_page = active_page;

    if (dir == LV_DIR_LEFT) {
        for (int step = 1; step < total_page_count; step++) {
            int candidate = (active_page + step) % total_page_count;
            if (candidate == SETTINGS_PAGE_IDX(page_count)) continue;
            /* Optional pages (AllSky/Spotify/Image Display) are skipped when
             * unavailable; availability comes from their registered ops
             * (get_obj() == NULL while disabled). */
            if ((candidate == PAGE_IDX_ALLSKY ||
                 candidate == PAGE_IDX_SPOTIFY ||
                 candidate == PAGE_IDX_IMAGE_DISPLAY) &&
                !page_ops_is_available(candidate)) continue;
            new_page = candidate;
            break;
        }
    } else if (dir == LV_DIR_RIGHT) {
        for (int step = 1; step < total_page_count; step++) {
            int candidate = (active_page - step + total_page_count) % total_page_count;
            if (candidate == SETTINGS_PAGE_IDX(page_count)) continue;
            if ((candidate == PAGE_IDX_ALLSKY ||
                 candidate == PAGE_IDX_SPOTIFY ||
                 candidate == PAGE_IDX_IMAGE_DISPLAY) &&
                !page_ops_is_available(candidate)) continue;
            new_page = candidate;
            break;
        }
    } else {
        return;
    }

    /* Task 4.1: route USER nav through the arbiter. Commit immediately for
     * instant swipe feedback AND record a USER claim so the grace window
     * (nav_grace_s) protects this page from lower-priority sources until the
     * next resolve(). */
    if (new_page == PAGE_IDX_IMAGE_DISPLAY) {
        /* Clear any stale slideshow image-source override BEFORE the page
         * becomes active, so the image page renders the correct source on the
         * very first goes_poll_task iteration. nav_arbiter_submit_user() below
         * also clears it, but only after the page is already visible, leaving a
         * one-frame window of the wrong source. */
        image_source_set_override(-1);
    }
    nina_dashboard_show_page_animated(new_page, 0, 0);
    nav_arbiter_submit_user(new_page, esp_timer_get_time() / 1000, -1);
}

/* Target name: click to request thumbnail */
static void target_name_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_thumbnail_request();
}

/* RMS widget: click to open RMS graph overlay */
static void rms_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_graph_show(GRAPH_TYPE_RMS, active_page);
}

/* HFR widget: click to open HFR graph overlay */
static void hfr_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_graph_show(GRAPH_TYPE_HFR, active_page);
}

/* Exposure arc: click to open camera + weather info overlay */
static void exposure_arc_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_show(INFO_OVERLAY_CAMERA, active_page);
}

/* Flip time box: click to open mount position info overlay */
static void flip_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_show(INFO_OVERLAY_MOUNT, active_page);
}

/* Stars box: click to open image statistics info overlay */
static void stars_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_show(INFO_OVERLAY_IMAGESTATS, active_page);
}

/* Sequence box: click to open sequence details info overlay */
static void sequence_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_show(INFO_OVERLAY_SEQUENCE, active_page);
}

/* Filter label: click to open filter wheel info overlay */
static void filter_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_show(INFO_OVERLAY_FILTER, active_page);
}

/* HFR box: long-press to open autofocus V-curve overlay */
static void autofocus_long_press_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_show(INFO_OVERLAY_AUTOFOCUS, active_page);
}

/* Time remaining box: click to open session statistics overlay */
static void session_stats_click_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_info_overlay_show(INFO_OVERLAY_SESSION_STATS, active_page);
}

/* True iff NINA instance has a non-empty URL and is enabled in config */
static bool slot_is_available_cfg(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return false;
    const char *url = app_config_get_instance_url(instance);
    return (url[0] != '\0') && app_config_is_instance_enabled(instance);
}

/* Set up the dashboard with one page per NINA instance */
void create_nina_dashboard(lv_obj_t *parent, int instance_count) {
    app_config_t *cfg = app_config_get();
    current_theme = themes_get(cfg->theme_index);
    update_styles();

    scr_dashboard = parent;

    /* Clear stale idle indicator pointers before creating new pages */
    nina_idle_indicator_reset();

    /* Reserved fixed NINA index band: the band is always MAX_NINA_INSTANCES wide.
     * Slot i always maps to instance i at absolute index NINA_PAGE_OFFSET + i.
     * page_count is the constant band width so SETTINGS_PAGE_IDX / SYSINFO_PAGE_IDX
     * never shift with the enabled count. There is NO floor to 1; a disabled or
     * URL-less instance simply has an unavailable (NULL) slot. */
    page_count = MAX_NINA_INSTANCES;        /* constant band width */
    nina_available_count = 0;
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        nina_slot_available[i] = slot_is_available_cfg(i);
        if (nina_slot_available[i]) nina_available_count++;
    }
    active_page = PAGE_IDX_SUMMARY;         /* Summary is the boot default */

    main_cont = lv_obj_create(scr_dashboard);
    lv_obj_remove_style_all(main_cont);
    lv_obj_set_size(main_cont, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(main_cont, lv_color_hex(current_theme->bg_main), 0);
    lv_obj_set_style_bg_opa(main_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(main_cont, OUTER_PADDING, 0);

    /* AllSky page — PAGE_IDX_ALLSKY, always created but hidden initially.
     * allsky_obj is set to NULL when disabled to remove from navigation.
     * Ops are registered BEFORE create so ops-based dispatch is live for
     * every subsequent reference to the page (same for all pages below). */
    page_registry_set_ops(PAGE_REF_ALLSKY, &s_allsky_page_ops);
    allsky_page_created = s_allsky_page_ops.create(main_cont);
    lv_obj_add_flag(allsky_page_created, LV_OBJ_FLAG_HIDDEN);
    allsky_obj = app_config_get()->allsky_enabled ? allsky_page_created : NULL;

    /* JSON Display page — PAGE_IDX_JSON, always created but hidden initially.
     * json_obj is set to NULL when disabled to remove from navigation. */
    page_registry_set_ops(PAGE_REF_JSON, &s_json_page_ops);
    json_page_created = s_json_page_ops.create(main_cont);
    lv_obj_add_flag(json_page_created, LV_OBJ_FLAG_HIDDEN);
    json_obj = app_config_get()->json_enabled ? json_page_created : NULL;

    /* Home Assistant page — PAGE_IDX_HA, always created but hidden initially.
     * ha_obj is set to NULL when disabled to remove from navigation. */
    page_registry_set_ops(PAGE_REF_HA, &s_ha_page_ops);
    ha_page_created = s_ha_page_ops.create(main_cont);
    lv_obj_add_flag(ha_page_created, LV_OBJ_FLAG_HIDDEN);
    ha_obj = app_config_get()->ha_enabled ? ha_page_created : NULL;

    /* Spotify page — PAGE_IDX_SPOTIFY, always created but hidden initially.
     * spotify_obj is set to NULL when disabled to remove from navigation. */
    page_registry_set_ops(PAGE_REF_SPOTIFY, &s_spotify_page_ops);
    spotify_page_created = s_spotify_page_ops.create(main_cont);
    lv_obj_add_flag(spotify_page_created, LV_OBJ_FLAG_HIDDEN);
    spotify_obj = app_config_get()->spotify_enabled ? spotify_page_created : NULL;

    /* Clock page — PAGE_IDX_CLOCK, always present, hidden initially. */
    page_registry_set_ops(PAGE_REF_CLOCK, &s_clock_page_ops);
    clock_obj = s_clock_page_ops.create(main_cont);
    lv_obj_add_flag(clock_obj, LV_OBJ_FLAG_HIDDEN);

    /* Image Display page — PAGE_IDX_IMAGE_DISPLAY, always created but hidden initially.
     * image_display_obj is set to NULL when disabled to remove from navigation.
     * Registered under PAGE_REF_IMG_DEFAULT (the page itself, source-agnostic). */
    page_registry_set_ops(PAGE_REF_IMG_DEFAULT, &s_image_display_page_ops);
    image_display_page_created = s_image_display_page_ops.create(main_cont);
    lv_obj_add_flag(image_display_page_created, LV_OBJ_FLAG_HIDDEN);
    image_display_obj = app_config_get()->image_display_enabled ? image_display_page_created : NULL;

    /* Summary page — PAGE_IDX_SUMMARY, visible by default */
    page_registry_set_ops(PAGE_REF_SUMMARY, &s_summary_page_ops);
    summary_obj = s_summary_page_ops.create(main_cont);

    /* NINA instance pages — absolute index NINA_PAGE_OFFSET + i, hidden initially.
     * Slot i maps to instance i (fixed identity). Index positions are reserved,
     * but the LVGL page object is created ONLY for an available slot; an
     * unavailable slot is left NULL so the internal-heap footprint stays
     * proportional to enabled instances. The arc timer is created only when the
     * page exists. */
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (!nina_slot_available[i]) {
            pages[i].page = NULL;
            pages[i].arc_timer = NULL;
            continue;
        }
        create_dashboard_page(&pages[i], main_cont, i);
        lv_obj_add_flag(pages[i].page, LV_OBJ_FLAG_HIDDEN);
        pages[i].arc_timer = lv_timer_create(arc_interp_timer_cb, ARC_TIMER_MS, &pages[i]);
    }

    /* Settings page — lazy-loaded on demand (SETTINGS_PAGE_IDX).
     * NOT created at boot to save internal heap for OTA task. */
    settings_obj = NULL;

    /* System info page — always last (SYSINFO_PAGE_IDX), hidden initially */
    page_registry_set_ops(PAGE_REF_SYSINFO, &s_sysinfo_page_ops);
    sysinfo_obj = s_sysinfo_page_ops.create(main_cont);
    lv_obj_add_flag(sysinfo_obj, LV_OBJ_FLAG_HIDDEN);
    total_page_count = page_count + EXTRA_PAGES;  /* allsky + spotify + clock + image_display + json + summary + NINA pages + settings + sysinfo */

    /* Page indicator dots — one dot per available NINA slot (not allsky, spotify, summary, settings, or sysinfo) */
    create_page_indicator(scr_dashboard, nina_available_count);

    /* Always enable swipe gestures */
    lv_obj_add_event_cb(scr_dashboard, gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_dashboard, screen_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_clear_flag(scr_dashboard, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Graph overlay (on top of pages, below thumbnail)
    nina_graph_overlay_create(scr_dashboard);

    // Info overlay (on top of pages, below thumbnail)
    nina_info_overlay_create(scr_dashboard);

    // Thumbnail overlay (on top of pages)
    nina_thumbnail_create(scr_dashboard);

    // OTA update prompt overlay (on top of everything)
    nina_ota_prompt_create(scr_dashboard);

    // Generic wait/loading overlay (created last so it sits on top)
    nina_wait_overlay_create(scr_dashboard);

    // Make header box clickable on available pages to open thumbnail
    for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
        if (pages[i].page && pages[i].header_box) {
            lv_obj_add_flag(pages[i].header_box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(pages[i].header_box, target_name_click_cb, LV_EVENT_CLICKED, NULL);
        }
    }

    nina_dashboard_apply_theme(cfg->theme_index);
}

bool nina_dashboard_slot_available(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return false;
    return nina_slot_available[instance];
}

void nina_dashboard_rebuild_slot(int instance) {
    if (instance < 0 || instance >= MAX_NINA_INSTANCES) return;
    bool want = slot_is_available_cfg(instance);
    bool have = nina_slot_available[instance];
    if (want == have) return;                 /* nothing to do */

    if (want && !have) {
        /* Create the slot's page + arc timer */
        create_dashboard_page(&pages[instance], main_cont, instance);
        lv_obj_add_flag(pages[instance].page, LV_OBJ_FLAG_HIDDEN);
        pages[instance].arc_timer =
            lv_timer_create(arc_interp_timer_cb, ARC_TIMER_MS, &pages[instance]);
        if (pages[instance].header_box) {
            lv_obj_add_flag(pages[instance].header_box, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(pages[instance].header_box,
                                target_name_click_cb, LV_EVENT_CLICKED, NULL);
        }
        nina_slot_available[instance] = true;
        nina_available_count++;
    } else {
        /* Destroy the slot's page + arc timer */
        if (active_page == NINA_PAGE_OFFSET + instance) {
            /* Pre-move off the dying page; the arbiter re-resolves next tick. */
            nina_dashboard_show_page(PAGE_IDX_SUMMARY, total_page_count);
        }
        if (pages[instance].arc_timer) {
            lv_timer_delete(pages[instance].arc_timer);
            pages[instance].arc_timer = NULL;
        }
        if (pages[instance].page) {
            lv_obj_delete(pages[instance].page);
            pages[instance].page = NULL;
        }
        nina_slot_available[instance] = false;
        if (nina_available_count > 0) nina_available_count--;
    }

    /* page_count (band width) and total_page_count are CONSTANT under the
     * reserved band, so SETTINGS/SYSINFO indices do not move. Only the dot
     * strip (sized to the available count) and the summary need a refresh. */
    create_page_indicator(scr_dashboard, nina_available_count);  /* rebuilds dots */
    summary_page_rebuild();                                      /* Task 1.3c */
    update_indicators();
}

void nina_dashboard_show_page(int page_index, int instance_count) {
    if (page_index < 0 || page_index >= total_page_count) return;
    /* Optional pages: reject navigation to a disabled page (ops availability,
     * derived from ops get_obj() == NULL while disabled). */
    if ((page_index == PAGE_IDX_ALLSKY ||
         page_index == PAGE_IDX_SPOTIFY ||
         page_index == PAGE_IDX_IMAGE_DISPLAY) &&
        !page_ops_is_available(page_index)) return;
    if (page_index == active_page) return;

    hide_page_at(active_page);
    active_page = page_index;
    show_page_at(active_page);

    update_indicators();

    if (page_change_cb) {
        page_change_cb(active_page);
    }
}

int nina_dashboard_get_active_page(void) {
    return active_page;
}

bool nina_dashboard_is_allsky_page(void) {
    return allsky_obj != NULL && active_page == PAGE_IDX_ALLSKY;
}

void nina_dashboard_set_allsky_enabled(bool enabled) {
    if (enabled) {
        allsky_obj = allsky_page_created;
    } else {
        /* If currently viewing the AllSky page, switch to summary first */
        if (active_page == PAGE_IDX_ALLSKY && allsky_obj != NULL) {
            nina_dashboard_show_page(PAGE_IDX_SUMMARY, total_page_count);
        }
        if (allsky_page_created) {
            lv_obj_add_flag(allsky_page_created, LV_OBJ_FLAG_HIDDEN);
        }
        allsky_obj = NULL;
    }
}

bool nina_dashboard_is_json_page(void) {
    return json_obj != NULL && active_page == PAGE_IDX_JSON;
}

void nina_dashboard_set_json_enabled(bool enabled) {
    if (enabled) {
        json_obj = json_page_created;
    } else {
        /* If currently viewing the JSON page, switch to summary first */
        if (active_page == PAGE_IDX_JSON && json_obj != NULL) {
            nina_dashboard_show_page(PAGE_IDX_SUMMARY, total_page_count);
        }
        if (json_page_created) {
            lv_obj_add_flag(json_page_created, LV_OBJ_FLAG_HIDDEN);
        }
        json_obj = NULL;
    }
}

bool nina_dashboard_is_ha_page(void) {
    return ha_obj != NULL && active_page == PAGE_IDX_HA;
}

void nina_dashboard_set_ha_enabled(bool enabled) {
    if (enabled) {
        ha_obj = ha_page_created;
    } else {
        /* If currently viewing the HA page, switch to summary first */
        if (active_page == PAGE_IDX_HA && ha_obj != NULL) {
            nina_dashboard_show_page(PAGE_IDX_SUMMARY, total_page_count);
        }
        if (ha_page_created) {
            lv_obj_add_flag(ha_page_created, LV_OBJ_FLAG_HIDDEN);
        }
        ha_obj = NULL;
    }
}

bool nina_dashboard_is_spotify_page(void) {
    return spotify_obj != NULL && active_page == PAGE_IDX_SPOTIFY;
}

void nina_dashboard_set_spotify_enabled(bool enabled) {
    if (enabled) {
        spotify_obj = spotify_page_created;
    } else {
        /* If currently viewing the Spotify page, switch to summary first */
        if (active_page == PAGE_IDX_SPOTIFY && spotify_obj != NULL) {
            nina_dashboard_show_page(PAGE_IDX_SUMMARY, total_page_count);
        }
        if (spotify_page_created) {
            lv_obj_add_flag(spotify_page_created, LV_OBJ_FLAG_HIDDEN);
        }
        spotify_obj = NULL;
    }
}

bool nina_dashboard_is_image_display_page(void) {
    return image_display_obj != NULL && active_page == PAGE_IDX_IMAGE_DISPLAY;
}

void nina_dashboard_set_image_display_enabled(bool enabled) {
    if (enabled) {
        image_display_obj = image_display_page_created;
    } else {
        /* If currently viewing the Image Display page, switch to summary first */
        if (active_page == PAGE_IDX_IMAGE_DISPLAY && image_display_obj != NULL) {
            nina_dashboard_show_page(PAGE_IDX_SUMMARY, total_page_count);
        }
        if (image_display_page_created) {
            lv_obj_add_flag(image_display_page_created, LV_OBJ_FLAG_HIDDEN);
        }
        image_display_obj = NULL;
    }
}

bool nina_dashboard_is_clock_page(void) {
    return active_page == PAGE_IDX_CLOCK;
}

bool nina_dashboard_is_settings_page(void) {
    return active_page == SETTINGS_PAGE_IDX(page_count);
}

bool nina_dashboard_is_sysinfo_page(void) {
    return active_page == SYSINFO_PAGE_IDX(page_count);
}

bool nina_dashboard_is_summary_page(void) {
    return active_page == PAGE_IDX_SUMMARY;
}

int nina_dashboard_get_total_page_count(void) {
    return total_page_count;
}

/* Pure-offset inverse of instance_to_page. Input is an ABSOLUTE page index.
 * Availability is a SEPARATE query (nina_slot_available[]); this maps the index
 * even for an unavailable slot so callers can map then test availability. */
int nina_dashboard_page_to_instance(int abs_page_idx) {
    int inst = abs_page_idx - NINA_PAGE_OFFSET;
    if (inst < 0 || inst >= MAX_NINA_INSTANCES) return -1;
    return inst;
}

/* Pure-offset inverse of page_to_instance. Returns the ABSOLUTE page index for
 * an instance, or -1 only for an out-of-range instance index. */
int nina_dashboard_instance_to_page(int instance_idx) {
    if (instance_idx < 0 || instance_idx >= MAX_NINA_INSTANCES) return -1;
    return NINA_PAGE_OFFSET + instance_idx;
}

static int next_page_index = -1;
static int slide_old_page_idx = -1;

static void fade_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void slide_x_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0);
}

static void update_indicators(void)
{
    if (!indicator_cont) return;

    int gb = app_config_get()->color_brightness;
    /* Dots are created one per AVAILABLE NINA slot (nina_available_count). Walk
     * the fixed-identity slots and map each created dot to its instance. */
    int dot = 0;
    for (int inst = 0; inst < MAX_NINA_INSTANCES; inst++) {
        if (!nina_slot_available[inst]) continue;
        if (indicator_dots[dot]) {
            uint32_t dot_color = (active_page == NINA_PAGE_OFFSET + inst)
                ? app_config_apply_brightness(current_theme->text_color, gb)
                : app_config_apply_brightness(current_theme->label_color, gb);
            lv_obj_set_style_bg_color(indicator_dots[dot], lv_color_hex(dot_color), 0);
        }
        dot++;
    }
    bool on_nina = (abs_page_to_instance(active_page) >= 0);
    if (on_nina) lv_obj_clear_flag(indicator_cont, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(indicator_cont, LV_OBJ_FLAG_HIDDEN);
}

static void fade_out_ready_cb(lv_anim_t * a)
{
    /* Hide the old page (which just finished fading out) */
    hide_page_at(active_page);

    /* Ensure opacity is reset for the old page so it's ready for next time */
    lv_obj_t *old_obj = get_page_obj(active_page);
    if (old_obj) lv_obj_set_style_opa(old_obj, LV_OPA_COVER, 0);

    /* Switch to the new page */
    if (next_page_index >= 0) {
        active_page = next_page_index;
        next_page_index = -1;
    }

    /* Update indicators for the new page */
    update_indicators();

    if (page_change_cb) page_change_cb(active_page);

    /* Fade-in the new page */
    lv_obj_t *new_obj = get_page_obj(active_page);
    if (new_obj) {
        lv_obj_set_style_opa(new_obj, LV_OPA_TRANSP, 0);
        show_page_at(active_page);

        lv_anim_t a_in;
        lv_anim_init(&a_in);
        lv_anim_set_exec_cb(&a_in, fade_anim_cb);
        lv_anim_set_var(&a_in, new_obj);
        lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a_in, 500); /* 500ms fade in */
        lv_anim_start(&a_in);
    }
}

static void slide_new_ready_cb(lv_anim_t *a)
{
    /* New page has arrived at x=0 — clean up the old page */
    if (slide_old_page_idx >= 0) {
        lv_obj_t *old_obj = get_page_obj(slide_old_page_idx);
        if (old_obj) {
            lv_obj_add_flag(old_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_translate_x(old_obj, 0, 0);
        }
        slide_old_page_idx = -1;
    }

    /* Ensure new page transform is fully reset */
    lv_obj_t *new_obj = (lv_obj_t *)a->var;
    if (new_obj) {
        lv_obj_set_style_translate_x(new_obj, 0, 0);
    }

    update_indicators();

    if (page_change_cb) page_change_cb(active_page);
}

void nina_dashboard_show_page_animated(int page_index, int instance_count, int effect)
{
    if (page_index < 0 || page_index >= total_page_count) return;
    /* Optional pages: reject navigation to a disabled page (ops availability,
     * same condition as nina_dashboard_show_page above). */
    if ((page_index == PAGE_IDX_ALLSKY ||
         page_index == PAGE_IDX_SPOTIFY ||
         page_index == PAGE_IDX_IMAGE_DISPLAY) &&
        !page_ops_is_available(page_index)) return;
    if (page_index == active_page) return;

    lv_obj_t *old_obj = get_page_obj(active_page);
    lv_obj_t *upcoming_obj = get_page_obj(page_index);

    /* Cancel any in-progress animations on both pages to prevent stacking */
    if (old_obj) lv_anim_delete(old_obj, NULL);
    if (upcoming_obj) lv_anim_delete(upcoming_obj, NULL);

    /* Also cancel animation on a previously sliding-out page if still in flight */
    if (slide_old_page_idx >= 0 && slide_old_page_idx != active_page) {
        lv_obj_t *prev_old = get_page_obj(slide_old_page_idx);
        if (prev_old) {
            lv_anim_delete(prev_old, NULL);
            lv_obj_add_flag(prev_old, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_translate_x(prev_old, 0, 0);
        }
        slide_old_page_idx = -1;
    }

    /* Reset opacity/translate on both objects in case a prior animation was interrupted */
    if (old_obj) {
        lv_obj_set_style_opa(old_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_translate_x(old_obj, 0, 0);
    }
    if (upcoming_obj) {
        lv_obj_set_style_opa(upcoming_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_translate_x(upcoming_obj, 0, 0);
    }

    if (effect == 1 && old_obj) {
        /* Fade-out: start opaque, animate to transparent */
        next_page_index = page_index;

        /* Ensure opacity is correct before starting */
        lv_obj_set_style_opa(old_obj, LV_OPA_COVER, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_exec_cb(&a, fade_anim_cb);
        lv_anim_set_var(&a, old_obj);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, 500); /* 500ms fade out */
        lv_anim_set_ready_cb(&a, fade_out_ready_cb);
        lv_anim_start(&a);
    } else if ((effect == 2 || effect == 3) && old_obj) {
        /* Slide left (2) or slide right (3) */
        int slide_dist = SCREEN_SIZE;
        /* effect 2 = slide-left: old goes left, new enters from right
         * effect 3 = slide-right: old goes right, new enters from left */
        int old_end_x   = (effect == 2) ? -slide_dist : slide_dist;
        int new_start_x = (effect == 2) ? slide_dist  : -slide_dist;

        lv_obj_t *new_obj = get_page_obj(page_index);
        if (!new_obj) {
            /* Fallback to instant if page doesn't exist */
            goto instant;
        }

        slide_old_page_idx = active_page;
        active_page = page_index;

        /* Position new page off-screen BEFORE showing it (avoids flash) */
        lv_obj_set_style_translate_x(new_obj, new_start_x, 0);
        show_page_at(page_index);

        /* Animate old page out */
        lv_anim_t a_old;
        lv_anim_init(&a_old);
        lv_anim_set_var(&a_old, old_obj);
        lv_anim_set_exec_cb(&a_old, slide_x_anim_cb);
        lv_anim_set_values(&a_old, 0, old_end_x);
        lv_anim_set_duration(&a_old, 500);
        lv_anim_set_path_cb(&a_old, lv_anim_path_ease_in_out);
        lv_anim_start(&a_old);

        /* Animate new page in */
        lv_anim_t a_new;
        lv_anim_init(&a_new);
        lv_anim_set_var(&a_new, new_obj);
        lv_anim_set_exec_cb(&a_new, slide_x_anim_cb);
        lv_anim_set_values(&a_new, new_start_x, 0);
        lv_anim_set_duration(&a_new, 500);
        lv_anim_set_path_cb(&a_new, lv_anim_path_ease_in_out);
        lv_anim_set_ready_cb(&a_new, slide_new_ready_cb);
        lv_anim_start(&a_new);
    } else {
instant:
        /* Instant switch */
        hide_page_at(active_page);
        active_page = page_index;
        show_page_at(active_page);

        /* Ensure opacity and translate are reset if we switched back from a halfway animation */
        lv_obj_t *new_obj = get_page_obj(active_page);
        if (new_obj) {
             lv_obj_set_style_opa(new_obj, LV_OPA_COVER, 0);
             lv_obj_set_style_translate_x(new_obj, 0, 0);
        }

        update_indicators();

        if (page_change_cb) page_change_cb(active_page);
    }
}
