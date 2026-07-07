#pragma once

/**
 * @file page_registry.h
 * @brief Canonical page registry — single source of truth for every UI page's identity.
 *
 * Each navigable page, image source, and detail overlay is assigned a stable
 * numeric id (page_ref_t) and a stable string slug. The registry maps these
 * external identities onto the runtime absolute page index (the value the
 * Navigation Arbiter commits) and, for the Image Display page, the image
 * source index (0=GOES, 1=Moon, 2=Solar, 3=Custom, -1 = use configured default).
 *
 * APPEND-ONLY / FROZEN-IDS CONTRACT
 * ---------------------------------
 * The numeric ids 0..PAGE_REF_ID_MAX-1 are FROZEN forever. Several config
 * fields persist these ids verbatim in NVS:
 *   - auto_rotate_order2[]      (slideshow stop list; ids 0..11 = ARP_IDX_*)
 *   - idle_page_override_target (idle page id)
 *   - active_page_override      (Home Page id)
 * Renumbering or reordering an existing id would silently re-point a user's
 * persisted selection at a different page. Therefore:
 *   1. NEVER renumber, reorder, or remove an existing id.
 *   2. A new page gets the NEXT free id, appended at the end (and a slug).
 *   3. The slug is the stable EXTERNAL key for web/API use; ids are the
 *      stable INTERNAL/persisted key. Both are permanent.
 * Ids 0..11 are deliberately identical to the ARP_IDX_* constants in
 * app_config.h so the slideshow stop list and the registry share one numbering.
 *
 * NOTE: an `icon` field is reserved for a future additive append to
 * page_ref_entry_t. Do NOT add it now; add it only as a new trailing struct
 * member when actually needed, leaving existing initializers valid.
 */

#include <stdint.h>
#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Stable page identity. Persisted in NVS — see FROZEN-IDS contract above. */
typedef uint8_t page_ref_t;

/** Overlay-owner sentinel: "the current NINA page" (resolved at open time). */
#define PAGE_REF_ID_NINA_ANY ((page_ref_t)255)

/* ── Anchored ids (FROZEN forever, identical to ARP_IDX_*) ── */
#define PAGE_REF_SUMMARY        ((page_ref_t)0)
#define PAGE_REF_NINA1          ((page_ref_t)1)
#define PAGE_REF_NINA2          ((page_ref_t)2)
#define PAGE_REF_NINA3          ((page_ref_t)3)
#define PAGE_REF_SYSINFO        ((page_ref_t)4)
#define PAGE_REF_ALLSKY         ((page_ref_t)5)
#define PAGE_REF_SPOTIFY        ((page_ref_t)6)
#define PAGE_REF_CLOCK          ((page_ref_t)7)
#define PAGE_REF_IMG_GOES       ((page_ref_t)8)
#define PAGE_REF_IMG_MOON       ((page_ref_t)9)
#define PAGE_REF_IMG_SOLAR      ((page_ref_t)10)
#define PAGE_REF_IMG_CUSTOM     ((page_ref_t)11)

/* ── Appended ids (FROZEN forever) ── */
#define PAGE_REF_IMG_DEFAULT        ((page_ref_t)12)
#define PAGE_REF_SETTINGS           ((page_ref_t)13)
#define PAGE_REF_OVL_GRAPH          ((page_ref_t)14)
#define PAGE_REF_OVL_INFO_CAMERA    ((page_ref_t)15)
#define PAGE_REF_OVL_INFO_MOUNT     ((page_ref_t)16)
#define PAGE_REF_OVL_INFO_SEQUENCE  ((page_ref_t)17)
#define PAGE_REF_OVL_INFO_FILTER    ((page_ref_t)18)
#define PAGE_REF_OVL_INFO_AUTOFOCUS ((page_ref_t)19)
#define PAGE_REF_OVL_INFO_IMAGESTATS ((page_ref_t)20)
#define PAGE_REF_OVL_INFO_SESSION   ((page_ref_t)21)
#define PAGE_REF_OVL_THUMBNAIL      ((page_ref_t)22)
#define PAGE_REF_OVL_EVENTLOG       ((page_ref_t)23)

/** Exclusive upper bound on assigned ids. */
#define PAGE_REF_ID_MAX ((page_ref_t)24)

/** What kind of UI surface a page_ref names. */
typedef enum {
    PAGE_REF_KIND_PAGE         = 0,  /* a navigable top-level page                */
    PAGE_REF_KIND_IMAGE_SOURCE = 1,  /* the Image Display page with a given source */
    PAGE_REF_KIND_OVERLAY      = 2,  /* a modal overlay (not directly navigable)   */
} page_ref_kind_t;

/**
 * One registry entry. Static metadata only; page_idx/img_src below are the
 * values page_ref_resolve() returns at runtime (the table stores the static
 * mapping, resolution folds in availability).
 */
typedef struct {
    page_ref_t       id;          /* stable numeric id (FROZEN)                    */
    const char      *slug;        /* stable external key (FROZEN)                  */
    const char      *label;       /* human-readable label                         */
    page_ref_kind_t  kind;        /* page / image-source / overlay                */
    page_ref_t       owner;       /* owning page for overlays; self for pages     */
    bool             targetable;  /* true if a nav target (false for overlays/settings) */
    const char      *category;    /* grouping label for UI lists                  */
    int              page_idx;    /* absolute page index, or -1 (overlays)        */
    int8_t           img_src;     /* image source 0-3, or -1 (non image-source)   */
} page_ref_entry_t;

/** Number of entries in the registry table. */
int page_ref_count(void);

/** Get entry by table position [0, page_ref_count()); NULL if out of range. */
const page_ref_entry_t *page_ref_get(int i);

/** Get entry by numeric id; NULL if no entry has that id. */
const page_ref_entry_t *page_ref_by_id(page_ref_t id);

/** Get entry by slug (NULL-safe); NULL if no match. */
const page_ref_entry_t *page_ref_by_slug(const char *slug);

/** True if the page/source named by @p id is currently available to show. */
bool page_ref_is_available(page_ref_t id);

/**
 * Resolve @p id to its runtime absolute page index and image source.
 * Returns false if the id is unknown, unavailable, or not navigable (overlays).
 * @p page_idx_out and @p img_src_out may be NULL.
 */
bool page_ref_resolve(page_ref_t id, int *page_idx_out, int8_t *img_src_out);

/**
 * Navigate to the page named by @p id: resolve, set the image-source override
 * if needed, then issue a USER-claim navigation. Returns false if the id is
 * not targetable or not available.
 *
 * Caller must NOT hold the LVGL lock; the arbiter takes it internally.
 */
bool page_ref_navigate(page_ref_t id);

/**
 * OPTIONAL page lifecycle ops (Task 4.7 / retro 4.7, wave P7a).
 *
 * The identity table above (s_pages) is frozen metadata only — no function
 * pointers. This ops table is a SEPARATE, opt-in registration: a page module
 * may register a page_ops_t for its own id so generic dispatch code (see
 * nina_dashboard.c: hide_page_at/show_page_at/get_page_obj/apply_theme) can
 * drive it without a hardcoded branch. Pages that do not register ops keep
 * flowing through the existing hardcoded branches unchanged.
 *
 * All ops are called with the LVGL display lock already held by the caller
 * (dispatchers run under the same lock discipline as the branches they
 * replace). `create` and `get_obj` are NOT optional for a registered page —
 * dispatchers assume both are non-NULL if the ops pointer itself is non-NULL.
 */
typedef struct {
    lv_obj_t *(*create)(lv_obj_t *parent);   /* build page, return root obj */
    void (*destroy)(void);                    /* optional teardown; NULL = never destroyed */
    lv_obj_t *(*get_obj)(void);               /* current root obj or NULL */
    void (*show)(void);                       /* on-activate (after unhide); optional */
    void (*hide)(void);                       /* on-deactivate (before hide); optional */
    void (*apply_theme)(void);                /* re-theme existing widgets; optional */
    bool (*is_available)(void);               /* optional; NULL = derive from get_obj()!=NULL */
} page_ops_t;

/**
 * Register (or clear, with ops=NULL) the lifecycle ops for @p page_id.
 * @p ops must have static storage duration (the registry stores the pointer,
 * not a copy). Not thread-safe; call only from the UI/LVGL-owning task during
 * page creation, before any dispatcher can reference the id.
 */
void page_registry_set_ops(uint8_t page_id, const page_ops_t *ops);

/** Get the registered ops for @p page_id, or NULL if none registered. */
const page_ops_t *page_registry_get_ops(uint8_t page_id);

#ifdef __cplusplus
}
#endif
