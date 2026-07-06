/* Host test for main/time_parse.c — RFC-1123 HTTP Date header parser.
 *
 * time_parse.c is pure standard C (no ESP-IDF headers), so it links
 * directly with no shims or mocks. Expected epoch values were computed
 * independently (Unix epoch, UTC):
 *   2026-07-06 19:06:19 UTC = 1783364779
 *     (20640 days since 1970-01-01: 56 years * 365 + 14 leap days
 *      + 181 days Jan..Jun 2026 + 5; * 86400 + 68779)
 *   2020-02-29 12:00:00 UTC = 1582977600
 *     (2020-03-01 00:00 UTC = 1583020800, minus 12 h)
 *
 * Build: see test/host/CMakeLists.txt (add_nina_host_test(test_time_parse ...)).
 */

#include "time_parse.h"

#include <stdio.h>

static int fails = 0;

static void expect_time(const char *label, time_t got, long long want) {
    int ok = ((long long)got == want);
    printf("%-52s got=%lld want=%lld %s\n", label, (long long)got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    // Canonical RFC-1123 form.
    expect_time("canonical GMT",
                time_parse_rfc1123("Mon, 06 Jul 2026 19:06:19 GMT"), 1783364779LL);

    // Leap day in a leap year.
    expect_time("leap day 2020-02-29",
                time_parse_rfc1123("Sat, 29 Feb 2020 12:00:00 GMT"), 1582977600LL);

    // "UTC" suffix tolerated.
    expect_time("UTC suffix",
                time_parse_rfc1123("Mon, 06 Jul 2026 19:06:19 UTC"), 1783364779LL);

    // Missing timezone suffix tolerated.
    expect_time("missing suffix",
                time_parse_rfc1123("Mon, 06 Jul 2026 19:06:19"), 1783364779LL);

    // Single-digit day.
    expect_time("single-digit day",
                time_parse_rfc1123("Mon, 6 Jul 2026 19:06:19 GMT"), 1783364779LL);

    // No day-of-week prefix at all.
    expect_time("no weekday prefix",
                time_parse_rfc1123("06 Jul 2026 19:06:19 GMT"), 1783364779LL);

    // Epoch reference point.
    expect_time("epoch start",
                time_parse_rfc1123("Thu, 01 Jan 1970 00:00:00 GMT"), 0LL);

    // Failure cases -> 0.
    expect_time("garbage string", time_parse_rfc1123("not a date"), 0LL);
    expect_time("NULL input", time_parse_rfc1123(NULL), 0LL);
    expect_time("empty string", time_parse_rfc1123(""), 0LL);
    expect_time("truncated (no time)", time_parse_rfc1123("Mon, 06 Jul 2026"), 0LL);
    expect_time("truncated (mid-time)", time_parse_rfc1123("Mon, 06 Jul 2026 19:06"), 0LL);
    expect_time("bad month name", time_parse_rfc1123("Mon, 06 Xyz 2026 19:06:19 GMT"), 0LL);
    expect_time("day out of range", time_parse_rfc1123("Mon, 32 Jul 2026 19:06:19 GMT"), 0LL);
    expect_time("impossible 31 Feb", time_parse_rfc1123("Mon, 31 Feb 2026 19:06:19 GMT"), 0LL);
    expect_time("29 Feb non-leap", time_parse_rfc1123("Mon, 29 Feb 2026 19:06:19 GMT"), 0LL);
    expect_time("hour out of range", time_parse_rfc1123("Mon, 06 Jul 2026 24:06:19 GMT"), 0LL);
    expect_time("pre-epoch year", time_parse_rfc1123("Wed, 06 Jul 1966 19:06:19 GMT"), 0LL);
    expect_time("bad suffix", time_parse_rfc1123("Mon, 06 Jul 2026 19:06:19 PST"), 0LL);
    expect_time("trailing garbage", time_parse_rfc1123("Mon, 06 Jul 2026 19:06:19 GMT junk"), 0LL);

    printf("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILURES", fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
