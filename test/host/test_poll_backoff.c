/* Host test for main/poll_backoff.h -- pure exponential-backoff step
 * function used by poll_task.c. No ESP-IDF dependency; assert-style like
 * test/host/test_route_auth.c. */
#include "poll_backoff.h"
#include <stdio.h>

static int fails = 0;

static void check_u32(const char *label, uint32_t got, uint32_t expect) {
    printf("%-60s got=%-10u expect=%-10u %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void) {
    /* -- no-backoff mode: initial==0 always returns 0 ----------------------- */
    check_u32("no-backoff: cur=0, initial=0", poll_backoff_next(0, 0, 5000), 0);
    check_u32("no-backoff: cur=1000, initial=0", poll_backoff_next(1000, 0, 5000), 0);
    check_u32("no-backoff: cur=0, initial=0, max=0",
              poll_backoff_next(0, 0, 0), 0);

    /* -- first failure: cur=0 starts at initial ------------------------------ */
    check_u32("first failure: initial=500, max=5000",
              poll_backoff_next(0, 500, 5000), 500);
    check_u32("first failure: initial exceeds max, clamps",
              poll_backoff_next(0, 8000, 5000), 5000);
    check_u32("first failure: max=0 (unbounded), uses initial",
              poll_backoff_next(0, 500, 0), 500);

    /* -- doubling ------------------------------------------------------------- */
    check_u32("double: 500 -> 1000", poll_backoff_next(500, 500, 5000), 1000);
    check_u32("double: 1000 -> 2000", poll_backoff_next(1000, 500, 5000), 2000);
    check_u32("double: 2000 -> 4000", poll_backoff_next(2000, 500, 5000), 4000);

    /* -- cap saturation --------------------------------------------------------- */
    check_u32("cap: 4000 -> 5000 (would overshoot to 8000)",
              poll_backoff_next(4000, 500, 5000), 5000);
    check_u32("cap: already at cap stays at cap",
              poll_backoff_next(5000, 500, 5000), 5000);
    check_u32("cap: unbounded (max=0) keeps doubling",
              poll_backoff_next(1000000, 500, 0), 2000000);

    /* -- reset semantics: caller responsibility, not this function ----------
     * poll_task.c resets its backoff-state variable to 0 after a successful
     * poll_once(); the very next failure then starts again at `initial`,
     * which is exactly the cur=0 case already covered above. */
    check_u32("reset-then-fail: behaves like first failure",
              poll_backoff_next(0, 500, 5000), 500);

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
