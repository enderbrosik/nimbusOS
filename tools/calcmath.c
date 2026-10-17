/* Host test for the Calculator arithmetic in apps/calc_math.h. */
#include <stdio.h>
#include <string.h>
#include "apps/calc_math.h"

static int failures;

static void check_num(const char *text, calc_num want)
{
    calc_num got = 0;
    if (!calc_parse(text, &got) || got != want) {
        printf("parse \"%s\": want %lld, got %lld\n", text, want, got);
        failures++;
    }
}

static void check_bad(const char *text)
{
    calc_num got;
    if (calc_parse(text, &got)) {
        printf("parse \"%s\": should have been rejected\n", text);
        failures++;
    }
}

static void check_text(calc_num v, const char *want)
{
    char buf[32];
    calc_format(v, buf, sizeof buf);
    if (strcmp(buf, want)) {
        printf("format %lld: want \"%s\", got \"%s\"\n", v, want, buf);
        failures++;
    }
}

static void check_op(calc_num a, char op, calc_num b, int want_err, calc_num want)
{
    calc_num got = 0;
    int err = calc_apply(a, op, b, &got);
    if (err != want_err || (err == CALC_OK && got != want)) {
        printf("%lld %c %lld: want err %d value %lld, got err %d value %lld\n", a, op, b, want_err, want, err, got);
        failures++;
    }
}

int main(void)
{
    const calc_num S = CALC_SCALE;

    /* parsing: typed text to scaled values */
    check_num("0", 0);
    check_num("42", 42 * S);
    check_num("-3", -3 * S);
    check_num("12.5", 125 * S / 10);
    check_num("5.", 5 * S);
    check_num(".5", S / 2);
    check_num("0.000001", 1);
    check_num("-0.25", -S / 4);
    check_bad("");
    check_bad("-");
    check_bad(".");
    check_bad("1.2.3");
    check_bad("1e3");
    check_bad("0.0000001");          /* more than six decimals */
    check_bad("100000000000.5");     /* beyond the calculator's range */

    /* formatting: scaled values to text, no trailing zeros */
    check_text(0, "0");
    check_text(42 * S, "42");
    check_text(S + S / 2, "1.5");
    check_text(-S / 4, "-0.25");
    check_text(1, "0.000001");
    check_text(S / 20, "0.05");
    check_text(-(99999999999LL * S), "-99999999999");

    /* round trip */
    check_text(-(3 * S + 141592), "-3.141592");

    /* addition and subtraction */
    check_op(2 * S, '+', 3 * S, CALC_OK, 5 * S);
    check_op(1 * S / 10, '+', 2 * S / 10, CALC_OK, 3 * S / 10);
    check_op(7 * S, '-', 10 * S, CALC_OK, -3 * S);

    /* multiplication, including fractional and negative operands */
    check_op(6 * S, '*', 7 * S, CALC_OK, 42 * S);
    check_op(15 * S / 10, '*', 2 * S, CALC_OK, 3 * S);
    check_op(-2 * S, '*', 3 * S, CALC_OK, -6 * S);
    check_op(S / 2, '*', S / 2, CALC_OK, S / 4);
    check_op(123456 * S / 1000, '*', 0, CALC_OK, 0);
    check_op(10000000000LL * S, '*', 2 * S, CALC_OK, 20000000000LL * S);
    check_op(99999999999LL * S, '*', 99999999999LL * S, CALC_OVERFLOW, 0);

    /* division, including repeating decimals (six places, truncated) */
    check_op(1 * S, '/', 4 * S, CALC_OK, S / 4);
    check_op(10 * S, '/', 3 * S, CALC_OK, 3333333);
    check_op(-1 * S, '/', 2 * S, CALC_OK, -S / 2);
    check_op(1 * S, '/', -4 * S, CALC_OK, -S / 4);
    check_op(50 * S, '/', 100 * S, CALC_OK, S / 2);
    check_op(1 * S, '/', 0, CALC_DIV_ZERO, 0);
    check_op(99999999999LL * S, '/', S / 100, CALC_OVERFLOW, 0);

    if (failures) {
        printf("calcmath: %d FAILED\n", failures);
        return 1;
    }
    printf("calcmath: PASS\n");
    return 0;
}
