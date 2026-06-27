#pragma once

/**
 * @file control_registry.h
 * @brief Generic "Control API" registry — a curated table of runtime-settable
 *        controls (display, image display, behavior) plus the special non-config
 *        "page" item. Each item knows how to read/write itself from an
 *        app_config_t snapshot and how to apply its value live after a save.
 *
 * The Control API (web_handlers_control.c) exposes list/get/toggle/cycle/set/
 * adjust operations over this table. All config writes go through the standard
 * snapshot+modify+save path; the "page" item is non-config and routes through
 * the navigation arbiter instead.
 */

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

typedef enum {
    CTRL_TYPE_BOOL = 0,
    CTRL_TYPE_ENUM = 1,
    CTRL_TYPE_INT  = 2,
    CTRL_TYPE_PAGE = 3,   /* special non-config item: current page */
} ctrl_type_t;

typedef struct control_item control_item_t;

/* Read the item's current integer value from a config snapshot (bool as 0/1).
 * For the PAGE item, cfg is ignored and the live current page id is returned. */
typedef int  (*ctrl_get_fn)(const control_item_t *item, const app_config_t *cfg);
/* Write the item's value into a config snapshot (already clamped by caller).
 * NULL for the PAGE item (which does not touch config). */
typedef void (*ctrl_set_fn)(const control_item_t *item, app_config_t *cfg, int value);
/* Apply live preview after save. NULL = save suffices (poller reads it live). */
typedef void (*ctrl_apply_fn)(const app_config_t *prev, const app_config_t *cur);

struct control_item {
    const char        *name;        /* stable external key */
    ctrl_type_t        type;
    int                vmin;
    int                vmax;        /* -1 = runtime sentinel; see control_item_effective_max() */
    int                vstep;
    const char *const *labels;      /* enum labels, NULL otherwise */
    int                label_count; /* count of labels[] */
    ctrl_get_fn        get;
    ctrl_set_fn        set;         /* NULL for PAGE */
    ctrl_apply_fn      apply;       /* NULL = none */
};

int                    control_registry_count(void);
const control_item_t  *control_registry_get(int i);             /* by index, NULL if oob */
const control_item_t  *control_registry_find(const char *name); /* NULL if not found */

/* Human label for an item's current value (enum -> labels[v]; bool -> On/Off;
 * int -> NULL meaning "format the number"; page -> page label). out may be NULL.
 * Returns a const string when one exists, else NULL (caller formats the int). */
const char            *control_item_label(const control_item_t *item, int value);

/* Resolve an item's effective maximum value. The table stores vmax=-1 as a
 * runtime sentinel for items whose max depends on runtime state:
 *   theme        -> themes_get_count() - 1
 *   widget_style -> WIDGET_STYLE_COUNT - 1
 *   page         -> PAGE_REF_ID_MAX - 1 (the id space upper bound)
 * For every other item this returns item->vmax. Callers MUST use this for
 * clamping, cycle wrap, and the "max" reported in list/get JSON — never read
 * item->vmax directly. */
int                    control_item_effective_max(const control_item_t *item);

/* ---- PAGE item helpers (non-config; route through the nav arbiter) ---- */

/* Current page id (a page_ref_t value). */
int  control_page_current_id(void);
/* Navigate to the page named by @p id; returns false if not targetable/available. */
bool control_page_set_by_id(int id);
/* Advance to the next targetable + available page id (registry order), skipping
 * unavailable and non-targetable entries, wrapping around; navigates and returns
 * the new page id (or the current id if none could be selected). */
int  control_page_cycle_next(void);
