/**
 * @file page_registry.c
 * @brief Canonical page registry implementation — see page_registry.h.
 *
 * The static table below holds one entry per page_ref id (0..PAGE_REF_ID_MAX-1),
 * in id order. page_idx / img_src in the table are the static mapping; the
 * runtime-correct values are produced by page_ref_resolve(), which folds in
 * availability (and is the only path callers should use to obtain a nav target).
 *
 * Availability replicates slideshow_build_candidates() in nina_nav_arbiter.c
 * exactly so the slideshow and the registry never disagree about which stops
 * are showable.
 *
 * SETTINGS/SYSINFO page indices: SETTINGS_PAGE_IDX()/SYSINFO_PAGE_IDX() expand
 * to (pc + NINA_PAGE_OFFSET[ + 1]). Evaluated with MAX_NINA_INSTANCES (a
 * compile-time constant equal to the reserved NINA band width / page_count),
 * the result is a constant expression and is therefore valid as a static
 * initializer. We store it statically rather than computing it lazily.
 */

#include "page_registry.h"

#include <string.h>

#include "esp_timer.h"

#include "app_config.h"
#include "nina_dashboard.h"
#include "nina_dashboard_internal.h"
#include "nina_nav_arbiter.h"
#include "tasks.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Static registry table — id order, one entry per id 0..PAGE_REF_ID_MAX-1.
 * page_idx/img_src here are the static mapping; resolve() returns them after
 * an availability check. */
static const page_ref_entry_t s_pages[] = {
    /* id 0  */ { PAGE_REF_SUMMARY,          "summary",                "Summary",          PAGE_REF_KIND_PAGE,         PAGE_REF_SUMMARY,      true,  "NINA",    PAGE_IDX_SUMMARY,                       -1 },
    /* id 1  */ { PAGE_REF_NINA1,            "nina.1",                 "NINA 1",           PAGE_REF_KIND_PAGE,         PAGE_REF_NINA1,        true,  "NINA",    NINA_PAGE_OFFSET + 0,                   -1 },
    /* id 2  */ { PAGE_REF_NINA2,            "nina.2",                 "NINA 2",           PAGE_REF_KIND_PAGE,         PAGE_REF_NINA2,        true,  "NINA",    NINA_PAGE_OFFSET + 1,                   -1 },
    /* id 3  */ { PAGE_REF_NINA3,            "nina.3",                 "NINA 3",           PAGE_REF_KIND_PAGE,         PAGE_REF_NINA3,        true,  "NINA",    NINA_PAGE_OFFSET + 2,                   -1 },
    /* id 4  */ { PAGE_REF_SYSINFO,          "sysinfo",                "System Info",      PAGE_REF_KIND_PAGE,         PAGE_REF_SYSINFO,      true,  "System",  SYSINFO_PAGE_IDX(MAX_NINA_INSTANCES),   -1 },
    /* id 5  */ { PAGE_REF_ALLSKY,           "allsky",                 "AllSky",           PAGE_REF_KIND_PAGE,         PAGE_REF_ALLSKY,       true,  "Ambient", PAGE_IDX_ALLSKY,                        -1 },
    /* id 6  */ { PAGE_REF_SPOTIFY,          "spotify",                "Spotify",          PAGE_REF_KIND_PAGE,         PAGE_REF_SPOTIFY,      true,  "Ambient", PAGE_IDX_SPOTIFY,                       -1 },
    /* id 7  */ { PAGE_REF_CLOCK,            "clock",                  "Clock",            PAGE_REF_KIND_PAGE,         PAGE_REF_CLOCK,        true,  "Ambient", PAGE_IDX_CLOCK,                         -1 },
    /* id 8  */ { PAGE_REF_IMG_GOES,         "image.goes",             "GOES Satellite",   PAGE_REF_KIND_IMAGE_SOURCE, PAGE_REF_IMG_GOES,     true,  "Image",   PAGE_IDX_IMAGE_DISPLAY,                  0 },
    /* id 9  */ { PAGE_REF_IMG_MOON,         "image.moon",             "Moon",             PAGE_REF_KIND_IMAGE_SOURCE, PAGE_REF_IMG_MOON,     true,  "Image",   PAGE_IDX_IMAGE_DISPLAY,                  1 },
    /* id 10 */ { PAGE_REF_IMG_SOLAR,        "image.solar",            "Solar",            PAGE_REF_KIND_IMAGE_SOURCE, PAGE_REF_IMG_SOLAR,    true,  "Image",   PAGE_IDX_IMAGE_DISPLAY,                  2 },
    /* id 11 */ { PAGE_REF_IMG_CUSTOM,       "image.custom",           "Custom Image",     PAGE_REF_KIND_IMAGE_SOURCE, PAGE_REF_IMG_CUSTOM,   true,  "Image",   PAGE_IDX_IMAGE_DISPLAY,                  3 },
    /* id 12: generic "saved default" entry is reachable for resolution/back-compat
     * but is NOT offered as a selectable nav target (targetable=false); selectors
     * show the concrete image sources (ids 8..11) instead. */
    /* id 12 */ { PAGE_REF_IMG_DEFAULT,      "image.default",          "Image Display",    PAGE_REF_KIND_IMAGE_SOURCE, PAGE_REF_IMG_DEFAULT,  false, "Image",   PAGE_IDX_IMAGE_DISPLAY,                 -1 },
    /* id 13 */ { PAGE_REF_SETTINGS,         "settings",               "Settings",         PAGE_REF_KIND_PAGE,         PAGE_REF_SETTINGS,     false, "System",  SETTINGS_PAGE_IDX(MAX_NINA_INSTANCES),  -1 },
    /* id 14 */ { PAGE_REF_OVL_GRAPH,        "overlay.graph",          "RMS/HFR Graph",    PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 15 */ { PAGE_REF_OVL_INFO_CAMERA,  "overlay.info.camera",    "Camera Details",   PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 16 */ { PAGE_REF_OVL_INFO_MOUNT,   "overlay.info.mount",     "Mount Details",    PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 17 */ { PAGE_REF_OVL_INFO_SEQUENCE,"overlay.info.sequence",  "Sequence Details", PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 18 */ { PAGE_REF_OVL_INFO_FILTER,  "overlay.info.filter",    "Filter Details",   PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 19 */ { PAGE_REF_OVL_INFO_AUTOFOCUS,"overlay.info.autofocus","Autofocus Details",PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 20 */ { PAGE_REF_OVL_INFO_IMAGESTATS,"overlay.info.imagestats","Image Statistics",PAGE_REF_KIND_OVERLAY,     PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 21 */ { PAGE_REF_OVL_INFO_SESSION, "overlay.info.session",   "Session Stats",    PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 22 */ { PAGE_REF_OVL_THUMBNAIL,    "overlay.thumbnail",      "Thumbnail",        PAGE_REF_KIND_OVERLAY,      PAGE_REF_ID_NINA_ANY,  false, "NINA",    -1,                                     -1 },
    /* id 23 */ { PAGE_REF_OVL_EVENTLOG,     "overlay.eventlog",       "Event Log",        PAGE_REF_KIND_OVERLAY,      PAGE_REF_SUMMARY,      false, "System",  -1,                                     -1 },
};

int page_ref_count(void)
{
    return (int)ARRAY_SIZE(s_pages);
}

const page_ref_entry_t *page_ref_get(int i)
{
    if (i < 0 || i >= (int)ARRAY_SIZE(s_pages)) {
        return NULL;
    }
    return &s_pages[i];
}

const page_ref_entry_t *page_ref_by_id(page_ref_t id)
{
    for (int i = 0; i < (int)ARRAY_SIZE(s_pages); i++) {
        if (s_pages[i].id == id) {
            return &s_pages[i];
        }
    }
    return NULL;
}

const page_ref_entry_t *page_ref_by_slug(const char *slug)
{
    if (slug == NULL) {
        return NULL;
    }
    for (int i = 0; i < (int)ARRAY_SIZE(s_pages); i++) {
        if (s_pages[i].slug != NULL && strcmp(s_pages[i].slug, slug) == 0) {
            return &s_pages[i];
        }
    }
    return NULL;
}

bool page_ref_is_available(page_ref_t id)
{
    const page_ref_entry_t *e = page_ref_by_id(id);
    if (e == NULL) {
        return false;
    }

    const app_config_t *c = app_config_get();

    /* Per-id availability — mirrors slideshow_build_candidates() exactly. */
    bool avail;
    switch (id) {
        case PAGE_REF_NINA1: avail = nina_dashboard_slot_available(0); break;
        case PAGE_REF_NINA2: avail = nina_dashboard_slot_available(1); break;
        case PAGE_REF_NINA3: avail = nina_dashboard_slot_available(2); break;
        case PAGE_REF_ALLSKY: avail = c->allsky_enabled; break;
        case PAGE_REF_SPOTIFY: avail = c->spotify_enabled; break;
        case PAGE_REF_IMG_GOES:
        case PAGE_REF_IMG_MOON:
        case PAGE_REF_IMG_SOLAR:
        case PAGE_REF_IMG_DEFAULT:
            avail = c->image_display_enabled;
            break;
        case PAGE_REF_IMG_CUSTOM:
            avail = c->image_display_enabled && c->custom_image_url[0] != '\0';
            break;
        case PAGE_REF_SETTINGS:           /* settings: never a slideshow/nav-list target */
        case PAGE_REF_OVL_GRAPH:          /* overlays: not directly navigable             */
        case PAGE_REF_OVL_INFO_CAMERA:
        case PAGE_REF_OVL_INFO_MOUNT:
        case PAGE_REF_OVL_INFO_SEQUENCE:
        case PAGE_REF_OVL_INFO_FILTER:
        case PAGE_REF_OVL_INFO_AUTOFOCUS:
        case PAGE_REF_OVL_INFO_IMAGESTATS:
        case PAGE_REF_OVL_INFO_SESSION:
        case PAGE_REF_OVL_THUMBNAIL:
        case PAGE_REF_OVL_EVENTLOG:
            return false;
        default:                          /* summary, sysinfo, clock */
            avail = true;
            break;
    }

    if (!avail) {
        return false;
    }

    /* Page-backed entries additionally require an allocated, navigable page —
     * same AND-in that slideshow_build_candidates() applies. */
    if (e->page_idx >= 0 && !nina_dashboard_page_is_available(e->page_idx)) {
        return false;
    }
    return true;
}

bool page_ref_resolve(page_ref_t id, int *page_idx_out, int8_t *img_src_out)
{
    const page_ref_entry_t *e = page_ref_by_id(id);
    if (e == NULL || !page_ref_is_available(id)) {
        return false;
    }
    /* Overlays have no nav target. */
    if (e->page_idx < 0) {
        return false;
    }
    if (page_idx_out != NULL) {
        *page_idx_out = e->page_idx;
    }
    if (img_src_out != NULL) {
        *img_src_out = e->img_src;
    }
    return true;
}

bool page_ref_navigate(page_ref_t id)
{
    int page_idx;
    int8_t img_src;

    const page_ref_entry_t *e = page_ref_by_id(id);
    if (e == NULL || !e->targetable) {
        return false;
    }
    if (!page_ref_resolve(id, &page_idx, &img_src)) {
        return false;
    }

    /* Apply the image-source override (-1 clears it for non-image / default). */
    image_source_set_override(img_src);

    /* USER-claim navigation. Caller must not hold the LVGL lock. Pass the
     * resolved img_src so the arbiter's USER rung re-applies the SAME source
     * instead of clearing the override to the persisted default. */
    nav_arbiter_submit_user(page_idx, esp_timer_get_time() / 1000, img_src);
    return true;
}
