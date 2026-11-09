/*
 * nina_profile_sets.h -- per-NINA-profile filter colours and RMS/HFR limits.
 *
 * Pure, header-only decisions over the per-node store JSON kept in NVS keys
 * "prof_sets0".."prof_sets2" (see app_config.c). The node's three working
 * fields in app_config_t (filter_colors, rms_thresholds, hfr_thresholds)
 * always belong to the profile named in "cur"; every other remembered profile
 * sits in "sets", most recently loaded first:
 *
 *   {"cur":{"id":"..","name":".."},
 *    "sets":[{"id":"..","name":"..","f":"<colours>","r":"<rms>","h":"<hfr>"}]}
 *
 * An empty, missing or unparsable store means "nothing recorded yet".
 * cJSON only, no ESP-IDF dependency; host-tested by
 * test/host/test_nina_profile_sets.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"

#define NPS_MAX_PROFILES 8      /* loaded one included, so at most 7 entries in "sets" */
#define NPS_STORE_MAX    3900   /* store text incl NUL; NVS strings cap at 4000 */
#define NPS_ID_MAX       64     /* profile id or name used as key, incl NUL */

typedef struct {
    char filter_colors[512];
    char rms[256];
    char hfr[256];
} nps_set_t;

typedef enum { NPS_NOOP = 0, NPS_ADOPTED, NPS_SWITCHED, NPS_RENAMED } nps_result_t;

/* Byte copy that always NUL-terminates and truncates at size - 1. Ids and
 * names go through this both when stored and when compared, so a long one
 * truncates the same way every time. */
static inline void nps_copy(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* String member of an object, or "" when absent or not a string. */
static inline const char *nps_str(const cJSON *obj, const char *key) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : "";
}

/* Parse a store; NULL when empty, garbage or not an object. */
static inline cJSON *nps_parse(const char *store) {
    if (!store || !store[0]) {
        return NULL;
    }
    cJSON *doc = cJSON_Parse(store);
    if (doc && !cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        doc = NULL;
    }
    return doc;
}

/* "sets" array of a parsed store, or NULL. */
static inline cJSON *nps_sets(cJSON *doc) {
    cJSON *sets = doc ? cJSON_GetObjectItemCaseSensitive(doc, "sets") : NULL;
    return cJSON_IsArray(sets) ? sets : NULL;
}

/* Append one normalized entry to arr. Returns false on OOM. */
static inline bool nps_add_entry(cJSON *arr, const char *id, const char *name,
                                 const char *f, const char *r, const char *h) {
    cJSON *e = cJSON_CreateObject();
    if (!e) {
        return false;
    }
    cJSON_AddStringToObject(e, "id", id);
    cJSON_AddStringToObject(e, "name", name);
    cJSON_AddStringToObject(e, "f", f);
    cJSON_AddStringToObject(e, "r", r);
    cJSON_AddStringToObject(e, "h", h);
    cJSON_AddItemToArray(arr, e);
    return true;
}

/* Serialize out into dst, dropping "sets" entries from the tail while there
 * are too many or the text does not fit. False when even an empty list does
 * not fit (dst untouched). */
static inline bool nps_emit(cJSON *out, cJSON *osets, char *dst, size_t dst_size) {
    size_t limit = dst_size < NPS_STORE_MAX ? dst_size : NPS_STORE_MAX;
    for (;;) {
        int count = osets ? cJSON_GetArraySize(osets) : 0;
        char *s = NULL;
        if (count <= NPS_MAX_PROFILES - 1) {
            s = cJSON_PrintUnformatted(out);
            if (!s) {
                return false;
            }
            size_t len = strlen(s);
            if (len + 1 <= limit) {
                memcpy(dst, s, len + 1);
                free(s);
                return true;
            }
            free(s);
        }
        if (count == 0) {
            return false;
        }
        cJSON_DeleteItemFromArray(osets, count - 1);
    }
}

static inline nps_result_t nps_switch(const char *store_in, const char *id, const char *name,
                                      nps_set_t *working, const nps_set_t *defaults,
                                      char *store_out, size_t store_out_size) {
    if (!id || !id[0] || !working || !defaults || !store_out || store_out_size == 0) {
        return NPS_NOOP;
    }
    char nid[NPS_ID_MAX];
    char nname[NPS_ID_MAX];
    nps_copy(nid, sizeof(nid), id);
    nps_copy(nname, sizeof(nname), name);

    cJSON *doc = nps_parse(store_in);
    const cJSON *cur = doc ? cJSON_GetObjectItemCaseSensitive(doc, "cur") : NULL;
    const char *cid = cJSON_IsObject(cur) ? nps_str(cur, "id") : "";
    const char *cname = cJSON_IsObject(cur) ? nps_str(cur, "name") : "";

    nps_result_t res;
    if (!cid[0]) {
        res = NPS_ADOPTED;
    } else if (strcmp(cid, nid) == 0) {
        res = (strcmp(cname, nname) == 0) ? NPS_NOOP : NPS_RENAMED;
    } else {
        res = NPS_SWITCHED;
    }
    if (res == NPS_NOOP) {
        cJSON_Delete(doc);
        return NPS_NOOP;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *ocur = out ? cJSON_AddObjectToObject(out, "cur") : NULL;
    cJSON *osets = out ? cJSON_AddArrayToObject(out, "sets") : NULL;
    bool ok = (ocur != NULL && osets != NULL);
    if (ok) {
        cJSON_AddStringToObject(ocur, "id", nid);
        cJSON_AddStringToObject(ocur, "name", nname);
    }
    if (ok && res == NPS_SWITCHED) {
        ok = nps_add_entry(osets, cid, cname, working->filter_colors, working->rms, working->hfr);
    }

    /* Carry the remembered list over in age order. The new profile's own
     * entry is taken out (its values become the working ones); any entry
     * repeating the old cur id is a stale duplicate and is dropped. */
    const cJSON *found = NULL;
    const cJSON *e = NULL;
    cJSON *isets = nps_sets(doc);
    if (ok && isets) {
        cJSON_ArrayForEach(e, isets) {
            if (!cJSON_IsObject(e)) {
                continue;
            }
            const char *eid = nps_str(e, "id");
            if (!eid[0]) {
                continue;
            }
            if (strcmp(eid, nid) == 0) {
                if (!found) {
                    found = e;
                }
                continue;
            }
            if (res == NPS_SWITCHED && strcmp(eid, cid) == 0) {
                continue;
            }
            if (!nps_add_entry(osets, eid, nps_str(e, "name"), nps_str(e, "f"),
                               nps_str(e, "r"), nps_str(e, "h"))) {
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        ok = nps_emit(out, osets, store_out, store_out_size);
    }
    if (ok && res == NPS_SWITCHED) {
        if (found) {
            const char *f = nps_str(found, "f");
            const char *r = nps_str(found, "r");
            const char *h = nps_str(found, "h");
            nps_copy(working->filter_colors, sizeof(working->filter_colors),
                     f[0] ? f : defaults->filter_colors);
            nps_copy(working->rms, sizeof(working->rms), r[0] ? r : defaults->rms);
            nps_copy(working->hfr, sizeof(working->hfr), h[0] ? h : defaults->hfr);
        } else {
            *working = *defaults;
        }
    }
    cJSON_Delete(out);
    cJSON_Delete(doc);
    return ok ? res : NPS_NOOP;
}

static inline bool nps_forget(const char *store_in, const char *id,
                              char *store_out, size_t store_out_size) {
    if (!id || !id[0] || !store_out || store_out_size == 0) {
        return false;
    }
    char nid[NPS_ID_MAX];
    nps_copy(nid, sizeof(nid), id);
    cJSON *doc = nps_parse(store_in);
    cJSON *sets = nps_sets(doc);
    int hit = -1;
    int i = 0;
    const cJSON *e = NULL;
    if (sets) {
        cJSON_ArrayForEach(e, sets) {
            if (cJSON_IsObject(e) && strcmp(nps_str(e, "id"), nid) == 0) {
                hit = i;
                break;
            }
            i++;
        }
    }
    bool ok = false;
    if (hit >= 0) {
        cJSON_DeleteItemFromArray(sets, hit);
        ok = nps_emit(doc, sets, store_out, store_out_size);
    }
    cJSON_Delete(doc);
    return ok;
}

static inline void nps_current(const char *store_in, char *id, size_t id_size,
                               char *name, size_t name_size) {
    cJSON *doc = nps_parse(store_in);
    const cJSON *cur = doc ? cJSON_GetObjectItemCaseSensitive(doc, "cur") : NULL;
    bool have = cJSON_IsObject(cur);
    nps_copy(id, id_size, have ? nps_str(cur, "id") : "");
    nps_copy(name, name_size, have ? nps_str(cur, "name") : "");
    cJSON_Delete(doc);
}

/* Does profile key `id` still exist in NINA? Implementations test against the
 * full parsed list NINA returned, never a truncated copy. */
typedef bool (*nps_exists_fn)(const char *id, void *ctx);
/* Told about each remembered profile the prune drops (for a log line). */
typedef void (*nps_removed_fn)(const char *id, const char *name, void *ctx);

/* Drop remembered profiles NINA no longer has. Does nothing (returns 0) when
 * the store is empty or garbage, nothing is loaded, or the loaded profile is
 * itself missing from NINA's list (a list that does not contain the loaded
 * profile is not trusted). The loaded profile is never removed. Returns the
 * number removed; store_out is written only when that is above 0.
 * on_removed may be NULL. */
static inline int nps_prune(const char *store_in, nps_exists_fn exists, void *ctx,
                            nps_removed_fn on_removed, void *removed_ctx,
                            char *store_out, size_t store_out_size) {
    if (!exists || !store_out || store_out_size == 0) {
        return 0;
    }
    cJSON *doc = nps_parse(store_in);
    const cJSON *cur = doc ? cJSON_GetObjectItemCaseSensitive(doc, "cur") : NULL;
    const char *cid = cJSON_IsObject(cur) ? nps_str(cur, "id") : "";
    cJSON *sets = nps_sets(doc);
    if (!cid[0] || !sets || !exists(cid, ctx)) {
        cJSON_Delete(doc);
        return 0;
    }
    int removed = 0;
    cJSON *e = sets->child;
    while (e) {
        cJSON *next = e->next;
        const char *eid = cJSON_IsObject(e) ? nps_str(e, "id") : "";
        if (eid[0] && !exists(eid, ctx)) {
            if (on_removed) {
                on_removed(eid, nps_str(e, "name"), removed_ctx);
            }
            cJSON_Delete(cJSON_DetachItemViaPointer(sets, e));
            removed++;
        }
        e = next;
    }
    if (removed > 0 && !nps_emit(doc, sets, store_out, store_out_size)) {
        removed = 0;
    }
    cJSON_Delete(doc);
    return removed;
}

/* Optional string member: absent is fine, otherwise a string shorter than max. */
static inline bool nps_opt_str_ok(const cJSON *obj, const char *key, size_t max) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return true;
    }
    return cJSON_IsString(it) && it->valuestring && strlen(it->valuestring) < max;
}

static inline bool nps_store_valid(const char *store) {
    if (!store) {
        return false;
    }
    if (!store[0]) {
        return true;
    }
    if (strlen(store) + 1 > NPS_STORE_MAX) {
        return false;
    }
    cJSON *doc = nps_parse(store);
    if (!doc) {
        return false;
    }
    bool ok = true;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(doc, "cur");
    if (cur) {
        ok = cJSON_IsObject(cur) && nps_opt_str_ok(cur, "id", NPS_ID_MAX) &&
             nps_opt_str_ok(cur, "name", NPS_ID_MAX);
    }
    const cJSON *sets = cJSON_GetObjectItemCaseSensitive(doc, "sets");
    if (ok && sets) {
        ok = cJSON_IsArray(sets) && cJSON_GetArraySize(sets) <= NPS_MAX_PROFILES - 1;
        const cJSON *e = NULL;
        if (ok) {
            cJSON_ArrayForEach(e, sets) {
                if (!cJSON_IsObject(e) || !nps_str(e, "id")[0] ||
                    !nps_opt_str_ok(e, "id", NPS_ID_MAX) ||
                    !nps_opt_str_ok(e, "name", NPS_ID_MAX) ||
                    !nps_opt_str_ok(e, "f", sizeof(((nps_set_t *)0)->filter_colors)) ||
                    !nps_opt_str_ok(e, "r", sizeof(((nps_set_t *)0)->rms)) ||
                    !nps_opt_str_ok(e, "h", sizeof(((nps_set_t *)0)->hfr))) {
                    ok = false;
                    break;
                }
            }
        }
    }
    cJSON_Delete(doc);
    return ok;
}
