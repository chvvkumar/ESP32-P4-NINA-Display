/*
 * time_parse.h - Pure, host-testable date/time string parsers.
 *
 * No ESP-IDF dependencies; standard C only so the module can be compiled
 * unmodified into the host test suite (test/host).
 */

#ifndef TIME_PARSE_H
#define TIME_PARSE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse an RFC-1123 HTTP Date header ("Mon, 06 Jul 2026 19:06:19 GMT")
 * to a Unix epoch timestamp (UTC).
 *
 * - The day-of-week prefix, if present, is skipped without validation.
 * - Month is a 3-letter English abbreviation (case-insensitive).
 * - A trailing "GMT" or "UTC" suffix is tolerated but not required.
 * - Single-digit days ("Mon, 6 Jul 2026 ...") are accepted.
 *
 * Returns 0 on any parse failure (including NULL/empty input).
 */
time_t time_parse_rfc1123(const char *s);

/*
 * Elapsed/remaining-seconds render styles. Each value names the exact output
 * shape, because callers depend on the literal text (labels are diffed against
 * the previous frame). Add a style rather than bending an existing one.
 */
typedef enum {
    /* "2h 05m" when at least one hour, otherwise "45m".
     * Sequence/filter integration totals. */
    FMT_DUR_HM_COMPACT = 0,
    /* "2:05" — always hours:minutes, hours unpadded. */
    FMT_DUR_HM_CLOCK,
    /* "2:05:09" — always hours:minutes:seconds, hours unpadded. */
    FMT_DUR_HMS_CLOCK,
    /* Largest-two-units tier: "2h 5m", else "5m 9s", else "9s". */
    FMT_DUR_TIERED,
    /* Uptime: "3d 04:05:09" once past a day, otherwise "04:05:09". */
    FMT_DUR_UPTIME,
} fmt_duration_style_t;

/*
 * Render @p seconds into @p buf using @p style.
 *
 * Negative inputs are clamped to zero: every caller measures an elapsed or
 * remaining span, and a negative span only ever comes from clock skew or a
 * countdown that ran past its deadline. Rendering "-1:-30" helps nobody.
 *
 * @p buf is always NUL-terminated (unless @p sz is 0, when nothing is written).
 * 24 bytes is enough for every style at any int32_t input.
 */
void fmt_duration(char *buf, size_t sz, int32_t seconds, fmt_duration_style_t style);

#ifdef __cplusplus
}
#endif

#endif /* TIME_PARSE_H */
