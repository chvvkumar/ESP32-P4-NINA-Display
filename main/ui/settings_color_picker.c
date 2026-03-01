/**
 * @file settings_color_picker.c
 * @brief Modal color picker popup with a 5x4 grid of preset color swatches.
 *
 * Shows a full-screen semi-transparent overlay with a centered card containing
 * 20 color swatches. Position 0 is always the current color (white border).
 * Tapping a swatch invokes the user callback and auto-hides. Tapping outside
 * the card dismisses without selection.
 */

#include "settings_color_picker.h"
#include "nina_dashboard_internal.h"  /* current_theme, OUTER_PADDING, SCREEN_SIZE etc */
#include "app_config.h"               /* app_config_apply_brightness, app_config_get */
#include "themes.h"
#include "ui_styles.h"                /* style_bento_box, ui_styles_set_widget_draw_cbs */

#include <string.h>

/* ── Preset palette (positions 1-19) ─────────────────────────────────── */
static const uint32_t preset_colors[] = {
    0x787878, /* L  - Luminance gray */
    0xDC2626, /* R  - Red */
    0x16A34A, /* G  - Green */
    0x2563EB, /* B  - Blue */
    0xB91C1C, /* Ha - H-alpha deep red */
    0x0D9488, /* OIII - Teal */
    0xEA580C, /* SII - Orange-red */
    0x15803D, /* Good green (threshold default) */
    0xCA8A04, /* Warn yellow (threshold default) */
    0xB91C1C, /* Bad red (threshold default) */
    0x10B981, /* Emerald */
    0xEAB308, /* Yellow */
    0xEF4444, /* Bright red */
    0xF97316, /* Orange */
    0x8B5CF6, /* Violet */
    0xEC4899, /* Pink */
    0x06B6D4, /* Cyan */
    0xFFFFFF, /* White */
    0x6B7280, /* Gray */
};
#define PRESET_COUNT  (sizeof(preset_colors) / sizeof(preset_colors[0]))
#define SWATCH_TOTAL  (1 + PRESET_COUNT)  /* 20: current + 19 presets */

/* ── Layout constants ────────────────────────────────────────────────── */
#define SWATCH_SIZE   56
#define SWATCH_GAP    8
#define SWATCH_RADIUS 8
#define GRID_COLS     5
#define CARD_PAD      20
#define CURRENT_BORDER_WIDTH 3

/* ── Static state ────────────────────────────────────────────────────── */
static lv_obj_t *cp_overlay = NULL;
static lv_obj_t *cp_card = NULL;
static lv_obj_t *cp_title = NULL;
static lv_obj_t *cp_hint = NULL;
static lv_obj_t *cp_swatches[20];
static void (*cp_callback)(uint32_t color, void *user_data) = NULL;
static void *cp_user_data = NULL;

/* ── Forward declarations ────────────────────────────────────────────── */
static void overlay_click_cb(lv_event_t *e);
static void swatch_click_cb(lv_event_t *e);
static void apply_theme_to_card(void);

/* ── Public API ──────────────────────────────────────────────────────── */

void color_picker_show(uint32_t current_color,
                       void (*cb)(uint32_t color, void *user_data),
                       void *user_data)
{
    /* If already visible, tear down and rebuild */
    if (cp_overlay) {
        color_picker_hide();
    }

    cp_callback = cb;
    cp_user_data = user_data;

    int gb = app_config_get()->color_brightness;

    /* ── Full-screen overlay ─────────────────────────────────────────── */
    cp_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cp_overlay);
    lv_obj_set_size(cp_overlay, SCREEN_SIZE, SCREEN_SIZE);
    lv_obj_set_style_bg_color(cp_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cp_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(cp_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cp_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cp_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(cp_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    /* ── Centered card ───────────────────────────────────────────────── */
    /* Card width: padding + 5 swatches + 4 gaps + padding */
    int card_w = CARD_PAD * 2 + GRID_COLS * SWATCH_SIZE + (GRID_COLS - 1) * SWATCH_GAP;
    /* Card height: padding + title + gap + 4 rows + 3 gaps + gap + hint + padding */
    int grid_h = 4 * SWATCH_SIZE + 3 * SWATCH_GAP;
    int card_h = CARD_PAD + 30 + 12 + grid_h + 12 + 20 + CARD_PAD;

    cp_card = lv_obj_create(cp_overlay);
    lv_obj_remove_style_all(cp_card);
    lv_obj_add_style(cp_card, &style_bento_box, 0);
    ui_styles_set_widget_draw_cbs(cp_card);
    lv_obj_set_size(cp_card, card_w, card_h);
    lv_obj_center(cp_card);
    lv_obj_clear_flag(cp_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cp_card, LV_OBJ_FLAG_CLICKABLE);  /* absorb clicks so overlay doesn't dismiss */

    /* Apply theme bg */
    if (current_theme) {
        lv_obj_set_style_bg_color(cp_card, lv_color_hex(current_theme->bento_bg), 0);
        lv_obj_set_style_bg_opa(cp_card, LV_OPA_COVER, 0);
    }

    /* Use flex column layout for the card content */
    lv_obj_set_flex_flow(cp_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cp_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cp_card, CARD_PAD, 0);
    lv_obj_set_style_pad_row(cp_card, 12, 0);

    /* ── Title ───────────────────────────────────────────────────────── */
    cp_title = lv_label_create(cp_card);
    lv_label_set_text(cp_title, "Select Color");
    lv_obj_set_style_text_font(cp_title, &lv_font_montserrat_22, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(cp_title,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* ── Grid container (flex row with wrap) ─────────────────────────── */
    lv_obj_t *grid = lv_obj_create(cp_card);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid,
                    GRID_COLS * SWATCH_SIZE + (GRID_COLS - 1) * SWATCH_GAP,
                    grid_h);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, SWATCH_GAP, 0);
    lv_obj_set_style_pad_row(grid, SWATCH_GAP, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Swatches ────────────────────────────────────────────────────── */
    memset(cp_swatches, 0, sizeof(cp_swatches));

    for (int i = 0; i < (int)SWATCH_TOTAL; i++) {
        uint32_t color = (i == 0) ? current_color : preset_colors[i - 1];

        lv_obj_t *sw = lv_obj_create(grid);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, SWATCH_SIZE, SWATCH_SIZE);
        lv_obj_set_style_bg_color(sw, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, SWATCH_RADIUS, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

        /* Current color (position 0) gets a white border */
        if (i == 0) {
            lv_obj_set_style_border_color(sw, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(sw, CURRENT_BORDER_WIDTH, 0);
            lv_obj_set_style_border_opa(sw, LV_OPA_COVER, 0);
        }

        /* Store color in user_data as uintptr_t for retrieval in callback */
        lv_obj_add_event_cb(sw, swatch_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)color);

        cp_swatches[i] = sw;
    }

    /* ── Hint label ──────────────────────────────────────────────────── */
    cp_hint = lv_label_create(cp_card);
    lv_label_set_text(cp_hint, "Tap a color to select");
    lv_obj_set_style_text_font(cp_hint, &lv_font_montserrat_16, 0);
    if (current_theme) {
        lv_obj_set_style_text_color(cp_hint,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
}

void color_picker_hide(void)
{
    if (cp_overlay) {
        lv_obj_delete(cp_overlay);
        cp_overlay = NULL;
        cp_card = NULL;
        cp_title = NULL;
        cp_hint = NULL;
        memset(cp_swatches, 0, sizeof(cp_swatches));
    }
    cp_callback = NULL;
    cp_user_data = NULL;
}

void color_picker_apply_theme(void)
{
    if (!cp_overlay || !cp_card) return;
    apply_theme_to_card();
}

/* ── Event handlers ──────────────────────────────────────────────────── */

/** Clicking the overlay (outside the card) dismisses the popup */
static void overlay_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    color_picker_hide();
}

/** Clicking a swatch invokes the callback and hides the popup */
static void swatch_click_cb(lv_event_t *e)
{
    uint32_t color = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    /* Cache the callback before hide() clears it */
    void (*cb)(uint32_t, void *) = cp_callback;
    void *ud = cp_user_data;

    color_picker_hide();

    if (cb) {
        cb(color, ud);
    }
}

/* ── Theme helpers ───────────────────────────────────────────────────── */

static void apply_theme_to_card(void)
{
    if (!current_theme) return;
    int gb = app_config_get()->color_brightness;

    /* Card background */
    if (cp_card) {
        lv_obj_set_style_bg_color(cp_card, lv_color_hex(current_theme->bento_bg), 0);
    }

    /* Title text */
    if (cp_title) {
        lv_obj_set_style_text_color(cp_title,
            lv_color_hex(app_config_apply_brightness(current_theme->text_color, gb)), 0);
    }

    /* Hint text */
    if (cp_hint) {
        lv_obj_set_style_text_color(cp_hint,
            lv_color_hex(app_config_apply_brightness(current_theme->label_color, gb)), 0);
    }
}
