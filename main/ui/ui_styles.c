/**
 * @file ui_styles.c
 * @brief Shared LVGL style definitions and initialization.
 */

#include "ui_styles.h"
#include "themes.h"
#include "app_config.h"
#include "display_defs.h"

/* Layout constants used by styles */
#define BENTO_RADIUS 24

/* Style variable definitions */
lv_style_t style_bento_box;
lv_style_t style_label_small;
lv_style_t style_value_large;
lv_style_t style_header_gradient;

void ui_styles_update(const void *theme_ptr) {
    const theme_t *theme = (const theme_t *)theme_ptr;
    if (!theme) return;

    int gb = app_config_get()->color_brightness;

    lv_style_reset(&style_bento_box);
    lv_style_init(&style_bento_box);
    lv_style_set_bg_color(&style_bento_box, lv_color_hex(theme->bento_bg));
    lv_style_set_bg_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_radius(&style_bento_box, BENTO_RADIUS);
    lv_style_set_border_width(&style_bento_box, 1);
    lv_style_set_border_color(&style_bento_box, lv_color_hex(theme->bento_bg));
    lv_style_set_border_opa(&style_bento_box, LV_OPA_COVER);
    lv_style_set_pad_all(&style_bento_box, 20);

    lv_style_reset(&style_label_small);
    lv_style_init(&style_label_small);
    lv_style_set_text_color(&style_label_small, lv_color_hex(app_config_apply_brightness(theme->label_color, gb)));
    lv_style_set_text_font(&style_label_small, &lv_font_montserrat_16);
    lv_style_set_text_letter_space(&style_label_small, 1);

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
