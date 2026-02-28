/**
 * @file ui_styles.c
 * @brief Shared LVGL style definitions and initialization.
 *
 * Supports multiple widget panel styles (0-6) selected via app_config.
 * Styles 2 (Wireframe) and 6 (Chamfered) use custom draw callbacks for
 * corner accents and chamfered corners respectively.
 */

#include "ui_styles.h"
#include "themes.h"
#include "app_config.h"
#include "display_defs.h"
#include "nina_dashboard_internal.h"

#include <string.h>

/* Layout constants used by styles */
#define BENTO_RADIUS 24

/* Sentinel value stored in user_data to prevent duplicate callback attachment */
#define WIDGET_DRAW_CB_MAGIC ((void *)(uintptr_t)0xBE470001)

/* Style variable definitions */
lv_style_t style_bento_box;
lv_style_t style_label_small;
lv_style_t style_value_large;
lv_style_t style_header_gradient;

/* ---------- helpers ---------- */

static uint32_t darken_color(uint32_t color, int pct)
{
    int r = (color >> 16) & 0xFF;
    int g = (color >>  8) & 0xFF;
    int b =  color        & 0xFF;
    r = r * (100 - pct) / 100;
    g = g * (100 - pct) / 100;
    b = b * (100 - pct) / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ---------- custom draw callback ---------- */

static void widget_draw_cb(lv_event_t *e)
{
    const app_config_t *cfg = app_config_get();
    uint8_t ws = cfg->widget_style;

    /* Only styles 2 and 6 need custom drawing */
    if (ws != 2 && ws != 6) return;

    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    int32_t x1 = coords.x1;
    int32_t y1 = coords.y1;
    int32_t x2 = coords.x2;
    int32_t y2 = coords.y2;

    if (ws == 2) {
        /* Wireframe: draw 2px thick, 8px long corner accent lines */
        uint32_t accent_color = current_theme ? current_theme->progress_color : 0x00AAFF;

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_hex(accent_color);
        line_dsc.width = 2;
        line_dsc.opa   = LV_OPA_COVER;

        /* Top-left corner */
        line_dsc.p1 = (lv_point_precise_t){x1, y1};
        line_dsc.p2 = (lv_point_precise_t){x1 + 8, y1};
        lv_draw_line(layer, &line_dsc);
        line_dsc.p1 = (lv_point_precise_t){x1, y1};
        line_dsc.p2 = (lv_point_precise_t){x1, y1 + 8};
        lv_draw_line(layer, &line_dsc);

        /* Top-right corner */
        line_dsc.p1 = (lv_point_precise_t){x2, y1};
        line_dsc.p2 = (lv_point_precise_t){x2 - 8, y1};
        lv_draw_line(layer, &line_dsc);
        line_dsc.p1 = (lv_point_precise_t){x2, y1};
        line_dsc.p2 = (lv_point_precise_t){x2, y1 + 8};
        lv_draw_line(layer, &line_dsc);

        /* Bottom-left corner */
        line_dsc.p1 = (lv_point_precise_t){x1, y2};
        line_dsc.p2 = (lv_point_precise_t){x1 + 8, y2};
        lv_draw_line(layer, &line_dsc);
        line_dsc.p1 = (lv_point_precise_t){x1, y2};
        line_dsc.p2 = (lv_point_precise_t){x1, y2 - 8};
        lv_draw_line(layer, &line_dsc);

        /* Bottom-right corner */
        line_dsc.p1 = (lv_point_precise_t){x2, y2};
        line_dsc.p2 = (lv_point_precise_t){x2 - 8, y2};
        lv_draw_line(layer, &line_dsc);
        line_dsc.p1 = (lv_point_precise_t){x2, y2};
        line_dsc.p2 = (lv_point_precise_t){x2, y2 - 8};
        lv_draw_line(layer, &line_dsc);

    } else if (ws == 6) {
        /* Chamfered: draw filled triangles in bg_main at each corner (~26px) */
        uint32_t bg_color = current_theme ? current_theme->bg_main : 0x000000;
        int32_t sz = 26;

        lv_draw_triangle_dsc_t tri_dsc;
        lv_draw_triangle_dsc_init(&tri_dsc);
        tri_dsc.bg_color = lv_color_hex(bg_color);
        tri_dsc.bg_opa   = LV_OPA_COVER;

        /* Top-left */
        tri_dsc.p[0] = (lv_point_precise_t){x1, y1};
        tri_dsc.p[1] = (lv_point_precise_t){x1 + sz, y1};
        tri_dsc.p[2] = (lv_point_precise_t){x1, y1 + sz};
        lv_draw_triangle(layer, &tri_dsc);

        /* Top-right */
        tri_dsc.p[0] = (lv_point_precise_t){x2, y1};
        tri_dsc.p[1] = (lv_point_precise_t){x2 - sz, y1};
        tri_dsc.p[2] = (lv_point_precise_t){x2, y1 + sz};
        lv_draw_triangle(layer, &tri_dsc);

        /* Bottom-left */
        tri_dsc.p[0] = (lv_point_precise_t){x1, y2};
        tri_dsc.p[1] = (lv_point_precise_t){x1 + sz, y2};
        tri_dsc.p[2] = (lv_point_precise_t){x1, y2 - sz};
        lv_draw_triangle(layer, &tri_dsc);

        /* Bottom-right */
        tri_dsc.p[0] = (lv_point_precise_t){x2, y2};
        tri_dsc.p[1] = (lv_point_precise_t){x2 - sz, y2};
        tri_dsc.p[2] = (lv_point_precise_t){x2, y2 - sz};
        lv_draw_triangle(layer, &tri_dsc);
    }
}

/* ---------- public API ---------- */

void ui_styles_update(const void *theme_ptr)
{
    const theme_t *theme = (const theme_t *)theme_ptr;
    if (!theme) return;

    int gb = app_config_get()->color_brightness;
    uint8_t ws = app_config_get()->widget_style;

    /* --- style_bento_box: varies by widget_style --- */
    lv_style_reset(&style_bento_box);
    lv_style_init(&style_bento_box);
    lv_style_set_pad_all(&style_bento_box, 20);

    switch (ws) {
    default:
    case 0: /* Default — solid opaque, radius 24, border matches bg */
        lv_style_set_bg_color(&style_bento_box, lv_color_hex(theme->bento_bg));
        lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
        lv_style_set_radius(&style_bento_box, BENTO_RADIUS);
        lv_style_set_border_width(&style_bento_box, 1);
        lv_style_set_border_color(&style_bento_box, lv_color_hex(theme->bento_bg));
        lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
        break;

    case 1: /* Subtle Border — solid panel, 1px bento_border, radius 12 */
        lv_style_set_bg_color(&style_bento_box, lv_color_hex(theme->bento_bg));
        lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
        lv_style_set_radius(&style_bento_box, 12);
        lv_style_set_border_width(&style_bento_box, 1);
        lv_style_set_border_color(&style_bento_box, lv_color_hex(theme->bento_border));
        lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
        break;

    case 2: /* Tech Wireframe — transparent bg, 1px progress_color border, radius 0 */
        lv_style_set_bg_opa(&style_bento_box, LV_OPA_TRANSP);
        lv_style_set_radius(&style_bento_box, 0);
        lv_style_set_border_width(&style_bento_box, 1);
        lv_style_set_border_color(&style_bento_box, lv_color_hex(theme->progress_color));
        lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
        break;

    case 3: /* Soft Inset — darkened bg, 1px top-only black border, radius 12 */
        lv_style_set_bg_color(&style_bento_box, lv_color_hex(darken_color(theme->bento_bg, 30)));
        lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
        lv_style_set_radius(&style_bento_box, 12);
        lv_style_set_border_width(&style_bento_box, 1);
        lv_style_set_border_color(&style_bento_box, lv_color_hex(0x000000));
        lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
        lv_style_set_border_side(&style_bento_box, LV_BORDER_SIDE_TOP);
        break;

    case 4: /* Frosted Glass — semi-transparent bg, subtle border, radius 24 */
        lv_style_set_bg_color(&style_bento_box, lv_color_hex(theme->bento_bg));
        lv_style_set_bg_opa(&style_bento_box, LV_OPA_40);
        lv_style_set_radius(&style_bento_box, BENTO_RADIUS);
        lv_style_set_border_width(&style_bento_box, 1);
        lv_style_set_border_color(&style_bento_box, lv_color_hex(theme->bento_border));
        lv_style_set_border_opa(&style_bento_box, LV_OPA_20);
        break;

    case 5: /* Accent Bar — solid panel, 4px left-only border in progress_color, radius 12 */
        lv_style_set_bg_color(&style_bento_box, lv_color_hex(theme->bento_bg));
        lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
        lv_style_set_radius(&style_bento_box, 12);
        lv_style_set_border_width(&style_bento_box, 4);
        lv_style_set_border_color(&style_bento_box, lv_color_hex(theme->progress_color));
        lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
        lv_style_set_border_side(&style_bento_box, LV_BORDER_SIDE_LEFT);
        break;

    case 6: /* Chamfered — solid panel, radius 0, no border */
        lv_style_set_bg_color(&style_bento_box, lv_color_hex(theme->bento_bg));
        lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
        lv_style_set_radius(&style_bento_box, 0);
        lv_style_set_border_width(&style_bento_box, 0);
        break;
    }

    /* --- style_label_small: unchanged --- */
    lv_style_reset(&style_label_small);
    lv_style_init(&style_label_small);
    lv_style_set_text_color(&style_label_small, lv_color_hex(app_config_apply_brightness(theme->label_color, gb)));
    lv_style_set_text_font(&style_label_small, &lv_font_montserrat_16);
    lv_style_set_text_letter_space(&style_label_small, 1);

    /* --- style_value_large: unchanged --- */
    lv_style_reset(&style_value_large);
    lv_style_init(&style_value_large);
    lv_style_set_text_color(&style_value_large, lv_color_hex(app_config_apply_brightness(theme->text_color, gb)));
#ifdef LV_FONT_MONTSERRAT_48
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_48);
#elif defined(LV_FONT_MONTSERRAT_32)
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_32);
#elif defined(LV_FONT_MONTSERRAT_28)
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_28);
#else
    lv_style_set_text_font(&style_value_large, &lv_font_montserrat_20);
#endif

    /* --- style_header_gradient: unchanged --- */
    lv_style_reset(&style_header_gradient);
    lv_style_init(&style_header_gradient);
    lv_style_set_bg_color(&style_header_gradient, lv_color_hex(theme->header_grad_color));
    lv_style_set_bg_grad_color(&style_header_gradient, lv_color_hex(0x000000));
    lv_style_set_bg_grad_dir(&style_header_gradient, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&style_header_gradient, LV_OPA_30);
    lv_style_set_radius(&style_header_gradient, BENTO_RADIUS);
    lv_style_set_border_width(&style_header_gradient, 1);
    lv_style_set_border_color(&style_header_gradient, lv_color_hex(theme->header_grad_color));
    lv_style_set_border_opa(&style_header_gradient, LV_OPA_30);
    lv_style_set_pad_all(&style_header_gradient, 20);
}

void ui_styles_set_widget_draw_cbs(lv_obj_t *obj)
{
    if (!obj) return;

    /* Only attach the callback once per object */
    if (lv_obj_get_user_data(obj) == WIDGET_DRAW_CB_MAGIC) return;

    lv_obj_add_event_cb(obj, widget_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_set_user_data(obj, WIDGET_DRAW_CB_MAGIC);
}

const char *ui_styles_get_widget_style_name(int index)
{
    static const char *names[] = {
        "Default",
        "Subtle Border",
        "Wireframe",
        "Soft Inset",
        "Frosted Glass",
        "Accent Bar",
        "Chamfered",
    };
    if (index >= 0 && index < WIDGET_STYLE_COUNT) return names[index];
    return "Default";
}
