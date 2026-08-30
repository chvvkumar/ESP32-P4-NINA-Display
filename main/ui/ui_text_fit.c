/**
 * @file ui_text_fit.c
 * @brief Font-ladder fitter and label write guard (see ui_text_fit.h).
 *
 * One slot table serves both jobs: the ladder pick memo and the write guard's
 * shadow copy. They are separate fields because they answer different
 * questions - `text` is the string the pick was measured against, `shown` is
 * the string last written - and folding them together would let a write skip
 * the next re-fit.
 */

#include "ui_text_fit.h"

#include <string.h>

#include "esp_heap_caps.h"

const lv_font_t *const UI_FIT_LADDER_NAME[UI_FIT_LADDER_NAME_N] = {
    &lv_font_montserrat_40, &lv_font_montserrat_36, &lv_font_montserrat_34,
    &lv_font_montserrat_32, &lv_font_montserrat_30, &lv_font_montserrat_28,
    &lv_font_montserrat_26, &lv_font_montserrat_24, &lv_font_montserrat_20,
};

const lv_font_t *const UI_FIT_LADDER_NAME_28[UI_FIT_LADDER_NAME_28_N] = {
    &lv_font_montserrat_28, &lv_font_montserrat_26,
    &lv_font_montserrat_24, &lv_font_montserrat_20,
};

/* 64 slots. A round capture layout routes about 14 labels through this table
 * and up to three NINA pages exist at once, so the first cut of 24 overflowed.
 * The table is NEVER evicted from: an overflow degrades that one call to the
 * behaviour this code had before the memo existed (measure every time, compare
 * against the label's own text) instead of stealing a live label's slot. An
 * eviction would both silently disable the write guard for the evicted label
 * and leave a delete hook on it that a later re-claim would duplicate.
 * Linear scan; the table is only touched on a label write. */
#define UI_FIT_SLOTS 64

typedef struct {
    const lv_obj_t  *label;
    const lv_font_t *pick;
    int32_t          avail;
    int32_t          letter_space;
    char             text[64];   /* what the pick was measured against */
    char             shown[64];  /* what ui_label_set_text() last wrote  */
} ui_fit_slot_t;

/* About 9 KB, so it lives in PSRAM per the project's allocation policy rather
 * than in .bss. Allocated on the first label write; a failed allocation latches
 * and every caller degrades to the un-memoised path. */
static ui_fit_slot_t *s_slots;
static bool           s_slots_failed;

static void ui_fit_delete_cb(lv_event_t *e);

/* Find or claim the slot for @p label.
 *
 * Returns NULL when the table is full or could not be allocated. NULL is not an
 * error: it means "no memo for this call", and both callers have a correct
 * slower path for it. Claiming registers the delete hook that releases the
 * slot, removing any hook this label already carries first, so a label that is
 * re-claimed after a release can never end up with two. */
static ui_fit_slot_t *ui_fit_slot(lv_obj_t *label)
{
    if (!s_slots) {
        if (s_slots_failed) return NULL;
        s_slots = heap_caps_calloc(UI_FIT_SLOTS, sizeof(*s_slots), MALLOC_CAP_SPIRAM);
        if (!s_slots) {
            s_slots_failed = true;
            return NULL;
        }
    }

    ui_fit_slot_t *empty = NULL;
    for (int i = 0; i < UI_FIT_SLOTS; i++) {
        if (s_slots[i].label == label) return &s_slots[i];
        if (!s_slots[i].label && !empty) empty = &s_slots[i];
    }
    if (!empty) return NULL;              /* full: degrade, never evict */

    memset(empty, 0, sizeof(*empty));
    empty->label = label;
    /* Seed the shadow with what the label already shows. A zeroed shadow reads
     * as "", so the first write of "" to a label created with other text would
     * be skipped as unchanged. */
    {
        const char *cur = lv_label_get_text(label);
        if (cur) {
            strncpy(empty->shown, cur, sizeof(empty->shown) - 1);
            empty->shown[sizeof(empty->shown) - 1] = 0;
        }
    }
    lv_obj_remove_event_cb(label, ui_fit_delete_cb);
    lv_obj_add_event_cb(label, ui_fit_delete_cb, LV_EVENT_DELETE, NULL);
    return empty;
}

static void ui_fit_delete_cb(lv_event_t *e)
{
    if (!s_slots) return;
    const lv_obj_t *obj = lv_event_get_target_obj(e);
    for (int i = 0; i < UI_FIT_SLOTS; i++) {
        if (s_slots[i].label == obj) memset(&s_slots[i], 0, sizeof(s_slots[i]));
    }
}

void ui_fit_label_font(lv_obj_t *label, const lv_font_t *const *ladder, int n,
                       int avail_px)
{
    if (!label || !ladder || n <= 0) return;

    const char *text = lv_label_get_text(label);
    int32_t letter_space = lv_obj_get_style_text_letter_space(label, 0);

    /* No slot means no memo: measure the ladder on every call, which is what
     * this did before the memo existed. Correct, just slower. */
    ui_fit_slot_t *slot = ui_fit_slot(label);
    const lv_font_t *pick;

    if (slot && slot->pick && slot->avail == avail_px &&
        slot->letter_space == letter_space &&
        strncmp(slot->text, text, sizeof(slot->text) - 1) == 0) {
        pick = slot->pick;
    } else {
        pick = ladder[n - 1];
        for (int i = 0; i < n; i++) {
            lv_point_t size;
            lv_text_get_size(&size, text, ladder[i], letter_space, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (size.x <= avail_px) {
                pick = ladder[i];
                break;
            }
        }
        if (slot) {
            slot->pick         = pick;
            slot->avail        = avail_px;
            slot->letter_space = letter_space;
            strncpy(slot->text, text, sizeof(slot->text) - 1);
            slot->text[sizeof(slot->text) - 1] = '\0';
        }
    }

    if (lv_obj_get_style_text_font(label, 0) != pick) {
        lv_obj_set_style_text_font(label, pick, 0);
    }
}

void ui_fit_label(lv_obj_t *label, const lv_font_t *const *ladder, int n,
                  int avail_px)
{
    if (!label || !ladder || n <= 0) return;

    ui_fit_label_font(label, ladder, n, avail_px);

    const lv_font_t *pick = lv_obj_get_style_text_font(label, 0);
    if (lv_label_get_long_mode(label) != LV_LABEL_LONG_DOT) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_width(label, avail_px);
    lv_obj_set_height(label, lv_font_get_line_height(pick));
}

void ui_label_set_text(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    ui_fit_slot_t *slot = ui_fit_slot(label);
    if (slot) {
        if (strncmp(slot->shown, text, sizeof(slot->shown) - 1) == 0) return;
        strncpy(slot->shown, text, sizeof(slot->shown) - 1);
        slot->shown[sizeof(slot->shown) - 1] = '\0';
    } else if (strcmp(lv_label_get_text(label), text) == 0) {
        /* No shadow copy for this label: fall back to the plain comparison the
         * rest of the code base uses. It stops matching once LVGL has
         * ellipsised the label in place, so an over-long string on an
         * un-memoised label repaints every poll. That is the pre-existing
         * behaviour, and it is only reachable once the table is genuinely
         * full. */
        return;
    }
    lv_label_set_text(label, text);
}
