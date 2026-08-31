/* Host test for main/screen_geom.c: the safe area arithmetic for all three
 * panel rows, plus the inscribed-square rule the inset is derived from.
 * Integer only: a layout pass must never run floating point, so the sqrt(2)
 * ratio is SCREEN_SQRT2_1000 and every comparison here uses it.
 *
 * It also pins the BSP backlight curve (bsp/brightness_curve.h), which is the
 * only host-visible piece of the display bring-up: spec D.4 requires the square
 * panel's numbers to be unchanged and the BSP .c itself is not host compilable. */
#include "screen_geom.h"
/* The BSP is not on the host include path; reach its header-only brightness
 * curve by relative path, the same way test_session_stats.c reaches
 * main/ui/nina_session_stats.h. */
#include "../../components/esp32_p4_wifi6_touch_lcd/include/bsp/brightness_curve.h"

#include <stdio.h>

static int fails = 0;

static void check_int(const char *label, int got, int expect)
{
    printf("%-62s got=%-6d expect=%-6d %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_bool(const char *label, int got, int expect)
{
    printf("%-62s got=%-6s expect=%-6s %s\n", label,
           got ? "true" : "false", expect ? "true" : "false",
           (!got == !expect) ? "OK" : "FAIL");
    if (!got != !expect) fails++;
}

/* Distance from the centre to the corner of the safe square, in whole pixels,
 * using the integer sqrt(2) ratio: half_side * 1414 / 1000. */
static int corner_dist(int size, int inset)
{
    const int half_side = size / 2 - inset;
    return half_side * SCREEN_SQRT2_1000 / 1000;
}

/* Largest square that fits inside a circle of radius r: side = 2r / sqrt(2). */
static int inscribed_side(int radius)
{
    return 2 * radius * 1000 / SCREEN_SQRT2_1000;
}

static void check_row(const char *name, int size, int inset, int radius)
{
    char label[96];
    screen_geom_set(size, inset, radius);

    snprintf(label, sizeof(label), "%s: screen_size", name);
    check_int(label, screen_size(), size);
    snprintf(label, sizeof(label), "%s: screen_center", name);
    check_int(label, screen_center(), size / 2);
    snprintf(label, sizeof(label), "%s: screen_safe_inset", name);
    check_int(label, screen_safe_inset(), inset);
    snprintf(label, sizeof(label), "%s: screen_safe_radius", name);
    check_int(label, screen_safe_radius(), radius);

    if (radius == 0) {
        snprintf(label, sizeof(label), "%s: square panel has no inset", name);
        check_int(label, inset, 0);
        return;
    }

    /* The property that makes one inset serve all four rotations: a circular
     * safe area is rotation invariant, so the worst case is the corner. */
    snprintf(label, sizeof(label), "%s: inset corner is inside the safe radius", name);
    check_bool(label, corner_dist(size, inset) <= radius, 1);

    /* The inset is at least the inscribed-square inset, so it never claims
     * more area than the circle can show. */
    const int min_inset = (size - inscribed_side(radius)) / 2;
    snprintf(label, sizeof(label), "%s: inset >= inscribed-square inset (%d)", name, min_inset);
    check_bool(label, inset >= min_inset, 1);

    /* One pixel less would put the corner outside on the tight row, which is
     * what makes the 720 round value exact rather than generous. */
    snprintf(label, sizeof(label), "%s: safe square is not empty", name);
    check_bool(label, size - 2 * inset > 0, 1);
}

int main(void)
{
    printf("== panel rows ==\n");
    check_row("4b square 720",  720, 0,   0);
    check_row("3.4c round 800", 800, 118, 400);
    check_row("4c round 720",   720, 105, 360);

    printf("\n== inscribed square sizes ==\n");
    screen_geom_set(800, 118, 400);
    check_int("800 round: safe square side", screen_size() - 2 * screen_safe_inset(), 564);
    screen_geom_set(720, 105, 360);
    check_int("720 round: safe square side", screen_size() - 2 * screen_safe_inset(), 510);

    printf("\n== screen_geom_set rejects nonsense ==\n");
    screen_geom_set(720, 105, 360);
    screen_geom_set(0, 10, 10);
    check_int("zero size keeps the previous width", screen_size(), 720);
    screen_geom_set(4096, 10, 10);
    check_int("absurd size keeps the previous width", screen_size(), 720);
    screen_geom_set(720, 400, 360);
    check_int("inset wider than the panel clamps to 0", screen_safe_inset(), 0);
    screen_geom_set(720, 105, 900);
    check_int("radius past the panel clamps to 0", screen_safe_radius(), 0);

    printf("\n== default before screen_geom_set ==\n");
    printf("(the module initialises to the square 4B panel: 720, 0, 0)\n");

    /* Spec D.4 / A.2: the square backlight curve must be byte for byte what the
     * 4B shipped before the panel table existed. The expression it hardcoded
     * was `47 + (p * (100 - 47)) / 100` followed by `(1023 * actual) / 100`,
     * both integer, so these numbers are the whole contract. */
    printf("\n== brightness curve, square floor 47 ==\n");
    check_int("floor 47, 0%: actual percent",   bsp_brightness_actual_pct(47, 0),   47);
    check_int("floor 47, 1%: actual percent",   bsp_brightness_actual_pct(47, 1),   47);
    check_int("floor 47, 50%: actual percent",  bsp_brightness_actual_pct(47, 50),  73);
    check_int("floor 47, 100%: actual percent", bsp_brightness_actual_pct(47, 100), 100);
    check_int("floor 47, 0%: duty",   bsp_brightness_duty(47, 0),   480);
    check_int("floor 47, 1%: duty",   bsp_brightness_duty(47, 1),   480);
    check_int("floor 47, 50%: duty",  bsp_brightness_duty(47, 50),  746);
    check_int("floor 47, 100%: duty", bsp_brightness_duty(47, 100), 1023);

    printf("\n== brightness curve, round floor 0 is linear ==\n");
    check_int("floor 0, 0%: actual percent",   bsp_brightness_actual_pct(0, 0),   0);
    check_int("floor 0, 1%: actual percent",   bsp_brightness_actual_pct(0, 1),   1);
    check_int("floor 0, 50%: actual percent",  bsp_brightness_actual_pct(0, 50),  50);
    check_int("floor 0, 100%: actual percent", bsp_brightness_actual_pct(0, 100), 100);
    check_int("floor 0, 0%: duty",   bsp_brightness_duty(0, 0),   0);
    check_int("floor 0, 100%: duty", bsp_brightness_duty(0, 100), 1023);

    printf("\n== brightness curve, round panel floors ==\n");
    /* 3.4C stays dark up to about 21% duty and the 4C up to about 23%, so the
     * round rows floor there: user 0 is the last dark step, 100 is full duty. */
    check_int("floor 20, 0%: actual percent",   bsp_brightness_actual_pct(20, 0),   20);
    check_int("floor 20, 50%: actual percent",  bsp_brightness_actual_pct(20, 50),  60);
    check_int("floor 20, 100%: actual percent", bsp_brightness_actual_pct(20, 100), 100);
    check_int("floor 20, 100%: duty",           bsp_brightness_duty(20, 100),       1023);
    check_int("floor 22, 0%: actual percent",   bsp_brightness_actual_pct(22, 0),   22);
    check_int("floor 22, 50%: actual percent",  bsp_brightness_actual_pct(22, 50),  61);
    check_int("floor 22, 100%: actual percent", bsp_brightness_actual_pct(22, 100), 100);
    check_int("floor 22, 100%: duty",           bsp_brightness_duty(22, 100),       1023);

    printf("\n== brightness curve clamps out-of-range input ==\n");
    check_int("floor 47, -5% clamps to 0%",   bsp_brightness_actual_pct(47, -5),  47);
    check_int("floor 47, 150% clamps to 100%", bsp_brightness_actual_pct(47, 150), 100);

    printf("\n%s (%d failure%s)\n", fails == 0 ? "PASS" : "FAIL", fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
