/*
 * time_parse.c - Pure, host-testable date/time string parsers.
 *
 * No ESP-IDF dependencies, no dynamic allocation, integer arithmetic only
 * (P4 FPU is single-precision; no doubles anywhere in this module).
 */

#include "time_parse.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

/*
 * Leap-year-aware epoch conversion from a UTC struct tm. Copied verbatim
 * from the static my_timegm() in nina_client.c (a later cleanup can dedupe;
 * nina_client.c is owned by another change and must not be touched here).
 */
static time_t my_timegm(struct tm *tm) {
    int64_t t = 0;
    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon; // 0-11

    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    for (int y = 1970; y < year; y++) {
        t += 365 * 24 * 3600;
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
            t += 24 * 3600;
        }
    }

    for (int m = 0; m < mon; m++) {
        t += days_per_month[m] * 24 * 3600;
        if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
            t += 24 * 3600;
        }
    }

    t += (tm->tm_mday - 1) * 24 * 3600;
    t += tm->tm_hour * 3600;
    t += tm->tm_min * 60;
    t += tm->tm_sec;

    return (time_t)t;
}

// Returns 0-11 for a 3-letter English month abbreviation, -1 otherwise.
static int month_from_abbrev(const char *p) {
    static const char *const names[12] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"
    };
    char m[4];
    for (int i = 0; i < 3; i++) {
        if (!isalpha((unsigned char)p[i])) return -1;
        m[i] = (char)tolower((unsigned char)p[i]);
    }
    m[3] = '\0';
    for (int i = 0; i < 12; i++) {
        if (strcmp(m, names[i]) == 0) return i;
    }
    return -1;
}

// Parses exactly min_digits..max_digits decimal digits; advances *p.
// Returns the value, or -1 on failure.
static int parse_uint_field(const char **p, int min_digits, int max_digits) {
    int val = 0;
    int n = 0;
    while (n < max_digits && isdigit((unsigned char)(*p)[0])) {
        val = val * 10 + ((*p)[0] - '0');
        (*p)++;
        n++;
    }
    if (n < min_digits) return -1;
    return val;
}

static const char *skip_spaces(const char *p) {
    while (*p == ' ') p++;
    return p;
}

time_t time_parse_rfc1123(const char *s) {
    if (!s || !*s) return 0;

    const char *p = skip_spaces(s);

    // Optional day-of-week prefix: letters followed by a comma. Skipped,
    // not validated (some servers send abbreviated or full day names).
    const char *q = p;
    while (isalpha((unsigned char)*q)) q++;
    if (*q == ',') {
        // Only treat the leading letters as a weekday if a comma follows;
        // otherwise they would be the month of a malformed string (reject
        // below via the day-of-month digit check).
        p = skip_spaces(q + 1);
    }

    int day = parse_uint_field(&p, 1, 2);
    if (day < 1 || day > 31 || *p != ' ') return 0;
    p = skip_spaces(p);

    int mon = month_from_abbrev(p);
    if (mon < 0) return 0;
    p += 3;
    if (*p != ' ') return 0;
    p = skip_spaces(p);

    int year = parse_uint_field(&p, 4, 4);
    if (year < 1970 || *p != ' ') return 0;
    p = skip_spaces(p);

    int hour = parse_uint_field(&p, 2, 2);
    if (hour < 0 || hour > 23 || *p != ':') return 0;
    p++;
    int min = parse_uint_field(&p, 2, 2);
    if (min < 0 || min > 59 || *p != ':') return 0;
    p++;
    int sec = parse_uint_field(&p, 2, 2);
    if (sec < 0 || sec > 60) return 0; // 60 tolerated for leap seconds

    // Optional timezone suffix: "GMT" or "UTC" (or nothing). Anything else
    // trailing (besides whitespace) is rejected as garbage.
    p = skip_spaces(p);
    if (*p != '\0') {
        if ((strncmp(p, "GMT", 3) != 0 && strncmp(p, "UTC", 3) != 0)) return 0;
        p = skip_spaces(p + 3);
        if (*p != '\0') return 0;
    }

    // Reject impossible day-of-month combinations (e.g. 31 Feb).
    static const int month_len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = month_len[mon];
    if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        max_day = 29;
    }
    if (day > max_day) return 0;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon = mon;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    return my_timegm(&tm);
}
