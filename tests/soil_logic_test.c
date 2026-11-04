/* soil_logic_test: median, range and stuck rules, with no hardware.  Exits 1 at the first
 * failed check. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soil_logic.h"

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            (void)fprintf(stderr, "soil_logic_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

#define STUCK_N SOIL_STUCK_DEFAULT

static const soil_cfg CFG = {.lo = SOIL_LO_DEFAULT, .hi = SOIL_HI_DEFAULT, .stuck_n = STUCK_N};

/* Judge `value` n times in a row; true if every cycle came back as `expect`. */
static bool feed(soil_state* st, uint16_t value, unsigned n, uint16_t expect)
{
    for (unsigned i = 0; i < n; i++) {
        if (soil_judge(st, &CFG, true, value) != expect) {
            return false;
        }
    }
    return true;
}

static void median_cases(void)
{
    const uint16_t odd[] = {900, 100, 500, 300, 700};
    const uint16_t spike[] = {14620, 29100, 14610, 14630, 14615}; /* failure-table row 3 */
    const uint16_t even[] = {4, 1, 3, 2};
    uint16_t       copy[5];
    uint16_t       big[SOIL_MAX_SAMPLES + 1];

    CHECK(soil_median(odd, 1) == 900);
    CHECK(soil_median(odd, 5) == 500);
    CHECK(soil_median(spike, 5) == 14620); /* the spike does not move it */
    CHECK(soil_median(even, 4) == 3);      /* upper median */

    memcpy(copy, odd, sizeof copy); /* the caller's array is not reordered */
    (void)soil_median(copy, 5);
    CHECK(memcmp(copy, odd, sizeof copy) == 0);

    for (size_t i = 0; i <= SOIL_MAX_SAMPLES; i++) {
        big[i] = (uint16_t)(1000 - i);
    }
    CHECK(soil_median(big, SOIL_MAX_SAMPLES) == 985); /* 969..1000, index 16 */
    CHECK(soil_median(big, SOIL_MAX_SAMPLES + 1) == 0);
    CHECK(soil_median(odd, 0) == 0);
    CHECK(soil_median(NULL, 3) == 0);
}

static void range_cases(void)
{
    static const struct {
        uint16_t value;
        uint16_t expect;
    } steps[] = {
        {600, GT_ST_OK},
        {SOIL_LO_DEFAULT, GT_ST_OK}, /* bounds are inclusive */
        {SOIL_HI_DEFAULT, GT_ST_OK},
        {SOIL_LO_DEFAULT - 1, GT_ST_INVALID},
        {SOIL_HI_DEFAULT + 1, GT_ST_INVALID},
        {0, GT_ST_INVALID},
        {0xffff, GT_ST_INVALID}, /* the read right after i2cdetect */
        {600, GT_ST_OK},         /* recovers on its own */
    };
    soil_state st = {.repeat = 0};

    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        CHECK(feed(&st, steps[i].value, 1, steps[i].expect));
    }
}

static void stuck_cases(void)
{
    soil_state a = {.repeat = 0};
    soil_state b = {.repeat = 0};
    soil_state c = {.repeat = 0};
    soil_state d = {.have_last = true, .last = 700, .repeat = ~0u};

    /* The 60th identical cycle is the first STUCK one, not the 59th. */
    CHECK(feed(&a, 700, STUCK_N - 1, GT_ST_OK));
    CHECK(feed(&a, 700, 2, GT_ST_STUCK));
    CHECK(feed(&a, 701, 1, GT_ST_OK)); /* any change clears it */
    CHECK(a.repeat == 1);

    /* A repeated out-of-range value is both. */
    CHECK(feed(&b, 0xffff, STUCK_N - 1, GT_ST_INVALID));
    CHECK(feed(&b, 0xffff, 1, GT_ST_INVALID | GT_ST_STUCK));

    /* A failed read is INVALID and leaves the counter alone. */
    CHECK(soil_judge(&c, &CFG, false, 0) == GT_ST_INVALID);
    CHECK(!c.have_last && c.repeat == 0);
    CHECK(feed(&c, 700, STUCK_N - 1, GT_ST_OK));
    CHECK(soil_judge(&c, &CFG, false, 0) == GT_ST_INVALID);
    CHECK(c.repeat == STUCK_N - 1 && c.last == 700);
    CHECK(feed(&c, 700, 1, GT_ST_STUCK));

    /* The counter saturates instead of wrapping to zero. */
    CHECK(feed(&d, 700, 1, GT_ST_STUCK));
    CHECK(d.repeat == ~0u);
}

int main(void)
{
    median_cases();
    range_cases();
    stuck_cases();
    (void)puts("soil_logic_test: ok");
    return 0;
}
