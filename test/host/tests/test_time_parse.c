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
#include <string.h>

static int fails = 0;

static void expect_time(const char *label, time_t got, long long want) {
    int ok = ((long long)got == want);
    printf("%-52s got=%lld want=%lld %s\n", label, (long long)got, want, ok ? "OK" : "FAIL");
    if (!ok) fails++;
}

static void expect_dur(int32_t seconds, fmt_duration_style_t style, const char *want) {
    char got[24];
    fmt_duration(got, sizeof(got), seconds, style);
    int ok = (strcmp(got, want) == 0);
    printf("fmt_duration(%11d, %d)%-24s got=\"%s\" want=\"%s\" %s\n",
           (int)seconds, (int)style, "", got, want, ok ? "OK" : "FAIL");
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

    // ── fmt_duration ────────────────────────────────────────────────────
    // HM_COMPACT drops the hour field entirely below one hour; that branch is
    // the one every caller's label width was sized for.
    expect_dur(7530, FMT_DUR_HM_COMPACT, "2h 05m");
    expect_dur(3600, FMT_DUR_HM_COMPACT, "1h 00m");
    expect_dur(2700, FMT_DUR_HM_COMPACT, "45m");
    expect_dur(0,    FMT_DUR_HM_COMPACT, "0m");

    expect_dur(7530, FMT_DUR_HM_CLOCK,  "2:05");
    expect_dur(7539, FMT_DUR_HMS_CLOCK, "2:05:39");
    // Past 24 h the clock styles keep counting hours rather than wrapping.
    expect_dur(90000, FMT_DUR_HM_CLOCK, "25:00");

    // TIERED shows the largest two units only.
    expect_dur(7530, FMT_DUR_TIERED, "2h 5m");
    expect_dur(3600, FMT_DUR_TIERED, "1h 0m");
    expect_dur(309,  FMT_DUR_TIERED, "5m 9s");
    expect_dur(59,   FMT_DUR_TIERED, "59s");
    expect_dur(0,    FMT_DUR_TIERED, "0s");

    // UPTIME wraps hours into a day field, and pads hours once it does not.
    expect_dur(273909, FMT_DUR_UPTIME, "3d 04:05:09");
    expect_dur(14709,  FMT_DUR_UPTIME, "04:05:09");
    expect_dur(86399,  FMT_DUR_UPTIME, "23:59:59");
    expect_dur(86400,  FMT_DUR_UPTIME, "1d 00:00:00");

    // Negative spans clamp to zero rather than emitting "-1:-30".
    expect_dur(-90, FMT_DUR_HM_CLOCK,  "0:00");
    expect_dur(-90, FMT_DUR_TIERED,    "0s");
    expect_dur(-90, FMT_DUR_UPTIME,    "00:00:00");

    printf("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILURES", fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
