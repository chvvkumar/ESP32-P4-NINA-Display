/* Host test for main/http_fetch_policy.h -- pure decision functions used by
 * http_fetch.c (redirect classification, retry-loop continuation, response
 * buffer sizing). No ESP-IDF dependency; assert-style like
 * test/host/test_route_auth.c. */
#include "http_fetch_policy.h"
#include <stdio.h>

static int fails = 0;

static void check_bool(const char *label, bool got, bool expect) {
    printf("%-60s got=%-5s expect=%-5s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_size(const char *label, size_t got, size_t expect) {
    printf("%-60s got=%-8zu expect=%-8zu %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

int main(void) {
    /* -- http_status_is_redirect: exact set esp_http_client can follow ----- */
    check_bool("redirect: 301", http_status_is_redirect(301), true);
    check_bool("redirect: 302", http_status_is_redirect(302), true);
    check_bool("redirect: 303", http_status_is_redirect(303), true);
    check_bool("redirect: 307", http_status_is_redirect(307), true);
    check_bool("redirect: 308", http_status_is_redirect(308), true);
    check_bool("redirect: 200 is not", http_status_is_redirect(200), false);
    check_bool("redirect: 304 is not", http_status_is_redirect(304), false);
    check_bool("redirect: 404 is not", http_status_is_redirect(404), false);
    check_bool("redirect: 500 is not", http_status_is_redirect(500), false);
    check_bool("redirect: -1 is not", http_status_is_redirect(-1), false);

    /* -- http_should_retry: attempt is 0-indexed --------------------------- */
    check_bool("retry: max_attempts=0 never retries",
               http_should_retry(0, 0), false);
    check_bool("retry: max_attempts=1, attempt=0 is last",
               http_should_retry(0, 1), false);
    check_bool("retry: max_attempts=3, attempt=0 has more",
               http_should_retry(0, 3), true);
    check_bool("retry: max_attempts=3, attempt=1 has more",
               http_should_retry(1, 3), true);
    check_bool("retry: max_attempts=3, attempt=2 is last",
               http_should_retry(2, 3), false);
    check_bool("retry: max_attempts=3, attempt=5 (past end)",
               http_should_retry(5, 3), false);

    /* -- http_buf_initial: known content-length ----------------------------- */
    check_size("buf_initial: known length 100, cap 65536",
               http_buf_initial(100, 65536), 101);
    check_size("buf_initial: known length exactly fills cap-1",
               http_buf_initial(999, 1000), 1000);
    check_size("buf_initial: known length caps at cap (too large)",
               http_buf_initial(70000, 65536), 65536);
    check_size("buf_initial: known length == cap exactly",
               http_buf_initial(65536, 65536), 65536);

    /* -- http_buf_initial: unknown/chunked (<=0) ---------------------------- */
    check_size("buf_initial: unknown length starts at 4096",
               http_buf_initial(0, 65536), 4096);
    check_size("buf_initial: negative length treated as unknown",
               http_buf_initial(-1, 65536), 4096);
    check_size("buf_initial: unknown length, cap below 4096",
               http_buf_initial(0, 2048), 2048);

    /* -- http_buf_initial: cap edge ------------------------------------------ */
    check_size("buf_initial: cap=0 -> 0 (caller must fail)",
               http_buf_initial(100, 0), 0);

    /* -- http_buf_grow: doubling ---------------------------------------------- */
    check_size("buf_grow: 4096 -> 8192 (cap 65536)",
               http_buf_grow(4096, 65536), 8192);
    check_size("buf_grow: 8192 -> 16384", http_buf_grow(8192, 65536), 16384);

    /* -- http_buf_grow: saturates at cap ------------------------------------- */
    check_size("buf_grow: 40000 -> cap 65536 (would overshoot doubling)",
               http_buf_grow(40000, 65536), 65536);

    /* -- http_buf_grow: cannot grow further ----------------------------------- */
    check_size("buf_grow: cur == cap -> 0", http_buf_grow(65536, 65536), 0);
    check_size("buf_grow: cur > cap -> 0", http_buf_grow(70000, 65536), 0);
    check_size("buf_grow: cur == 0 -> 0 (nothing to grow from)",
               http_buf_grow(0, 65536), 0);

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
