/**
 * @file settings_wifi_round.c
 * @brief Round composition of the Panel Mode WiFi screens, inscribed board 9
 *        plus the addendum's hidden-network ruling.
 *
 * A fit pass over what settings_wifi.c already built. The PASSWORD screen goes
 * absolute (every child there is a captured handle): the full-width keyboard is
 * raised off the bottom edge to the equator and narrowed to 540 px so all four
 * rows keep ten full keys, and the CONNECT bar becomes a 280 px pill on the
 * bottom cap. The home, scan and result screens KEEP their column flex and are
 * fitted through it: cross-axis centring, vertical pads that seat the header on
 * the top cap and end the content above the bottom cap, per-row chord widths,
 * and RESCAN moved from the header to the bottom cap. Switching those screens
 * to LAYOUT_NONE would strand every label, the connected row, ADD NETWORK and
 * the scanning and failure wraps on the content origin, and would collapse the
 * scan list from flex_grow to the 130 px lv_obj default height.
 *
 * No radio call, no gate change: wifi_connect_gate_update(), wifi_kb_ready_cb
 * and the LV_EVENT_FOCUSED docking are the shipped ones and are not touched.
 * hub_back_cb stays the only back path.
 *
 * Display lock held by the caller.
 */

#include "settings_wifi_internal.h"
#include "settings_hub_internal.h"   /* hub_header_obj, header font, 27 px sweep */
#include "ui_round.h"
#include "screen_geom.h"
#include "lvgl.h"

#define WR_HEADER_W     420
#define WR_HEADER_H      72
#define WR_HEADER_DY   (-228)
#define WR_BACK_W        96     /* HUB_BACK_W: the shared header's BACK button */
#define WR_ROW_W        530
#define WR_ROW_H         72
#define WR_SROW_DY     (-142)
#define WR_PROW_DY     (-144)   /* normal variant */
#define WR_PROW_DY_HID  (-58)   /* hidden variant: password sits under the SSID row */
#define WR_KB_W         540
#define WR_KB_H         300
#define WR_KB_H_HID     260     /* 720 hidden variant: keep the dock, shrink the rows */
#define WR_KB_DOCK      152     /* pixels from the panel bottom edge */
#define WR_KB_DOCK_HID  112     /* addendum ruling: 40 px lower, 800 hidden variant only */
#define WR_CONNECT_W    280
#define WR_CONNECT_H     60
#define WR_CONNECT_DY   268
#define WR_LIST_BOT_DY   238    /* content ends above the bottom cap */
#define WR_ROW_W_MIN    240     /* a row past the bottom cap still needs a target */

/* Header seating, shared by all four screens. Sizes the row, raises its BACK
 * label to the 27 px floor and bounds the title so it cannot run over BACK.
 * Does NOT touch the screen's layout: only the password screen may lose its
 * flex (see fit_password). */
static void fit_header(void)
{
    if (!hub_header_obj) return;
    lv_obj_set_size(hub_header_obj, WR_HEADER_W, WR_HEADER_H);
    settings_hub_round_header_font(hub_header_obj);

    /* The centred title is unbounded (or sized 420 on the password screen) for
     * the 688 px square header. Give it the span to the RIGHT of BACK and
     * centre it there: the box then starts exactly at BACK's trailing edge, so
     * even a dotted 32-character SSID cannot overlap the button. */
    lv_obj_t *title = lv_obj_get_child(hub_header_obj, 1);
    if (title) {
        lv_obj_set_width(title, WR_HEADER_W - WR_BACK_W);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, WR_BACK_W / 2, 0);
    }
}

/* The one screen that may go absolute: every child is a captured handle. */
static void fit_password(lv_obj_t *screen)
{
    lv_obj_set_layout(screen, LV_LAYOUT_NONE);
    fit_header();
    if (hub_header_obj) {
        lv_obj_align(hub_header_obj, LV_ALIGN_CENTER, 0, WR_HEADER_DY);
    }

    if (wifi_srow_obj) {
        lv_obj_set_size(wifi_srow_obj, WR_ROW_W, WR_ROW_H);
        lv_obj_align(wifi_srow_obj, LV_ALIGN_CENTER, 0, WR_SROW_DY);
        /* The textarea is a fixed-height flex child: a shorter row would leave
         * it standing proud of the row it lives in. */
        lv_obj_t *ta = lv_obj_get_child(wifi_srow_obj, 0);
        if (ta) lv_obj_set_height(ta, WR_ROW_H);
    }
    if (wifi_prow_obj) {
        lv_obj_set_size(wifi_prow_obj, WR_ROW_W, WR_ROW_H);
        lv_obj_align(wifi_prow_obj, LV_ALIGN_CENTER, 0,
                     wifi_hidden_variant ? WR_PROW_DY_HID : WR_PROW_DY);
        /* Normal variant ships an 88 px field beside the 72 px eye button. */
        lv_obj_t *ta = lv_obj_get_child(wifi_prow_obj, 0);
        if (ta) lv_obj_set_height(ta, WR_ROW_H);
    }
    if (wifi_connect_obj) {
        lv_obj_set_size(wifi_connect_obj, WR_CONNECT_W, WR_CONNECT_H);
        lv_obj_set_style_radius(wifi_connect_obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(wifi_connect_obj, LV_ALIGN_CENTER, 0, WR_CONNECT_DY);
    }
    if (wifi_kb_obj) {
        /* Controller ruling on the addendum's hidden-network line: at 720 the
         * hidden variant keeps the 152 px dock and shrinks to 260 px rows to
         * make room for the SSID row; at 800 the 40 px lower dock stands with
         * the full 300 px keyboard. Panel width, not shape, decides. */
        int kb_h = (screen_size() >= 800 || !wifi_hidden_variant) ? WR_KB_H : WR_KB_H_HID;
        int dock = (screen_size() >= 800 && wifi_hidden_variant) ? WR_KB_DOCK_HID : WR_KB_DOCK;
        lv_obj_set_size(wifi_kb_obj, WR_KB_W, kb_h);
        /* Centre relative, so the dock means the same thing at both widths. */
        lv_obj_align(wifi_kb_obj, LV_ALIGN_CENTER, 0, screen_center() - dock - kb_h / 2);
    }
}

/* Raise the labels of any button nested one level inside a row. The shared
 * sweep only walks direct children, and the home rows carry a FORGET button
 * whose label ships at lv_font_montserrat_24, under the round floor. */
static void fit_row_nested_labels(lv_obj_t *row)
{
    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(row, i);
        if (lv_obj_check_type(c, &lv_button_class)) {
            settings_hub_round_raise_small_labels(c);
        }
    }
}

/* Set each row's width from the NARROWER of the two chords at its own resting
 * top and bottom edges: above the equator the top edge binds, below it the
 * bottom edge does, and taking the lower edge everywhere would push the topmost
 * rows back out of the circle. The list scrolls, so a row keeps the width it
 * was measured for; the taper is the picked composition (inscribed board 9). */
static void fit_rows(lv_obj_t *cont)
{
    if (!cont) return;
    uint32_t n = lv_obj_get_child_count(cont);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(cont, i);
        if (!row || !lv_obj_check_type(row, &lv_button_class)) continue;

        lv_area_t a;
        lv_obj_get_coords(row, &a);
        int c_top = ui_chord_at_y(a.y1);
        int c_bot = ui_chord_at_y(a.y2);
        int w = ((c_top < c_bot) ? c_top : c_bot) - 2 * UI_SQUARE_INSET;
        if (w < WR_ROW_W_MIN) w = WR_ROW_W_MIN;
        lv_obj_set_width(row, w);
        fit_row_nested_labels(row);
    }
}

/* Home and scan: the flex STAYS. Only the cross-axis alignment, the vertical
 * pads and the per-row widths change, so every label, the connected row, ADD
 * NETWORK and the "Scanning..." / "Scan failed" wraps keep their flow position
 * and the scan list keeps its flex_grow height. */
static void fit_list_screen(lv_obj_t *screen, bool has_scroller)
{
    fit_header();

    /* A row narrower than its parent must be centred, not left flush: a
     * left-flush 420 px row in a 688 px screen leaves the circle on the left. */
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(screen,
        screen_center() + WR_HEADER_DY - WR_HEADER_H / 2 - UI_SQUARE_INSET, 0);
    lv_obj_set_style_pad_bottom(screen,
        screen_center() - WR_LIST_BOT_DY - UI_SQUARE_INSET, 0);
    settings_hub_round_raise_small_labels(screen);

    /* The pads and the header resize above change every row's resting y, and
     * LVGL 9.5 only writes obj->coords during a layout pass, so fit_rows would
     * otherwise measure the pre-fit tree. */
    lv_obj_update_layout(screen);

    if (has_scroller && wifi_list_obj) {
        lv_obj_set_flex_align(wifi_list_obj, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_update_layout(screen);
        fit_rows(wifi_list_obj);
    } else {
        /* Home screen: the rows are direct children of the screen, and the
         * header is not a button, so the walk skips it. Widths only; the
         * screen itself is not resized. The three saved slots plus the
         * connected row, ADD NETWORK and the slots-full note do not fit
         * between the two caps, so this screen scrolls, exactly as the
         * Brightness and More screens do. */
        fit_rows(screen);
        lv_obj_add_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(screen, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_AUTO);
    }

    /* RESCAN does not fit beside BACK and a 290 px title in a 420 px header,
     * so it takes the bottom-cap seat CONNECT uses on the password screen. */
    if (wifi_rescan_obj) {
        lv_obj_set_parent(wifi_rescan_obj, screen);
        lv_obj_add_flag(wifi_rescan_obj, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(wifi_rescan_obj, WR_CONNECT_W, WR_CONNECT_H);
        lv_obj_set_style_radius(wifi_rescan_obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(wifi_rescan_obj, LV_ALIGN_CENTER, 0, WR_CONNECT_DY);
        lv_obj_t *lbl = lv_obj_get_child(wifi_rescan_obj, 0);
        if (lbl) lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    }
}

void settings_wifi_round_fit(lv_obj_t *screen, hub_screen_t which)
{
    if (!screen) return;
    switch (which) {
    case HUB_SCREEN_WIFI_PASSWORD:
        fit_password(screen);
        break;
    case HUB_SCREEN_WIFI_SCAN:
        fit_list_screen(screen, true);
        break;
    case HUB_SCREEN_WIFI_HOME:
        fit_list_screen(screen, false);
        break;
    case HUB_SCREEN_WIFI_CONNECT:
    default:
        /* The result screen is a centred spinner plus two buttons on a flex
         * column: it already sits on the vertical centreline, so only the
         * header row and the 27 px floor need attention. It has no header of
         * its own, so fit_header() is a no-op there. */
        fit_header();
        lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        settings_hub_round_raise_small_labels(screen);
        break;
    }
}
