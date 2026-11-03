/* analysis_thirst_test: the needs-water state (#43 step 1).  Hand-built records, so each
 * expected state can be worked out by hand; two cases add seeded noise.  Exits 1 at the first
 * failed check. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thirst.h"

#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) {                                                                      \
            (void)fprintf(stderr, "analysis_thirst_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

#define NOW    1700000000000ull
#define MIN_MS 60000ull
#define DRY    565u
#define BAND   48u

static gt_record rec(uint64_t ts, uint16_t soil, uint16_t flags)
{
    gt_record r;

    (void)memset(&r, 0, sizeof r);
    r.ts_unix_ms = ts;
    r.soil_raw = soil;
    r.flags = flags;
    return r;
}

static thirst_acc start(uint16_t dry, uint16_t band)
{
    thirst_params p;
    thirst_acc    a;

    thirst_params_default(&p, dry);
    p.band = band;
    CHECK(thirst_acc_init(&a, &p) == 0);
    return a;
}

/* Feeds one record stamped `ago_s` seconds before NOW (all well inside the 3 minute limit). */
static void feed(thirst_acc* a, unsigned ago_s, uint16_t soil, uint16_t flags)
{
    gt_record r = rec(NOW - (uint64_t)ago_s * 1000ull, soil, flags);

    thirst_acc_feed(a, &r);
}

static thirst_result judge_at(const thirst_acc* a, uint64_t now)
{
    thirst_result out;

    thirst_judge(a, now, &out);
    return out;
}

static thirst_result judge(const thirst_acc* a)
{
    return judge_at(a, NOW);
}

static void test_empty_and_no_good_sample(void)
{
    thirst_acc    a = start(DRY, BAND);
    thirst_result r = judge(&a);

    CHECK(r.state == THIRST_UNKNOWN && r.reason == THIRST_R_NO_DATA);
    CHECK(r.dry_raw == DRY);

    /* Records that are all unusable are still "no data", never OK. */
    feed(&a, 3, 0, GT_REC_SOIL_INVALID);
    feed(&a, 2, 700, GT_REC_SOIL_STUCK);
    feed(&a, 1, 700, GT_REC_TIME_UNSET);
    r = judge(&a);
    CHECK(r.state == THIRST_UNKNOWN && r.reason == THIRST_R_NO_DATA);
    CHECK(r.level_raw == 0);
}

static void test_threshold_edge(void)
{
    thirst_acc a = start(DRY, BAND);

    /* Dry means AT or below the threshold. */
    feed(&a, 0, DRY, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);

    a = start(DRY, BAND);
    feed(&a, 0, DRY + 1u, 0);
    CHECK(judge(&a).state == THIRST_OK);
    CHECK(judge(&a).reason == THIRST_R_NONE);
    CHECK(judge(&a).level_raw == DRY + 1u);
}

static void test_hysteresis_edges(void)
{
    thirst_acc    a = start(DRY, BAND);
    thirst_result r;

    feed(&a, 9, 500, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);

    /* Above the threshold but short of dry + band: still needs water. */
    feed(&a, 8, DRY + 1u, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);
    feed(&a, 7, DRY + BAND - 1u, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);

    /* Reaching dry + band clears it. */
    feed(&a, 6, DRY + BAND, 0);
    r = judge(&a);
    CHECK(r.state == THIRST_OK && r.level_raw == DRY + BAND);

    /* Wet to dry again only at the threshold, not just under dry + band. */
    feed(&a, 5, DRY + 1u, 0);
    CHECK(judge(&a).state == THIRST_OK);
    feed(&a, 4, DRY, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);
}

/* A small seeded generator, so a failure repeats. */
static uint32_t rng_state = 12345u;

static int noise(int amp)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int)((rng_state >> 16) % (uint32_t)(2 * amp + 1)) - amp;
}

/* Counts state changes over `n` noisy samples around `centre`; `first` is fed before them. */
static unsigned flips(uint16_t band, int centre, uint16_t first, unsigned n)
{
    thirst_acc   a = start(DRY, band);
    thirst_state last;
    unsigned     changes = 0;
    unsigned     i;

    feed(&a, 0, first, 0);
    last = judge(&a).state;
    for (i = 0; i < n; i++) {
        thirst_state now;

        feed(&a, 0, (uint16_t)(centre + noise(16)), 0);
        now = judge(&a).state;
        if (now != last) {
            changes++;
            last = now;
        }
    }
    return changes;
}

static void test_noise_does_not_flip(void)
{
    /* Noise of +-16 around the threshold.  With the band, once dry it stays dry. */
    CHECK(flips(BAND, (int)DRY, 500, 2000) == 0);
    /* Noise of +-16 around dry + band, starting dry: it clears once and stays clear. */
    CHECK(flips(BAND, (int)(DRY + BAND), 500, 2000) == 1);
    /* The same noise with no band flaps all the time: the band is doing the work. */
    CHECK(flips(0, (int)DRY, 500, 2000) > 100);
}

static void test_unusable_records_are_skipped(void)
{
    thirst_acc    a = start(DRY, BAND);
    thirst_result r;

    feed(&a, 5, 700, 0);
    feed(&a, 4, 0, GT_REC_SOIL_INVALID);  /* stored as 0: must not read as bone dry */
    feed(&a, 3, 400, GT_REC_SOIL_STUCK);  /* value kept, but not a measurement */
    feed(&a, 2, 300, GT_REC_TIME_UNSET);  /* a low value with an untrusted clock */
    feed(&a, 1, 700, 0);
    r = judge(&a);
    CHECK(r.state == THIRST_OK && r.level_raw == 700);

    /* And the other way: a good dry sample is not undone by an unusable high one. */
    a = start(DRY, BAND);
    feed(&a, 4, 500, 0);
    feed(&a, 3, 900, GT_REC_SOIL_STUCK);
    feed(&a, 2, 900, GT_REC_SOIL_INVALID);
    feed(&a, 1, 500, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);
}

static void test_newest_bad_is_unknown(void)
{
    thirst_acc    a = start(DRY, BAND);
    thirst_result r;

    /* An older good OK sample must not stand in for a newest bad one. */
    feed(&a, 2, 700, 0);
    feed(&a, 1, 0, GT_REC_SOIL_INVALID);
    r = judge(&a);
    CHECK(r.state == THIRST_UNKNOWN && r.reason == THIRST_R_NEWEST_BAD);
    CHECK(r.level_raw == 700);

    /* ... nor an older NEEDS_WATER one. */
    a = start(DRY, BAND);
    feed(&a, 2, 500, 0);
    feed(&a, 1, 500, GT_REC_SOIL_STUCK);
    r = judge(&a);
    CHECK(r.state == THIRST_UNKNOWN && r.reason == THIRST_R_NEWEST_BAD);

    a = start(DRY, BAND);
    feed(&a, 2, 700, 0);
    feed(&a, 1, 700, GT_REC_TIME_UNSET);
    CHECK(judge(&a).reason == THIRST_R_NEWEST_BAD);

    /* The next good record makes it known again. */
    feed(&a, 0, 700, 0);
    CHECK(judge(&a).state == THIRST_OK);
}

static void test_stale_and_clock_ahead(void)
{
    thirst_acc    a = start(DRY, BAND);
    thirst_result r;
    uint64_t      ts = NOW;

    feed(&a, 0, 700, 0);

    /* Exactly three minutes old is still fresh; one millisecond more is stale. */
    r = judge_at(&a, ts + 3u * MIN_MS);
    CHECK(r.state == THIRST_OK && r.age_ms == 3u * MIN_MS);
    r = judge_at(&a, ts + 3u * MIN_MS + 1u);
    CHECK(r.state == THIRST_UNKNOWN && r.reason == THIRST_R_STALE);
    CHECK(r.level_raw == 700);

    /* A stale dry reading is not an alert either: the plant may have been watered since. */
    a = start(DRY, BAND);
    feed(&a, 0, 500, 0);
    CHECK(judge_at(&a, ts + 4u * MIN_MS).state == THIRST_UNKNOWN);

    /* The record is up to a minute ahead of now: fine.  More than that: clock ahead. */
    a = start(DRY, BAND);
    feed(&a, 0, 700, 0);
    r = judge_at(&a, ts - MIN_MS);
    CHECK(r.state == THIRST_OK && r.age_ms == 0);
    r = judge_at(&a, ts - MIN_MS - 1u);
    CHECK(r.state == THIRST_UNKNOWN && r.reason == THIRST_R_CLOCK_AHEAD);
}

static void test_params_and_extremes(void)
{
    thirst_params p;
    thirst_acc    a;

    thirst_params_default(&p, DRY);
    CHECK(p.band == 48u && p.fresh_ms == 3u * MIN_MS && p.ahead_ms == MIN_MS);
    p.fresh_ms = 0;
    errno = 0;
    CHECK(thirst_acc_init(&a, &p) == -1 && errno == EINVAL);

    /* dry_raw + band must not wrap at 65535. */
    a = start(65535u, 48u);
    feed(&a, 1, 65535u, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);
    feed(&a, 0, 65535u, 0);
    CHECK(judge(&a).state == THIRST_NEEDS_WATER);

    /* Threshold 0: nothing valid reads as dry. */
    a = start(0u, BAND);
    feed(&a, 0, 1u, 0);
    CHECK(judge(&a).state == THIRST_OK);
}

static void test_judge_does_not_change_the_accumulator(void)
{
    thirst_acc    a = start(DRY, BAND);
    thirst_acc    copy;
    thirst_result r1;
    thirst_result r2;

    feed(&a, 1, 500, 0);
    copy = a;
    r1 = judge(&a);
    r2 = judge(&a);
    /* Field by field: memcmp would also compare padding, which a struct copy need not keep. */
    CHECK(a.have_record == copy.have_record && a.newest_ts_ms == copy.newest_ts_ms);
    CHECK(a.newest_flags == copy.newest_flags && a.have_good == copy.have_good);
    CHECK(a.needs == copy.needs && a.level_raw == copy.level_raw);
    CHECK(a.good_ts_ms == copy.good_ts_ms);
    CHECK(r1.state == r2.state && r1.reason == r2.reason && r1.level_raw == r2.level_raw);
    CHECK(r1.dry_raw == r2.dry_raw && r1.age_ms == r2.age_ms);
}

static void test_names(void)
{
    CHECK(strcmp(thirst_state_name(THIRST_UNKNOWN), "unknown") == 0);
    CHECK(strcmp(thirst_state_name(THIRST_OK), "ok") == 0);
    CHECK(strcmp(thirst_state_name(THIRST_NEEDS_WATER), "needs_water") == 0);
    CHECK(strcmp(thirst_reason_name(THIRST_R_NONE), "none") == 0);
    CHECK(strcmp(thirst_reason_name(THIRST_R_NO_DATA), "no_data") == 0);
    CHECK(strcmp(thirst_reason_name(THIRST_R_NEWEST_BAD), "newest_bad") == 0);
    CHECK(strcmp(thirst_reason_name(THIRST_R_STALE), "stale") == 0);
    CHECK(strcmp(thirst_reason_name(THIRST_R_CLOCK_AHEAD), "clock_ahead") == 0);
}

int main(void)
{
    test_empty_and_no_good_sample();
    test_threshold_edge();
    test_hysteresis_edges();
    test_noise_does_not_flip();
    test_unusable_records_are_skipped();
    test_newest_bad_is_unknown();
    test_stale_and_clock_ahead();
    test_params_and_extremes();
    test_judge_does_not_change_the_accumulator();
    test_names();
    (void)puts("analysis_thirst_test: all passed");
    return 0;
}
