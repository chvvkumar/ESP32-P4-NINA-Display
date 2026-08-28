/**
 * @file nina_net_debug.c
 * @brief NET TRACE overlay: header stats, TX sparkline with MARK cursors,
 *        scheduler countdown table and a burst ticker over the net_trace ring.
 *
 * Built lazily on lv_layer_top() at show(); every widget is destroyed on hide()
 * so the overlay costs nothing while closed. Modal open/close notifications to
 * the navigation arbiter are paired on the create/destroy edges, exactly like
 * nina_info_overlay.c pairs them on the hidden/visible edges.
 */

#include "nina_net_debug.h"
#include "nina_dashboard_internal.h"
#include "nina_info_internal.h"
#include "nina_nav_arbiter.h"
#include "net_trace.h"
#include "app_config.h"
#include "themes.h"
#include "ui_helpers.h"
#include "ui_round.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define ND_TICK_MS      250
#define ND_CURSORS      4
#define ND_TICKER_ROWS  12
#define ND_BURST_MS     300
#define ND_CHART_H      140
#define ND_SCHED_H      215

/* Widgets */
static lv_obj_t *nd_root      = NULL;
static lv_obj_t *nd_lbl_title = NULL;
static lv_obj_t *nd_lbl_stats = NULL;
static lv_obj_t *nd_lbl_state = NULL;
static lv_obj_t *nd_chart     = NULL;
static lv_obj_t *nd_lbl_peak  = NULL;
static lv_obj_t *nd_lbl_sched = NULL;   /* countdown table, left column */
static lv_obj_t *nd_lbl_sched2 = NULL;  /* right column */
static lv_obj_t *nd_lbl_tick  = NULL;
static lv_obj_t *nd_btn_back  = NULL;
static lv_obj_t *nd_btn_lbl   = NULL;
static lv_chart_series_t *nd_ser = NULL;
static lv_chart_cursor_t *nd_cur[ND_CURSORS];
static lv_timer_t *nd_timer   = NULL;

static bool     nd_frozen     = false;
static uint32_t nd_freeze_ms  = 0;

/* Scratch (static, PSRAM: never on the LVGL timer stack) */
EXT_RAM_BSS_ATTR static uint16_t nd_tx[NET_TRACE_BINS];
EXT_RAM_BSS_ATTR static uint16_t nd_rx[NET_TRACE_BINS];
EXT_RAM_BSS_ATTR static int32_t  nd_vals[NET_TRACE_BINS];
EXT_RAM_BSS_ATTR static net_sched_t nd_sched[NET_TRACE_MAX_SCHED];
EXT_RAM_BSS_ATTR static char     nd_buf[2048];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Append to nd_buf; saturates at the buffer end so a full buffer never underflows the size. */
static size_t nd_app(size_t off, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static size_t nd_app(size_t off, const char *fmt, ...) {
    if (off >= sizeof(nd_buf) - 1) return sizeof(nd_buf) - 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(nd_buf + off, sizeof(nd_buf) - off, fmt, ap);
    va_end(ap);
    if (n < 0) return off;
    off += (size_t)n;
    return off < sizeof(nd_buf) - 1 ? off : sizeof(nd_buf) - 1;
}

static uint32_t nd_col(uint32_t c) {
    return app_config_apply_brightness(c, app_config_get()->color_brightness);
}
static uint32_t nd_text(void)   { return nd_col(current_theme ? current_theme->text_color : 0xffffff); }
static uint32_t nd_label(void)  { return nd_col(current_theme ? current_theme->label_color : 0x9ca3af); }
static uint32_t nd_accent(void) { return nd_col(current_theme ? current_theme->progress_color : 0x4FC3F7); }
static uint32_t nd_bright(void) { return nd_col(current_theme ? current_theme->header_text_color : 0xffffff); }
static uint32_t nd_warn(void)   { return nd_col(theme_is_red_night(current_theme) ? 0xff5555 : 0xf59e0b); }

/* Refresh pieces */

static void nd_refresh_header(void) {
    uint32_t tx, rx, drop, fc;
    net_trace_totals(&tx, &rx, &drop, &fc);
    net_trace_bins(nd_tx, nd_rx, NET_TRACE_BINS);
    /* Last second = last 1000/NET_TRACE_BIN_MS bins */
    int per_s = 1000 / NET_TRACE_BIN_MS;
    uint32_t txs = 0, rxs = 0;
    for (int i = NET_TRACE_BINS - per_s; i < NET_TRACE_BINS; i++) { txs += nd_tx[i]; rxs += nd_rx[i]; }
    char b[96];
    snprintf(b, sizeof(b), "TX %lu/s  RX %lu/s  drop %lu  fc %lu",
             (unsigned long)txs, (unsigned long)rxs, (unsigned long)drop, (unsigned long)fc);
    lv_label_set_text(nd_lbl_stats, b);
}

/* LIVE / FROZEN +Ns label: the only thing that keeps updating while frozen. */
static void nd_refresh_state(void) {
    char b[32];
    if (nd_frozen) {
        snprintf(b, sizeof(b), "FROZEN +%lus", (unsigned long)((now_ms() - nd_freeze_ms) / 1000));
        lv_label_set_text(nd_lbl_state, b);
        lv_obj_set_style_text_color(nd_lbl_state, lv_color_hex(nd_warn()), 0);
    } else {
        lv_label_set_text(nd_lbl_state, "LIVE");
        lv_obj_set_style_text_color(nd_lbl_state, lv_color_hex(nd_accent()), 0);
    }
}

static void nd_refresh_chart(void) {
    /* nd_tx already filled by nd_refresh_header() this tick */
    int32_t mx = 8;
    for (int i = 0; i < NET_TRACE_BINS; i++) {
        nd_vals[i] = nd_tx[i];
        if (nd_vals[i] > mx) mx = nd_vals[i];
    }
    lv_chart_set_axis_range(nd_chart, LV_CHART_AXIS_PRIMARY_Y, 0, mx);
    lv_chart_set_series_values(nd_chart, nd_ser, nd_vals, NET_TRACE_BINS);

    /* MARK cursors: newest ND_CURSORS marks inside the 30 s window, re-derived
     * every tick from the ring, so no recycle bookkeeping is needed. */
    uint32_t now = now_ms();
    uint32_t n = net_trace_count();
    if (n > NET_TRACE_RING) n = NET_TRACE_RING;
    int c = 0;
    for (uint32_t back = 0; back < n && c < ND_CURSORS; back++) {
        net_trace_rec_t r;
        if (!net_trace_read(back, &r)) break;
        uint32_t age = now - r.t_ms;
        if (age >= (uint32_t)NET_TRACE_BINS * NET_TRACE_BIN_MS) break;
        if (r.kind != NET_EV_MARK) continue;
        uint32_t bin = NET_TRACE_BINS - 1 - age / NET_TRACE_BIN_MS;
        lv_chart_set_cursor_point(nd_chart, nd_cur[c], nd_ser, bin);
        c++;
    }
    for (; c < ND_CURSORS; c++) lv_chart_set_cursor_point(nd_chart, nd_cur[c], nd_ser, LV_CHART_POINT_NONE);

    uint16_t pk = 0;
    char srcs[64] = "";
    net_trace_peak(&pk, srcs, sizeof(srcs));
    char b[112];
    snprintf(b, sizeof(b), "peak 30s: %u pkt @ %s", (unsigned)pk, srcs[0] ? srcs : "-");
    lv_label_set_text(nd_lbl_peak, b);
}

/* One countdown row. Rows [0,n) are live sched slots, then the fixed
 * ws ping / (mqtt keepalive) / sntp rows. */
static size_t nd_sched_row(size_t off, size_t k, size_t n, uint32_t now, const app_config_t *cfg) {
    if (k < n) {
        const net_sched_t *sc = &nd_sched[k];
        char due[16];
        if (sc->due_ms == 0) {
            snprintf(due, sizeof(due), "parked");
        } else {
            int32_t rem = (int32_t)(sc->due_ms - now);
            if (rem <= 0) {
                snprintf(due, sizeof(due), "now");
            } else {
                snprintf(due, sizeof(due), "T-%ld.%lds", (long)(rem / 1000), (long)((rem % 1000) / 100));
            }
        }
        return nd_app(off, "%-14.14s %-8s p=%lu.%lus n=%lu\n",
                      sc->name ? sc->name : "?", due,
                      (unsigned long)(sc->period_ms / 1000),
                      (unsigned long)((sc->period_ms % 1000) / 100),
                      (unsigned long)sc->count);
    }
    size_t j = k - n;
    if (j == 0) {
        int ws = 0;
        for (int i = 0; i < MAX_NINA_INSTANCES; i++) {
            if (cfg->instance_enabled[i]) ws++;
        }
        return nd_app(off, "%-14s %-8s p=10s x%d\n", "ws ping", "-", ws);
    }
    if (j == 1 && cfg->mqtt_enabled) {
        return nd_app(off, "%-14s %-8s p=120s\n", "mqtt keepalive", "-");
    }
    return nd_app(off, "%-14s %-8s p=3600s\n", "sntp", "-");
}

static void nd_refresh_sched(void) {
    size_t n = net_sched_list(nd_sched, NET_TRACE_MAX_SCHED);
    uint32_t now = now_ms();
    const app_config_t *cfg = app_config_get();
    size_t total = n + 2 + (cfg->mqtt_enabled ? 1 : 0);
    size_t half = (total + 1) / 2;   /* two columns so 16 + 3 rows fit ND_SCHED_H */
    size_t off = 0;
    nd_buf[0] = '\0';
    for (size_t k = 0; k < half; k++) off = nd_sched_row(off, k, n, now, cfg);
    lv_label_set_text(nd_lbl_sched, nd_buf);
    off = 0;
    nd_buf[0] = '\0';
    for (size_t k = half; k < total; k++) off = nd_sched_row(off, k, n, now, cfg);
    lv_label_set_text(nd_lbl_sched2, nd_buf);
}

static void nd_refresh_ticker(void) {
    uint32_t n = net_trace_count();
    if (n > NET_TRACE_RING) n = NET_TRACE_RING;
    size_t off = 0;
    nd_buf[0] = '\0';
    for (uint32_t back = 0; back < n && back < ND_TICKER_ROWS; back++) {
        net_trace_rec_t r, older;
        if (!net_trace_read(back, &r)) break;
        uint32_t gap = 0;
        bool have_older = net_trace_read(back + 1, &older);
        if (have_older) gap = r.t_ms - older.t_ms;

        char text[40];
        uint32_t col;
        if (r.kind == NET_EV_MARK) {
            snprintf(text, sizeof(text), "---- MARK ----");
            col = nd_bright();
        } else if (r.kind == NET_EV_UNATTR) {
            snprintf(text, sizeof(text), "?? %upkt", (unsigned)r.pkts);
            col = nd_warn();
        } else {
            const char *nm = (r.src < net_trace_src_n()) ? net_trace_src_name(r.src) : NULL;
            snprintf(text, sizeof(text), "%.32s", (nm && nm[0]) ? nm : "?");
            col = (have_older && gap < ND_BURST_MS) ? nd_accent() : nd_text();
        }
        off = nd_app(off, "#%06lx +%-6lu %s#\n",
                        (unsigned long)(col & 0xffffff), (unsigned long)gap, text);
    }
    if (off == 0) {
        snprintf(nd_buf, sizeof(nd_buf), "#%06lx (no events yet)#", (unsigned long)(nd_label() & 0xffffff));
    }
    lv_label_set_text(nd_lbl_tick, nd_buf);
}

static void nd_refresh_all(void) {
    nd_refresh_header();
    nd_refresh_chart();
    nd_refresh_sched();
    nd_refresh_ticker();
    nd_refresh_state();
}

static void nd_timer_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    if (!nd_root) return;
    if (nd_frozen) {
        nd_refresh_state();   /* whole picture held; only the +Ns counter moves */
        return;
    }
    nd_refresh_all();
}

/* Events */

static void nd_back_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_net_debug_hide();
}

static void nd_body_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == nd_btn_back) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) {
        return;   /* that was a swipe, not a tap */
    }
    net_trace_mark();
    /* Capture the moment of the tap (MARK row, last-tick TX) before honouring
     * the freeze, then hold it. */
    bool was_frozen = nd_frozen;
    nd_frozen = false;
    nd_refresh_all();
    nd_frozen = !was_frozen;
    if (nd_frozen) nd_freeze_ms = now_ms();
    nd_refresh_state();
}

/* Styling */

static void nd_restyle(void) {
    if (!nd_root) return;
    lv_obj_set_style_bg_color(nd_root, lv_color_hex(current_theme ? current_theme->bg_main : 0x000000), 0);
    lv_obj_set_style_text_color(nd_lbl_title, lv_color_hex(nd_bright()), 0);
    lv_obj_set_style_text_color(nd_lbl_stats, lv_color_hex(nd_text()), 0);
    lv_obj_set_style_text_color(nd_lbl_peak, lv_color_hex(nd_label()), 0);
    lv_obj_set_style_text_color(nd_lbl_sched, lv_color_hex(nd_text()), 0);
    lv_obj_set_style_text_color(nd_lbl_sched2, lv_color_hex(nd_text()), 0);
    lv_obj_set_style_bg_color(nd_chart, lv_color_hex(current_theme ? current_theme->bento_bg : 0x111111), 0);
    lv_obj_set_style_line_color(nd_chart, lv_color_hex(current_theme ? current_theme->bento_border : 0x333333), 0);
    lv_chart_set_series_color(nd_chart, nd_ser, lv_color_hex(nd_accent()));
    lv_obj_set_style_bg_color(nd_btn_back, lv_color_hex(current_theme ? current_theme->bento_border : 0x333333), 0);
    lv_obj_set_style_bg_color(nd_btn_back, lv_color_hex(nd_accent()), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(nd_btn_lbl, lv_color_hex(nd_bright()), 0);
    /* Cursor colour is fixed at create; ticker/state colours refresh every tick. */
}

static lv_obj_t *nd_make_label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, "");
    return l;
}

/* Public API */

void nina_net_debug_show(void) {
    if (nd_root) return;
    nav_arbiter_notify_modal_open();
    net_trace_set_verbose(true);
    nd_frozen = false;

    nd_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(nd_root);
    lv_obj_set_size(nd_root, screen_size(), screen_size());
    lv_obj_set_style_bg_opa(nd_root, LV_OPA_COVER, 0);
    /* INFO_OUTER_PAD (16) on square, the safe inset on round: the root is a
     * full-panel child of lv_layer_top(), so the pad is what puts its rows
     * inside the circle. Every row is LV_PCT sized (:325-376) and follows. */
    lv_obj_set_style_pad_all(nd_root, ui_page_inset(), 0);
    lv_obj_set_style_pad_row(nd_root, 8, 0);
    lv_obj_set_flex_flow(nd_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nd_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(nd_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(nd_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(nd_root, nd_body_cb, LV_EVENT_CLICKED, NULL);

    /* Header row */
    lv_obj_t *hdr = lv_obj_create(nd_root);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), 44);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_right(hdr, INFO_BACK_BTN_ZONE, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    nd_lbl_title = nd_make_label(hdr, &lv_font_montserrat_28);
    lv_label_set_text(nd_lbl_title, "NET TRACE");
    nd_lbl_stats = nd_make_label(hdr, &lv_font_montserrat_18);
    nd_lbl_state = nd_make_label(hdr, &lv_font_montserrat_18);

    /* Sparkline */
    nd_chart = lv_chart_create(nd_root);
    lv_obj_set_size(nd_chart, LV_PCT(100), ND_CHART_H);
    lv_chart_set_type(nd_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(nd_chart, NET_TRACE_BINS);
    lv_chart_set_div_line_count(nd_chart, 3, 0);
    lv_obj_set_style_pad_all(nd_chart, 4, 0);
    lv_obj_set_style_pad_column(nd_chart, 1, 0);
    lv_obj_set_style_border_width(nd_chart, 0, 0);
    lv_obj_set_style_radius(nd_chart, 8, 0);
    lv_obj_remove_flag(nd_chart, LV_OBJ_FLAG_CLICKABLE);
    nd_ser = lv_chart_add_series(nd_chart, lv_color_hex(nd_accent()), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < ND_CURSORS; i++) {
        nd_cur[i] = lv_chart_add_cursor(nd_chart, lv_color_hex(nd_bright()), LV_DIR_VER);
        lv_chart_set_cursor_point(nd_chart, nd_cur[i], nd_ser, LV_CHART_POINT_NONE);
    }
    lv_obj_set_style_line_width(nd_chart, 2, LV_PART_CURSOR);

    nd_lbl_peak = nd_make_label(nd_root, &lv_font_montserrat_16);

    /* Countdown table: one multi-line label in a fixed, clipped box */
    lv_obj_t *sbox = lv_obj_create(nd_root);
    lv_obj_remove_style_all(sbox);
    lv_obj_set_size(sbox, LV_PCT(100), ND_SCHED_H);
    lv_obj_remove_flag(sbox, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    nd_lbl_sched = nd_make_label(sbox, &lv_font_montserrat_14);
    lv_obj_set_size(nd_lbl_sched, LV_PCT(50), LV_PCT(100));
    lv_label_set_long_mode(nd_lbl_sched, LV_LABEL_LONG_MODE_CLIP);
    nd_lbl_sched2 = nd_make_label(sbox, &lv_font_montserrat_14);
    lv_obj_set_size(nd_lbl_sched2, LV_PCT(50), LV_PCT(100));
    lv_obj_align(nd_lbl_sched2, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_label_set_long_mode(nd_lbl_sched2, LV_LABEL_LONG_MODE_CLIP);

    /* Ticker: remaining height, recoloured per row */
    lv_obj_t *tbox = lv_obj_create(nd_root);
    lv_obj_remove_style_all(tbox);
    lv_obj_set_width(tbox, LV_PCT(100));
    lv_obj_set_flex_grow(tbox, 1);
    lv_obj_set_style_pad_right(tbox, INFO_BACK_BTN_ZONE, 0);
    lv_obj_remove_flag(tbox, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    nd_lbl_tick = nd_make_label(tbox, &lv_font_montserrat_16);
    lv_obj_set_size(nd_lbl_tick, LV_PCT(100), LV_PCT(100));
    lv_label_set_long_mode(nd_lbl_tick, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_recolor(nd_lbl_tick, true);

    /* Back button: same geometry as nina_info_overlay */
    nd_btn_back = lv_button_create(nd_root);
    lv_obj_set_size(nd_btn_back, INFO_BACK_BTN_W, INFO_BACK_BTN_H);
    lv_obj_set_style_radius(nd_btn_back, 14, 0);
    lv_obj_set_style_bg_opa(nd_btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nd_btn_back, 0, 0);
    lv_obj_set_style_shadow_width(nd_btn_back, 0, 0);
    lv_obj_add_flag(nd_btn_back, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(nd_btn_back, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(nd_btn_back, LV_ALIGN_BOTTOM_RIGHT, -INFO_OUTER_PAD, -INFO_OUTER_PAD);
    nd_btn_lbl = nd_make_label(nd_btn_back, &lv_font_montserrat_24);
    lv_label_set_text(nd_btn_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(nd_btn_lbl);
    lv_obj_add_event_cb(nd_btn_back, nd_back_cb, LV_EVENT_CLICKED, NULL);

    nd_restyle();
    nd_timer_cb(NULL);
    nd_timer = lv_timer_create(nd_timer_cb, ND_TICK_MS, NULL);
}

void nina_net_debug_hide(void) {
    if (!nd_root) return;
    if (nd_timer) {
        lv_timer_delete(nd_timer);
        nd_timer = NULL;
    }
    lv_obj_delete(nd_root);
    nd_root = NULL;
    nd_ser = NULL;
    net_trace_set_verbose(app_config_get()->debug_mode);  /* stay on while debug is on */
    nav_arbiter_notify_modal_close(esp_timer_get_time() / 1000);
}

bool nina_net_debug_visible(void) { return nd_root != NULL; }

void nina_net_debug_apply_theme(void) { nd_restyle(); }
