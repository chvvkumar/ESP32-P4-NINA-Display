#include "nina_setup_screen.h"
#include "app_config.h"
#include "themes.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/* Provided by nina_dashboard.c; returns the active theme or NULL before init. */
const theme_t *nina_dashboard_get_theme(void);

static const char *TAG = "setup_screen";

static lv_obj_t *s_setup_cont = NULL;
static volatile bool s_setup_mode = false;

bool is_setup_mode(void)
{
    return s_setup_mode;
}

void set_setup_mode(bool enabled)
{
    s_setup_mode = enabled;
}

void nina_setup_screen_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Creating setup screen");

    /* Under the Red Night theme, substitute red shades for the hardcoded
     * white/gray/blue colors so the wizard does not leak bright colors.
     * When the theme is not yet initialized (NULL) or is not Red Night,
     * keep the original colors exactly. */
    const theme_t *t = nina_dashboard_get_theme();
    bool red = (t && theme_is_red_night(t));

    uint32_t col_title = red ? t->text_color : 0xFFFFFF;
    uint32_t col_secondary = red ? t->label_color : 0xCCCCCC;
    uint32_t col_divider = red ? t->bento_border : 0x444444;
    uint32_t col_waiting = red ? t->label_color : 0x555555;
    uint32_t col_value = red ? t->text_color : 0x3b82f6;
    uint32_t col_spin_ind = red ? t->progress_color : 0x3b82f6;
    uint32_t col_spin_track = red ? t->bento_border : 0x222222;

    /* Full-screen background */
    s_setup_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(s_setup_cont);
    lv_obj_set_size(s_setup_cont, 720, 720);
    lv_obj_set_style_bg_color(s_setup_cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_setup_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_setup_cont, 40, 0);
    lv_obj_set_style_pad_top(s_setup_cont, 100, 0);
    lv_obj_set_style_pad_row(s_setup_cont, 12, 0);
    lv_obj_set_flex_flow(s_setup_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_setup_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_setup_cont, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(s_setup_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s_setup_cont);
    lv_label_set_text(title, "WiFi Setup Required");
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(col_title), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* Subtitle */
    lv_obj_t *subtitle = lv_label_create(s_setup_cont);
    lv_label_set_text(subtitle, "Connect to this device's WiFi network\nand open the setup page in your browser.");
    lv_obj_set_width(subtitle, lv_pct(90));
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(col_secondary), 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    /* First divider */
    lv_obj_t *div1 = lv_obj_create(s_setup_cont);
    lv_obj_remove_style_all(div1);
    lv_obj_set_size(div1, lv_pct(80), 1);
    lv_obj_set_style_bg_color(div1, lv_color_hex(col_divider), 0);
    lv_obj_set_style_bg_opa(div1, LV_OPA_60, 0);

    /* Info rows */
    const app_config_t *cfg = app_config_get();
    const char *ap_name = (cfg && cfg->hostname[0] != '\0') ? cfg->hostname : "NINA-DISPLAY";

    const char *labels[] = {"Network:", "Password:", "Open in browser:"};
    const char *values[] = {ap_name, "12345678", "http://192.168.4.1"};

    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(s_setup_cont);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(85));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(col_secondary), 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, values[i]);
        lv_obj_set_style_text_font(val, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(col_value), 0);
    }

    /* Second divider */
    lv_obj_t *div2 = lv_obj_create(s_setup_cont);
    lv_obj_remove_style_all(div2);
    lv_obj_set_size(div2, lv_pct(80), 1);
    lv_obj_set_style_bg_color(div2, lv_color_hex(col_divider), 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_60, 0);

    /* Waiting text */
    lv_obj_t *waiting = lv_label_create(s_setup_cont);
    lv_label_set_text(waiting, "Waiting for configuration...");
    lv_obj_set_style_text_font(waiting, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(waiting, lv_color_hex(col_waiting), 0);
    lv_obj_set_style_text_align(waiting, LV_TEXT_ALIGN_CENTER, 0);

    /* Spinner */
    lv_obj_t *spinner = lv_spinner_create(s_setup_cont);
    lv_obj_set_size(spinner, 40, 40);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(col_spin_ind), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(col_spin_track), LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);

    ESP_LOGI(TAG, "Setup screen created");
}

void nina_setup_screen_destroy(void)
{
    if (s_setup_cont) {
        lv_obj_del(s_setup_cont);
        s_setup_cont = NULL;
        ESP_LOGI(TAG, "Setup screen destroyed");
    }
}
