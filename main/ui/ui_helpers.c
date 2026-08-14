/**
 * @file ui_helpers.c
 * @brief Shared themed-label and card/section/key-value widget factories.
 *
 * These replace the per-page private copies that had drifted apart (card pad
 * 12 vs 14, key/value fonts 18/18 vs 20/22 vs 18/20). Each call site passes the
 * values its page already used, so the rendered result is unchanged.
 */

#include "ui_helpers.h"

#include "app_config.h"

void ui_set_theme_text_color(lv_obj_t *obj, uint32_t theme_color) {
    if (theme_color == UI_COLOR_NONE) return;
    int gb = app_config_get()->color_brightness;
    lv_obj_set_style_text_color(obj, lv_color_hex(app_config_apply_brightness(theme_color, gb)), 0);
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t theme_color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    if (text) lv_label_set_text(lbl, text);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    ui_set_theme_text_color(lbl, theme_color);
    return lbl;
}

lv_obj_t *ui_card(lv_obj_t *parent, int pad, int row_gap) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_bento_box, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, row_gap, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_section_label(lv_obj_t *parent, const char *title) {
    lv_obj_t *lbl = ui_label(parent, title, &lv_font_montserrat_16,
                             UI_THEME_COLOR(label_color));
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    return lbl;
}

lv_obj_t *ui_kv(lv_obj_t *parent, const char *key, const lv_font_t *key_font,
                const lv_font_t *val_font)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    ui_label(row, key, key_font, UI_THEME_COLOR(label_color));
    return ui_label(row, "--", val_font, UI_THEME_COLOR(text_color));
}
