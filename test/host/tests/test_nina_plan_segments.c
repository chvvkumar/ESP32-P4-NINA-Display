/* Host test for main/nina_plan_segments.h -- the pure segment list the shared
 * sub bar draws: one segment per Smart Exposure per loop round, in execution
 * order, plus the single-image hide rule and the overflow collapse to one
 * segment per round. Header-only, no ESP-IDF/FreeRTOS/LVGL dependency; the
 * plan structs come from main/nina_plan_types.h, which main/nina_client.h
 * includes so the firmware and this test see one definition.
 *
 * Build: see test/host/CMakeLists.txt (add_nina_host_test(test_nina_plan_segments ...)).
 */
#include "nina_plan_segments.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;

static void check_int(const char *label, int got, int expect) {
    printf("%-62s got=%-6d expect=%-6d %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-62s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_f(const char *label, float got, float expect) {
    bool ok = fabsf(got - expect) < 0.0005f;
    printf("%-62s got=%-8.4f expect=%-8.4f %s\n", label, (double)got,
           (double)expect, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

/* The live shape from the plan spec: LRGBSHO, 10 rounds, 3 done, the third
 * item (G) running, the last (Oiii) disabled. Items 0 and 1 carry
 * completed 1 from the round that just finished; items 3..5 carry a STALE
 * completed 1 from the PREVIOUS round and must still read REMAINING. */
static void build_lrgbsho(nina_plan_t *p, int rounds) {
    static const char *filters[7] = { "L", "R", "G", "B", "Sii", "Ha", "Oiii" };
    memset(p, 0, sizeof(*p));
    p->n_items = 7;
    p->rounds = rounds;
    p->round_done = 3;
    p->running_idx = 2;
    p->total_images = rounds * 7;
    snprintf(p->container, sizeof(p->container), "LRGBSHO");
    for (int i = 0; i < 7; i++) {
        snprintf(p->items[i].filter, sizeof(p->items[i].filter), "%s", filters[i]);
        p->items[i].iterations = 1;
        p->items[i].completed = (i == 2) ? 0 : 1;   /* stale 1 on 3..5 */
        p->items[i].enabled = (i != 6);
        p->items[i].running = (i == 2);
    }
    p->items[6].completed = 0;
}

int main(void) {
    nina_seg_t segs[NINA_SEG_MAX];

    /* ---- (a) the 7 x 10 live plan --------------------------------------- */
    {
        nina_plan_t p;
        build_lrgbsho(&p, 10);
        int n = nina_plan_segments(&p, 0.4f, segs, NINA_SEG_MAX);
        check_int("a: 7 items x 10 rounds -> 70 segments", n, 70);
        check_bool("a: not hidden", nina_plan_hidden(&p), false);

        /* round-major order */
        check_int("a: seg 0 is round 0 item 0", segs[0].item, 0);
        check_int("a: seg 0 round", segs[0].round, 0);
        check_int("a: seg 8 is round 1 item 1", segs[8].item, 1);
        check_int("a: seg 8 round", segs[8].round, 1);

        /* rounds before round_done are all DONE (the disabled one stays a
         * placeholder in every round) */
        check_int("a: seg 0 state DONE", segs[0].state, NINA_SEG_DONE);
        check_f("a: seg 0 done_frac", segs[0].done_frac, 1.0f);
        check_int("a: seg 6 (disabled, round 0) PLACEHOLDER",
                  segs[6].state, NINA_SEG_PLACEHOLDER);

        /* the running round: round_done 3 x 7 items starts at segment 21
         * (the plan sketch's 30..36 predates the 7-per-round order) */
        check_int("a: seg 21 (round 3 item 0) DONE", segs[21].state, NINA_SEG_DONE);
        check_f("a: seg 21 done_frac", segs[21].done_frac, 1.0f);
        check_f("a: seg 21 fill_frac == done_frac", segs[21].fill_frac, 1.0f);
        check_int("a: seg 22 (round 3 item 1) DONE", segs[22].state, NINA_SEG_DONE);
        check_int("a: seg 23 (running item) ACTIVE", segs[23].state, NINA_SEG_ACTIVE);
        check_f("a: seg 23 done_frac (whole images only)", segs[23].done_frac, 0.0f);
        check_f("a: seg 23 fill_frac == exp_frac", segs[23].fill_frac, 0.4f);
        check_int("a: seg 24 REMAINING despite stale completed",
                  segs[24].state, NINA_SEG_REMAINING);
        check_f("a: seg 24 done_frac 0", segs[24].done_frac, 0.0f);
        check_int("a: seg 25 REMAINING despite stale completed",
                  segs[25].state, NINA_SEG_REMAINING);
        check_int("a: seg 26 REMAINING despite stale completed",
                  segs[26].state, NINA_SEG_REMAINING);
        check_int("a: seg 27 disabled -> PLACEHOLDER",
                  segs[27].state, NINA_SEG_PLACEHOLDER);
        check_int("a: seg 27 weight 1", segs[27].weight, 1);
        check_f("a: seg 27 never fills", segs[27].fill_frac, 0.0f);

        /* rounds after the running one */
        check_int("a: seg 28 (round 4 item 0) REMAINING",
                  segs[28].state, NINA_SEG_REMAINING);

        /* round_last on every 7th segment, nowhere else */
        int bad_last = 0;
        for (int i = 0; i < n; i++) {
            bool want = ((i % 7) == 6);
            if (segs[i].round_last != want) bad_last++;
            if (segs[i].round != i / 7) bad_last++;
        }
        check_int("a: round_last only on the 7th of each round", bad_last, 0);
    }

    /* ---- (b) a 20-iteration single item ---------------------------------- */
    {
        nina_plan_t p;
        memset(&p, 0, sizeof(p));
        p.n_items = 1;
        p.rounds = 1;
        p.round_done = 0;
        p.running_idx = 0;
        p.total_images = 20;
        snprintf(p.items[0].filter, sizeof(p.items[0].filter), "Ha");
        p.items[0].iterations = 20;
        p.items[0].completed = 5;
        p.items[0].enabled = true;
        p.items[0].running = true;

        int n = nina_plan_segments(&p, 0.5f, segs, NINA_SEG_MAX);
        check_int("b: 20 separate weight-1 segments", n, 20);
        for (int k = 0; k < 5; k++) {
            char label[64];
            snprintf(label, sizeof(label), "b: image %d weight 1", k);
            check_int(label, segs[k].weight, 1);
            snprintf(label, sizeof(label), "b: image %d DONE", k);
            check_int(label, segs[k].state, NINA_SEG_DONE);
            snprintf(label, sizeof(label), "b: image %d done_frac 1", k);
            check_f(label, segs[k].done_frac, 1.0f);
            snprintf(label, sizeof(label), "b: image %d fill_frac 1", k);
            check_f(label, segs[k].fill_frac, 1.0f);
        }
        check_int("b: image 5 ACTIVE", segs[5].state, NINA_SEG_ACTIVE);
        check_f("b: image 5 done_frac 0", segs[5].done_frac, 0.0f);
        check_f("b: image 5 fill_frac == exp_frac", segs[5].fill_frac, 0.5f);
        for (int k = 6; k < 20; k++) {
            char label[64];
            snprintf(label, sizeof(label), "b: image %d REMAINING", k);
            check_int(label, segs[k].state, NINA_SEG_REMAINING);
            snprintf(label, sizeof(label), "b: image %d done_frac 0", k);
            check_f(label, segs[k].done_frac, 0.0f);
        }
        check_bool("b: round_last on the last image only", segs[19].round_last, true);
        check_bool("b: not round_last on image 18", segs[18].round_last, false);
    }

    /* ---- (b2) a 3-iteration item (completed 1, running) between two
     * 1-iteration items: three separate weight-1 segments -------------------- */
    {
        nina_plan_t p;
        memset(&p, 0, sizeof(p));
        p.n_items = 3;
        p.rounds = 1;
        p.round_done = 0;
        p.running_idx = 1;
        p.total_images = 5;
        snprintf(p.items[0].filter, sizeof(p.items[0].filter), "L");
        p.items[0].iterations = 1;
        p.items[0].completed = 1;
        p.items[0].enabled = true;
        snprintf(p.items[1].filter, sizeof(p.items[1].filter), "Ha");
        p.items[1].iterations = 3;
        p.items[1].completed = 1;
        p.items[1].enabled = true;
        p.items[1].running = true;
        snprintf(p.items[2].filter, sizeof(p.items[2].filter), "R");
        p.items[2].iterations = 1;
        p.items[2].completed = 0;
        p.items[2].enabled = true;

        int n = nina_plan_segments(&p, 0.6f, segs, NINA_SEG_MAX);
        check_int("b2: 1 + 3 + 1 = 5 segments", n, 5);
        check_int("b2: seg 0 (item 0) DONE", segs[0].state, NINA_SEG_DONE);
        check_int("b2: seg 1 (item 1 image 0) DONE", segs[1].state, NINA_SEG_DONE);
        check_int("b2: seg 1 weight 1", segs[1].weight, 1);
        check_int("b2: seg 2 (item 1 image 1) ACTIVE", segs[2].state, NINA_SEG_ACTIVE);
        check_f("b2: seg 2 done_frac 0", segs[2].done_frac, 0.0f);
        check_f("b2: seg 2 fill_frac == exp_frac", segs[2].fill_frac, 0.6f);
        check_int("b2: seg 3 (item 1 image 2) REMAINING", segs[3].state, NINA_SEG_REMAINING);
        check_int("b2: seg 4 (item 2) REMAINING", segs[4].state, NINA_SEG_REMAINING);
        check_bool("b2: round_last only on seg 4", segs[4].round_last, true);
        check_bool("b2: not round_last on seg 3", segs[3].round_last, false);
    }

    /* ---- (c) the single-image hide rule ---------------------------------- */
    {
        nina_plan_t p;
        memset(&p, 0, sizeof(p));
        p.n_items = 1;
        p.rounds = 1;
        p.running_idx = 0;
        p.total_images = 1;
        p.items[0].iterations = 1;
        p.items[0].enabled = true;
        p.items[0].running = true;

        check_bool("c: one image -> hidden", nina_plan_hidden(&p), true);
        check_int("c: one image -> no segments",
                  nina_plan_segments(&p, 0.5f, segs, NINA_SEG_MAX), 0);
    }

    /* ---- (d) overflow: 20 rounds x 7 images/round = 140 images > 120 ------ */
    {
        nina_plan_t p;
        build_lrgbsho(&p, 20);
        int n = nina_plan_segments(&p, 0.5f, segs, NINA_SEG_MAX);
        check_int("d: overflow -> one segment per round", n, 20);
        check_int("d: item -1 marks a round segment", segs[0].item, -1);
        check_int("d: weight = images in the round", segs[0].weight, 7);
        check_bool("d: every round segment is round_last", segs[5].round_last, true);
        check_int("d: round before round_done is DONE", segs[2].state, NINA_SEG_DONE);
        check_f("d: that one is full", segs[2].done_frac, 1.0f);
        check_int("d: round_done is ACTIVE", segs[3].state, NINA_SEG_ACTIVE);
        /* 2 items finished of 7 images in the round, running item 0 of 1 */
        check_f("d: ACTIVE done_frac 2/7", segs[3].done_frac, 2.0f / 7.0f);
        check_f("d: ACTIVE fill_frac 2.5/7", segs[3].fill_frac, 2.5f / 7.0f);
        check_int("d: round after is REMAINING", segs[4].state, NINA_SEG_REMAINING);
        check_int("d: last round index", segs[19].round, 19);
    }

    /* ---- (e) no plan ----------------------------------------------------- */
    {
        nina_plan_t p;
        memset(&p, 0, sizeof(p));
        check_bool("e: n_items 0 -> hidden", nina_plan_hidden(&p), true);
        check_int("e: n_items 0 -> no segments",
                  nina_plan_segments(&p, 0.5f, segs, NINA_SEG_MAX), 0);
        check_int("e: NULL plan -> no segments",
                  nina_plan_segments(NULL, 0.5f, segs, NINA_SEG_MAX), 0);
        check_bool("e: NULL plan -> hidden", nina_plan_hidden(NULL), true);
    }

    /* ---- (f) an idle container: still RUNNING, nothing exposing ---------- */
    {
        nina_plan_t p;
        build_lrgbsho(&p, 10);
        p.running_idx = -1;
        p.items[2].running = false;
        int n = nina_plan_segments(&p, 0.5f, segs, NINA_SEG_MAX);
        check_int("f: idle plan still draws", n, 70);
        int active = 0;
        for (int i = 0; i < n; i++) {
            if (segs[i].state == NINA_SEG_ACTIVE) active++;
        }
        check_int("f: nothing is ACTIVE while idle", active, 0);
        check_int("f: finished item of the running round is DONE",
                  segs[21].state, NINA_SEG_DONE);
        check_int("f: unfinished item of the running round is REMAINING",
                  segs[23].state, NINA_SEG_REMAINING);
    }

    /* ---- (g) exp_frac is clamped, out-of-range plans do not overflow ----- */
    {
        nina_plan_t p;
        build_lrgbsho(&p, 10);
        int n = nina_plan_segments(&p, 9.0f, segs, NINA_SEG_MAX);
        check_int("g: still 70 segments", n, 70);
        check_f("g: fill_frac clamped to 1", segs[23].fill_frac, 1.0f);
        n = nina_plan_segments(&p, -3.0f, segs, NINA_SEG_MAX);
        check_f("g: negative exp_frac clamped to 0", segs[23].fill_frac, 0.0f);
        check_int("g: max smaller than the list truncates",
                  nina_plan_segments(&p, 0.0f, segs, 10), 10);
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
