/* Host test for main/nina_profile_sets.h (per-NINA-profile colours and limits). */
#include <stdio.h>
#include <string.h>
#include "nina_profile_sets.h"

static int s_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static char s_store[NPS_STORE_MAX];
static char s_out[NPS_STORE_MAX];

static void set_vals(nps_set_t *s, const char *f, const char *r, const char *h) {
    nps_copy(s->filter_colors, sizeof(s->filter_colors), f);
    nps_copy(s->rms, sizeof(s->rms), r);
    nps_copy(s->hfr, sizeof(s->hfr), h);
}

static int sets_count(const char *store) {
    cJSON *doc = cJSON_Parse(store);
    int n = cJSON_GetArraySize(cJSON_GetObjectItem(doc, "sets"));
    cJSON_Delete(doc);
    return n;
}

static void set_id_at(const char *store, int i, char *out, size_t n) {
    cJSON *doc = cJSON_Parse(store);
    cJSON *e = cJSON_GetArrayItem(cJSON_GetObjectItem(doc, "sets"), i);
    nps_copy(out, n, e ? nps_str(e, "id") : "");
    cJSON_Delete(doc);
}

/* Switch and copy store_out back into s_store when it was written. */
static nps_result_t sw(const char *id, const char *name, nps_set_t *w, const nps_set_t *d) {
    nps_result_t r = nps_switch(s_store, id, name, w, d, s_out, sizeof(s_out));
    if (r != NPS_NOOP) {
        memcpy(s_store, s_out, sizeof(s_store));
    }
    return r;
}

static const char *s_live[16];
static int s_live_n;
static bool live_cb(const char *id, void *ctx) {
    (void)ctx;
    for (int i = 0; i < s_live_n; i++) {
        if (strcmp(s_live[i], id) == 0) {
            return true;
        }
    }
    return false;
}
static int s_removed_n;
static void removed_cb(const char *id, const char *name, void *ctx) {
    (void)id; (void)name; (void)ctx;
    s_removed_n++;
}

int main(void) {
    nps_set_t w, d;
    char id[NPS_ID_MAX], name[NPS_ID_MAX], buf[NPS_ID_MAX];
    set_vals(&d, "{}", "{\"rd\":1}", "{\"hd\":1}");

    /* Adopt on an empty store: working untouched. */
    s_store[0] = '\0';
    set_vals(&w, "{\"L\":\"#111111\"}", "{\"ra\":1}", "{\"ha\":1}");
    CHECK(sw("A", "Alpha", &w, &d) == NPS_ADOPTED);
    CHECK(strcmp(w.filter_colors, "{\"L\":\"#111111\"}") == 0);
    nps_current(s_store, id, sizeof(id), name, sizeof(name));
    CHECK(strcmp(id, "A") == 0 && strcmp(name, "Alpha") == 0);
    CHECK(sets_count(s_store) == 0);

    /* Same id and name: no-op, store_out untouched. */
    strcpy(s_out, "sentinel");
    CHECK(nps_switch(s_store, "A", "Alpha", &w, &d, s_out, sizeof(s_out)) == NPS_NOOP);
    CHECK(strcmp(s_out, "sentinel") == 0);

    /* Rename: store updated, working untouched. */
    CHECK(sw("A", "Alpha2", &w, &d) == NPS_RENAMED);
    nps_current(s_store, id, sizeof(id), name, sizeof(name));
    CHECK(strcmp(name, "Alpha2") == 0);
    CHECK(strcmp(w.rms, "{\"ra\":1}") == 0);

    /* Switch to a new profile: defaults, old pushed to front. */
    CHECK(sw("B", "Beta", &w, &d) == NPS_SWITCHED);
    CHECK(memcmp(&w, &d, sizeof(w)) == 0);
    CHECK(sets_count(s_store) == 1);
    set_id_at(s_store, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "A") == 0);

    /* Switch back: A restored exactly, A entry removed, B pushed. */
    set_vals(&w, "{\"Ha\":\"#222222\"}", "{\"rb\":2}", "{\"hb\":2}");
    CHECK(sw("A", "Alpha2", &w, &d) == NPS_SWITCHED);
    CHECK(strcmp(w.filter_colors, "{\"L\":\"#111111\"}") == 0);
    CHECK(strcmp(w.rms, "{\"ra\":1}") == 0);
    CHECK(strcmp(w.hfr, "{\"ha\":1}") == 0);
    CHECK(sets_count(s_store) == 1);
    set_id_at(s_store, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "B") == 0);

    /* Age order: C then D; front is most recent. */
    CHECK(sw("C", "C", &w, &d) == NPS_SWITCHED);
    CHECK(sw("D", "D", &w, &d) == NPS_SWITCHED);
    set_id_at(s_store, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "C") == 0);
    set_id_at(s_store, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "A") == 0);
    set_id_at(s_store, 2, buf, sizeof(buf));
    CHECK(strcmp(buf, "B") == 0);

    /* 9th profile drops the oldest: loaded D + 7 remembered max. */
    s_store[0] = '\0';
    const char *ids[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7", "p8", "p9"};
    for (int i = 0; i < 9; i++) {
        nps_result_t r = sw(ids[i], ids[i], &w, &d);
        CHECK(r == (i == 0 ? NPS_ADOPTED : NPS_SWITCHED));
    }
    CHECK(sets_count(s_store) == NPS_MAX_PROFILES - 1);
    set_id_at(s_store, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "p8") == 0);
    set_id_at(s_store, 6, buf, sizeof(buf));
    CHECK(strcmp(buf, "p2") == 0);   /* p1 dropped */

    /* Oversize: big values drop from the tail until the store fits. */
    s_store[0] = '\0';
    char bigf[500];
    memset(bigf, 'x', sizeof(bigf) - 1);
    bigf[sizeof(bigf) - 1] = '\0';
    char bigr[250];
    memset(bigr, 'y', sizeof(bigr) - 1);
    bigr[sizeof(bigr) - 1] = '\0';
    for (int i = 0; i < 8; i++) {
        set_vals(&w, bigf, bigr, bigr);
        sw(ids[i], ids[i], &w, &d);
        CHECK(strlen(s_store) + 1 <= NPS_STORE_MAX);
    }
    CHECK(sets_count(s_store) < NPS_MAX_PROFILES - 1);
    CHECK(sets_count(s_store) >= 1);
    set_id_at(s_store, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "p7") == 0);   /* newest kept */
    CHECK(nps_store_valid(s_store));

    /* Small output buffer also honoured. */
    {
        char small[600];
        set_vals(&w, bigf, "{}", "{}");
        nps_result_t r = nps_switch(s_store, "zz", "zz", &w, &d, small, sizeof(small));
        CHECK(r == NPS_SWITCHED);
        CHECK(strlen(small) + 1 <= sizeof(small));
    }

    /* Forget hit and miss. */
    s_store[0] = '\0';
    sw("A", "A", &w, &d);
    sw("B", "B", &w, &d);
    sw("C", "C", &w, &d);   /* sets: B, A */
    CHECK(nps_forget(s_store, "A", s_out, sizeof(s_out)));
    CHECK(sets_count(s_out) == 1);
    CHECK(!nps_forget(s_store, "C", s_out, sizeof(s_out)));   /* loaded one is not remembered */
    CHECK(!nps_forget(s_store, "Q", s_out, sizeof(s_out)));
    CHECK(!nps_forget("garbage", "A", s_out, sizeof(s_out)));

    /* Garbage store behaves as empty. */
    strcpy(s_store, "{not json");
    set_vals(&w, "{\"k\":1}", "{}", "{}");
    CHECK(sw("A", "A", &w, &d) == NPS_ADOPTED);
    CHECK(strcmp(w.filter_colors, "{\"k\":1}") == 0);
    nps_current("[1,2]", id, sizeof(id), name, sizeof(name));
    CHECK(id[0] == '\0' && name[0] == '\0');
    CHECK(nps_switch("x", "", "n", &w, &d, s_out, sizeof(s_out)) == NPS_NOOP);
    CHECK(nps_switch("x", NULL, "n", &w, &d, s_out, sizeof(s_out)) == NPS_NOOP);

    /* Long ids truncate consistently. */
    {
        char longid[200];
        memset(longid, 'L', sizeof(longid) - 1);
        longid[sizeof(longid) - 1] = '\0';
        s_store[0] = '\0';
        CHECK(sw(longid, longid, &w, &d) == NPS_ADOPTED);
        CHECK(sw(longid, longid, &w, &d) == NPS_NOOP);
        nps_current(s_store, id, sizeof(id), name, sizeof(name));
        CHECK(strlen(id) == NPS_ID_MAX - 1);
        CHECK(nps_store_valid(s_store));
    }

    /* nps_store_valid. */
    CHECK(nps_store_valid(""));
    CHECK(!nps_store_valid(NULL));
    CHECK(!nps_store_valid("{bad"));
    CHECK(!nps_store_valid("[]"));
    CHECK(nps_store_valid("{}"));
    CHECK(nps_store_valid("{\"cur\":{\"id\":\"a\",\"name\":\"b\"},\"sets\":[]}"));
    CHECK(!nps_store_valid("{\"cur\":5}"));
    CHECK(!nps_store_valid("{\"sets\":[{\"name\":\"x\"}]}"));
    CHECK(!nps_store_valid("{\"sets\":{}}"));
    {
        char huge[NPS_STORE_MAX + 10];
        memset(huge, ' ', sizeof(huge) - 1);
        huge[0] = '{';
        huge[sizeof(huge) - 2] = '}';
        huge[sizeof(huge) - 1] = '\0';
        CHECK(!nps_store_valid(huge));
    }

    /* Prune. Store: cur C, sets B, A. */
    s_store[0] = '\0';
    sw("A", "A", &w, &d);
    sw("B", "B", &w, &d);
    sw("C", "C", &w, &d);
    sw("D", "D", &w, &d);   /* cur D, sets C, B, A */
    /* removes only missing ids, keeps order */
    s_live[0] = "D"; s_live[1] = "C"; s_live[2] = "A"; s_live_n = 3;
    s_removed_n = 0;
    CHECK(nps_prune(s_store, live_cb, NULL, removed_cb, NULL, s_out, sizeof(s_out)) == 1);
    CHECK(s_removed_n == 1);
    CHECK(sets_count(s_out) == 2);
    set_id_at(s_out, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "C") == 0);
    set_id_at(s_out, 1, buf, sizeof(buf));
    CHECK(strcmp(buf, "A") == 0);
    nps_current(s_out, id, sizeof(id), name, sizeof(name));
    CHECK(strcmp(id, "D") == 0);
    /* zero removed leaves store_out untouched */
    s_live[0] = "D"; s_live[1] = "C"; s_live[2] = "B"; s_live[3] = "A"; s_live_n = 4;
    strcpy(s_out, "sentinel");
    CHECK(nps_prune(s_store, live_cb, NULL, NULL, NULL, s_out, sizeof(s_out)) == 0);
    CHECK(strcmp(s_out, "sentinel") == 0);
    /* loaded profile missing from the list: list not trusted, nothing removed */
    s_live[0] = "C"; s_live_n = 1;
    CHECK(nps_prune(s_store, live_cb, NULL, NULL, NULL, s_out, sizeof(s_out)) == 0);
    CHECK(strcmp(s_out, "sentinel") == 0);
    /* garbage or empty store is a no-op */
    s_live[0] = "D"; s_live_n = 1;
    CHECK(nps_prune("{junk", live_cb, NULL, NULL, NULL, s_out, sizeof(s_out)) == 0);
    CHECK(nps_prune("", live_cb, NULL, NULL, NULL, s_out, sizeof(s_out)) == 0);
    CHECK(nps_prune(NULL, live_cb, NULL, NULL, NULL, s_out, sizeof(s_out)) == 0);
    CHECK(strcmp(s_out, "sentinel") == 0);
    /* only the loaded one remains in NINA: all remembered go, cur kept */
    CHECK(nps_prune(s_store, live_cb, NULL, NULL, NULL, s_out, sizeof(s_out)) == 3);
    CHECK(sets_count(s_out) == 0);
    nps_current(s_out, id, sizeof(id), name, sizeof(name));
    CHECK(strcmp(id, "D") == 0);

    if (s_fail) {
        printf("test_nina_profile_sets: %d FAILED\n", s_fail);
        return 1;
    }
    printf("test_nina_profile_sets: all passed\n");
    return 0;
}
