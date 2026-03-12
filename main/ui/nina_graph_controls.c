/**
 * @file nina_graph_controls.c
 * @brief Interactive controls for the graph overlay (point/scale selectors, legend).
 */

#include "nina_graph_internal.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include <stdio.h>
#include <string.h>

/* -- Callbacks ----------------------------------------------------------- */

void back_btn_cb(lv_event_t *e) {
    LV_UNUSED(e);
    nina_graph_hide();
    /* Return to the NINA instance page that opened us */
    nina_dashboard_show_page(return_page_index, 0);
}

void point_btn_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= POINT_OPT_COUNT) return;

    int gb = app_config_get()->color_brightness;

    /* Update button highlight and text color */
    for (int i = 0; i < POINT_OPT_COUNT; i++) {
        lv_obj_t *lbl = lv_obj_get_child(btn_points[i], 0);
        if (i == idx) {
            lv_obj_set_style_bg_color(btn_points[i], lv_color_hex(current_theme->progress_color), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(btn_points[i], lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(get_control_text_color(gb)), 0);
        }
    }

    selected_points_idx = idx;

    /* Show empty chart with loading text and request new data */
    show_loading_state();
    graph_requested = true;
}

void scale_btn_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= scale_btn_count) return;

    int gb = app_config_get()->color_brightness;

    for (int i = 0; i < scale_btn_count; i++) {
        lv_obj_t *lbl = lv_obj_get_child(btn_scale[i], 0);
        if (i == idx) {
            lv_obj_set_style_bg_color(btn_scale[i], lv_color_hex(current_theme->progress_color), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(btn_scale[i], lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(get_control_text_color(gb)), 0);
        }
    }

    selected_scale_idx = idx;

    /* Apply new Y range to the chart */
    if (chart) {
        int scale_val = 0;
        if (current_type == GRAPH_TYPE_RMS && idx < RMS_SCALE_COUNT) {
            scale_val = rms_scale_values[idx];
        } else if (current_type == GRAPH_TYPE_HFR && idx < HFR_SCALE_COUNT) {
            scale_val = hfr_scale_values[idx];
        }

        if (scale_val > 0) {
            /* Fixed scale: Y range is -scale_val to +scale_val for RMS, 0 to scale_val for HFR */
            if (current_type == GRAPH_TYPE_RMS) {
                lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -scale_val, scale_val);
                update_y_labels(-scale_val, scale_val);
                update_threshold_lines(-scale_val, scale_val);
            } else {
                lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, scale_val);
                update_y_labels(0, scale_val);
                update_threshold_lines(0, scale_val);
            }
        } else {
            /* Auto: re-request data to recalculate range */
            show_loading_state();
            graph_requested = true;
        }
        lv_chart_refresh(chart);
    }
}

/* -- Helper: create a pill button ---------------------------------------- */
lv_obj_t *make_pill_btn(lv_obj_t *parent, const char *text, bool selected,
                                lv_event_cb_t cb, int user_data) {
    int gb = app_config_get()->color_brightness;

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 48);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 4, 0);
    lv_obj_set_style_pad_ver(btn, 6, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

    if (selected) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->progress_color), 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(app_config_apply_brightness(current_theme->bento_border, gb)), 0);
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(current_theme->progress_color), LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    if (selected) {
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    } else {
        lv_obj_set_style_text_color(lbl, lv_color_hex(get_control_text_color(gb)), 0);
    }
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)user_data);

    return btn;
}

/* -- Controls row (rebuilt when switching graph type) --------------------- */

void rebuild_controls(void) {
    int gb = app_config_get()->color_brightness;

    /* Delete old controls if any */
    if (controls_cont) {
        lv_obj_delete(controls_cont);
        controls_cont = NULL;
    }

    /* Create new controls container -- column with two rows */
    controls_cont = lv_obj_create(overlay);
    lv_obj_remove_style_all(controls_cont);
    lv_obj_set_width(controls_cont, LV_PCT(100));
    lv_obj_set_height(controls_cont, GR_CONTROLS_H);
    lv_obj_set_flex_flow(controls_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(controls_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(controls_cont, 4, 0);
    lv_obj_set_style_pad_right(controls_cont, GR_BACK_BTN_W + 20, 0);

    /* -- Row 1: Point count -- */
    lv_obj_t *row_pts = lv_obj_create(controls_cont);
    lv_obj_remove_style_all(row_pts);
    lv_obj_set_width(row_pts, LV_PCT(100));
    lv_obj_set_height(row_pts, 48);
    lv_obj_set_flex_flow(row_pts, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_pts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_pts, 6, 0);

    lv_obj_t *lbl_pts = lv_label_create(row_pts);
    lv_label_set_text(lbl_pts, "Pts:");
    lv_obj_set_style_text_font(lbl_pts, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_pts, lv_color_hex(app_config_apply_brightness(
        current_theme ? current_theme->text_color : 0xaaaaaa, gb)), 0);
    lv_obj_set_width(lbl_pts, LV_SIZE_CONTENT);

    for (int i = 0; i < POINT_OPT_COUNT; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", point_options[i]);
        btn_points[i] = make_pill_btn(row_pts, buf, (i == selected_points_idx),
                                       point_btn_cb, i);
    }

    /* -- Row 2: Y scale -- */
    lv_obj_t *row_scale = lv_obj_create(controls_cont);
    lv_obj_remove_style_all(row_scale);
    lv_obj_set_width(row_scale, LV_PCT(100));
    lv_obj_set_height(row_scale, 48);
    lv_obj_set_flex_flow(row_scale, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_scale, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_scale, 6, 0);

    lv_obj_t *lbl_sc = lv_label_create(row_scale);
    lv_label_set_text(lbl_sc, "Y:");
    lv_obj_set_style_text_font(lbl_sc, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_sc, lv_color_hex(app_config_apply_brightness(
        current_theme ? current_theme->text_color : 0xaaaaaa, gb)), 0);
    lv_obj_set_width(lbl_sc, LV_SIZE_CONTENT);

    int sc_count = (current_type == GRAPH_TYPE_RMS) ? RMS_SCALE_COUNT : HFR_SCALE_COUNT;
    const char **sc_labels = (current_type == GRAPH_TYPE_RMS) ? rms_scale_labels : hfr_scale_labels;
    scale_btn_count = sc_count;

    if (selected_scale_idx >= sc_count) selected_scale_idx = 0;

    for (int i = 0; i < sc_count; i++) {
        btn_scale[i] = make_pill_btn(row_scale, sc_labels[i], (i == selected_scale_idx),
                                      scale_btn_cb, i);
    }

    /* Keep back button on top of newly created controls */
    if (btn_back) lv_obj_move_foreground(btn_back);
}

/* -- Legend item click callback: toggle series visibility ---------------- */

void legend_item_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *item = lv_event_get_current_target(e);

    bool *hidden_flag = NULL;
    lv_chart_series_t *series = NULL;

    switch (idx) {
    case 0: hidden_flag = &legend_ra_hidden;    series = ser_ra;    break;
    case 1: hidden_flag = &legend_dec_hidden;   series = ser_dec;   break;
    case 2: hidden_flag = &legend_hfr_hidden;   series = ser_hfr;   break;
    case 3: hidden_flag = &legend_total_hidden; series = ser_total; break;
    default: return;
    }

    *hidden_flag = !*hidden_flag;
    if (chart) {
        lv_chart_hide_series(chart, series, *hidden_flag);
        lv_chart_refresh(chart);
    }
    lv_obj_set_style_opa(item, *hidden_flag ? LV_OPA_30 : LV_OPA_COVER, 0);
}

/* -- Rebuild the legend -------------------------------------------------- */

void rebuild_legend(void) {
    if (!legend_cont) return;

    /* Clear existing children */
    lv_obj_clean(legend_cont);

    /* Helper struct: { color, label text, hidden flag ptr, callback index } */
    struct legend_item {
        uint32_t color;
        const char *text;
        bool hidden;
        int cb_idx;
    };

    struct legend_item items[3];
    int item_count = 0;

    if (current_type == GRAPH_TYPE_RMS) {
        items[0] = (struct legend_item){get_ra_color(),    "RA",    legend_ra_hidden,    0};
        items[1] = (struct legend_item){get_dec_color(),   "DEC",   legend_dec_hidden,   1};
        items[2] = (struct legend_item){get_total_color(), "Tot",   legend_total_hidden,  3};
        item_count = 3;
    } else {
        items[0] = (struct legend_item){get_hfr_color(), "HFR", legend_hfr_hidden, 2};
        item_count = 1;
    }

    for (int i = 0; i < item_count; i++) {
        /* Pill button with series color background */
        lv_obj_t *btn = lv_button_create(legend_cont);
        lv_obj_set_height(btn, 36);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(btn, 48, 0);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, 14, 0);
        lv_obj_set_style_pad_ver(btn, 4, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(items[i].color), 0);
        lv_obj_set_style_opa(btn, items[i].hidden ? LV_OPA_30 : LV_OPA_COVER, 0);
        lv_obj_add_event_cb(btn, legend_item_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)items[i].cb_idx);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, items[i].text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_center(lbl);
    }

    /* Re-align after content change */
    lv_obj_align(legend_cont, LV_ALIGN_TOP_RIGHT, -4, 4);
}
