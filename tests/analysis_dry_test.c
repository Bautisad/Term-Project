/* analysis_dry_test: dry-down cycle detection on synthetic soil series (#27 step 1).  The
 * series are built here with a seeded generator, so a failure repeats.  Exits 1 at the first
 * failed check. */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dry.h"

#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) {                                                                   \
            (void)fprintf(stderr, "analysis_dry_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

#define T0      1700000400000ull
#define MIN_MS  60000ull
#define HOUR_MS 3600000ull

static uint64_t g_seed = 12345u;

/* Uniform in [-amp, amp]. */
static double noise(double amp)
{
    double u;

    g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
    u = (double)(g_seed >> 33) / 2147483648.0;
    return (u * 2.0 - 1.0) * amp;
}

/* A fake hub: one record a minute into an accumulator. */
typedef struct sim {
    dry_acc* a;
    uint64_t t;
    uint32_t seq;
    double   amp;
} sim;

static void put(sim* s, uint16_t soil, uint16_t flags)
{
    gt_record r;

    (void)memset(&r, 0, sizeof r);
    r.ts_unix_ms = s->t;
    r.seq = s->seq++;
    r.soil_raw = soil;
    r.flags = flags;
    CHECK(dry_acc_visit(&r, GT_STORE_REC_OK, s->a) == 0);
    s->t += MIN_MS;
}

static void emit(sim* s, double y)
{
    put(s, (uint16_t)lround(y + noise(s->amp)), 0);
}

/* Dries from `from` at slope counts per hour for `hours`. */
static double fall(sim* s, double from, double slope, double hours)
{
    unsigned m;
    unsigned n = (unsigned)(hours * 60.0);

    for (m = 0; m < n; m++) {
        emit(s, from + slope * (double)m / 60.0);
    }
    return from + slope * hours;
}

/* Watering: a 30 minute rise. */
static void water(sim* s, double from, double to)
{
    unsigned m;

    for (m = 1; m <= 30; m++) {
        emit(s, from + (to - from) * (double)m / 30.0);
    }
}

/* `cycles` dry-downs of `hours` each at `slope`, starting wet at 900, with watering between. */
static void series(sim* s, unsigned cycles, double slope, double hours)
{
    unsigned k;
    double   y = 900.0;

    for (k = 0; k < cycles; k++) {
        if (k > 0) {
            water(s, y, 900.0);
        }
        y = fall(s, 900.0, slope, hours);
    }
}

static void begin(dry_acc* a, sim* s, double amp)
{
    CHECK(dry_acc_init(a, NULL) == 0);
    s->a = a;
    s->t = T0;
    s->seq = 1;
    s->amp = amp;
}

static void test_empty_and_params(void)
{
    dry_acc    a;
    dry_result r;
    dry_params p;

    CHECK(dry_acc_init(&a, NULL) == 0);
    dry_analyze(&a, &r);
    CHECK(r.n_cycles == 0u && r.cycles_total == 0u && r.buckets == 0u);
    CHECK(!r.have_last && r.records == 0u && r.used == 0u);
    dry_acc_free(&a);
    dry_acc_free(&a);

    dry_params_default(&p);
    p.bucket_ms = 999;
    errno = 0;
    CHECK(dry_acc_init(&a, &p) == -1 && errno == EINVAL);
    dry_acc_free(&a);
    dry_params_default(&p);
    p.settle_counts = p.rise_counts;
    CHECK(dry_acc_init(&a, &p) == -1);
    dry_params_default(&p);
    p.rise_counts = 0.0;
    CHECK(dry_acc_init(&a, &p) == -1);
    dry_params_default(&p);
    p.min_r2 = NAN;
    CHECK(dry_acc_init(&a, &p) == -1);
    dry_params_default(&p);
    p.min_r2 = 1.5;
    CHECK(dry_acc_init(&a, &p) == -1);
    dry_params_default(&p);
    p.max_gap_ms = p.bucket_ms - 1u;
    CHECK(dry_acc_init(&a, &p) == -1);
    dry_params_default(&p);
    p.min_cycle_ms = 0;
    CHECK(dry_acc_init(&a, &p) == -1);
    dry_params_default(&p);
    CHECK(p.max_buckets == DRY_MAX_BUCKETS);
    p.max_buckets = 0;
    CHECK(dry_acc_init(&a, &p) == -1);
    p.max_buckets = (size_t)DRY_MAX_BUCKETS + 1u;
    CHECK(dry_acc_init(&a, &p) == -1);
    p.max_buckets = DRY_MAX_BUCKETS;
    CHECK(dry_acc_init(&a, &p) == 0);
    dry_acc_free(&a);

    /* The visitor has the store's callback type. */
    {
        gt_store_visit_fn f = dry_acc_visit;
        CHECK(f != NULL);
    }
    CHECK(strcmp(dry_reject_name(DRY_OK), "ok") == 0);
    CHECK(strcmp(dry_reject_name(DRY_REJ_GAP), "gap") == 0);
    CHECK(strcmp(dry_reject_name(DRY_REJ_SHORT), "short") == 0);
    CHECK(strcmp(dry_reject_name(DRY_REJ_NOT_FALLING), "not_falling") == 0);
    CHECK(strcmp(dry_reject_name(DRY_REJ_POOR_FIT), "poor_fit") == 0);
}

/* Four 30 hour dry-downs at -8 counts/hour, three waterings between them. */
static void check_four_cycles(const dry_result* r, double slope_tol, double r2_min)
{
    size_t i;

    CHECK(r->cycles_total == 4u && r->n_cycles == 4u);
    for (i = 0; i < 4u; i++) {
        const dry_cycle* c = &r->cycles[i];

        CHECK(c->reject == DRY_OK);
        CHECK(c->complete == (i < 3u));
        CHECK(fabs(c->slope_per_h - (-8.0)) <= slope_tol);
        CHECK(c->r2 >= r2_min);
        CHECK(c->end_ms - c->start_ms >= 28u * HOUR_MS);
        CHECK(c->start_raw > 880.0 && c->start_raw < 920.0);
        if (i > 0u) {
            CHECK(c->start_ms > r->cycles[i - 1u].end_ms);
        }
    }
}

static void test_clean_cycles(void)
{
    dry_acc    a;
    dry_result r;
    sim        s;

    begin(&a, &s, 0.0);
    series(&s, 4, -8.0, 30.0);
    dry_analyze(&a, &r);
    check_four_cycles(&r, 0.4, 0.99);
    CHECK(r.have_last && r.records == r.used && r.skipped_invalid == 0u);

    /* Reading again changes nothing. */
    {
        dry_result r2;

        dry_analyze(&a, &r2);
        CHECK(r2.cycles_total == r.cycles_total && r2.n_cycles == r.n_cycles);
        CHECK(r2.cycles[3].slope_per_h == r.cycles[3].slope_per_h);
        CHECK(r2.buckets == r.buckets && r2.used == r.used);
    }
    dry_acc_free(&a);
}

static void test_noise(void)
{
    dry_acc    a;
    dry_result r;
    sim        s;

    /* +-16 counts is the single-read noise measured in S2. */
    begin(&a, &s, 16.0);
    series(&s, 4, -8.0, 30.0);
    dry_analyze(&a, &r);
    check_four_cycles(&r, 0.8, 0.95);
    dry_acc_free(&a);

    /* Noise alone on a flat soil is no cycle that can be used, and no false watering. */
    begin(&a, &s, 16.0);
    (void)fall(&s, 700.0, 0.0, 72.0);
    dry_analyze(&a, &r);
    CHECK(r.cycles_total == 1u && r.n_cycles == 1u);
    CHECK(!r.cycles[0].complete && r.cycles[0].reject != DRY_OK);
    dry_acc_free(&a);

    /* It falls, but as a step and not a line (15 h flat, a drop of 120, 15 h flat). */
    begin(&a, &s, 4.0);
    (void)fall(&s, 900.0, 0.0, 15.0);
    (void)fall(&s, 780.0, 0.0, 15.0);
    dry_analyze(&a, &r);
    CHECK(r.cycles_total == 1u && r.cycles[0].reject == DRY_REJ_POOR_FIT);
    CHECK(r.cycles[0].slope_per_h < 0.0 && r.cycles[0].r2 > 0.6 && r.cycles[0].r2 < 0.8);
    dry_acc_free(&a);

    /* A slow rise of 40 counts (humid air, say) is not watering. */
    begin(&a, &s, 4.0);
    (void)fall(&s, 700.0, 40.0 / 24.0, 24.0);
    dry_analyze(&a, &r);
    CHECK(r.cycles_total == 1u && r.cycles[0].reject == DRY_REJ_NOT_FALLING);
    dry_acc_free(&a);
}

static void test_skipped_records(void)
{
    dry_acc    a;
    dry_result r;
    sim        s;
    gt_record  bad;
    unsigned   i;

    begin(&a, &s, 0.0);
    (void)fall(&s, 900.0, -8.0, 10.0);
    /* The sensor drops out for 20 minutes: values that would wreck the fit if used. */
    for (i = 0; i < 20u; i++) {
        put(&s, 0, GT_REC_SOIL_INVALID);
    }
    for (i = 0; i < 20u; i++) {
        put(&s, 999, GT_REC_SOIL_STUCK);
    }
    for (i = 0; i < 5u; i++) {
        put(&s, 100, GT_REC_TIME_UNSET);
    }
    (void)memset(&bad, 0, sizeof bad);
    CHECK(dry_acc_visit(&bad, GT_STORE_REC_CORRUPT, &a) == 0);
    CHECK(dry_acc_visit(&bad, GT_STORE_REC_CORRUPT, &a) == 0);
    (void)fall(&s, 900.0 - 8.0 * (10.0 + 45.0 / 60.0), -8.0, 20.0);
    dry_analyze(&a, &r);
    CHECK(r.skipped_invalid == 20u && r.skipped_stuck == 20u && r.skipped_time_unset == 5u);
    CHECK(r.skipped_corrupt == 2u);
    CHECK(r.records == r.used + 45u);
    CHECK(r.n_cycles == 1u && r.cycles[0].reject == DRY_OK);
    CHECK(fabs(r.cycles[0].slope_per_h - (-8.0)) < 0.2);
    CHECK(r.cycles[0].r2 > 0.99);
    dry_acc_free(&a);
}

static void test_gaps(void)
{
    dry_acc    a;
    dry_result r;
    sim        s;

    /* A 3 hour hole inside a cycle is fine. */
    begin(&a, &s, 0.0);
    (void)fall(&s, 900.0, -8.0, 15.0);
    s.t += 3u * HOUR_MS;
    (void)fall(&s, 900.0 - 8.0 * 18.0, -8.0, 15.0);
    dry_analyze(&a, &r);
    CHECK(r.n_cycles == 1u && r.cycles[0].reject == DRY_OK);
    CHECK(fabs(r.cycles[0].slope_per_h - (-8.0)) < 0.2);
    dry_acc_free(&a);

    /* An 8 hour hole (the Pi was off) is not bridged. */
    begin(&a, &s, 0.0);
    (void)fall(&s, 900.0, -8.0, 15.0);
    s.t += 8u * HOUR_MS;
    (void)fall(&s, 900.0 - 8.0 * 23.0, -8.0, 15.0);
    dry_analyze(&a, &r);
    CHECK(r.n_cycles == 1u && r.cycles[0].reject == DRY_REJ_GAP);
    dry_acc_free(&a);
}

static void test_short_cycles(void)
{
    dry_acc    a;
    dry_result r;
    sim        s;
    size_t     i;

    /* Three 6 hour dry-downs, each dropping 120 so the watering is clear. */
    begin(&a, &s, 0.0);
    series(&s, 3, -20.0, 6.0);
    dry_analyze(&a, &r);
    CHECK(r.cycles_total == 3u);
    for (i = 0; i < 3u; i++) {
        CHECK(r.cycles[i].reject == DRY_REJ_SHORT);
    }
    dry_acc_free(&a);
}

static void test_order_and_partial_bucket(void)
{
    dry_acc    a;
    dry_result r;
    sim        s;
    gt_record  old;

    begin(&a, &s, 0.0);
    (void)fall(&s, 900.0, -8.0, 5.0);
    (void)memset(&old, 0, sizeof old);
    old.ts_unix_ms = T0 - 2u * HOUR_MS; /* two hours before the series began */
    old.soil_raw = 500;
    CHECK(dry_acc_visit(&old, GT_STORE_REC_OK, &a) == 0);
    dry_analyze(&a, &r);
    CHECK(r.out_of_order == 1u && r.used == 300u && r.records == 301u);
    dry_acc_free(&a);

    /* Five minutes of data: one partial bucket is still a point. */
    begin(&a, &s, 0.0);
    (void)fall(&s, 800.0, 0.0, 5.0 / 60.0);
    dry_analyze(&a, &r);
    CHECK(r.buckets == 1u && r.have_last && fabs(r.last_raw - 800.0) < 0.01);
    CHECK(r.n_cycles == 1u && r.cycles[0].reject == DRY_REJ_SHORT);
    dry_acc_free(&a);
}

static void test_limits(void)
{
    dry_acc    a;
    dry_result r;
    sim        s;
    dry_params p;
    uint64_t   t;
    gt_record  rec;
    int        stop = 0;
    size_t     i;

    /* 70 two-hour cycles: the newest 64 are kept, in time order. */
    begin(&a, &s, 0.0);
    series(&s, 70, -40.0, 2.0);
    CHECK(dry_acc_error(&a) == 0);
    dry_analyze(&a, &r);
    CHECK(r.cycles_total == 70u && r.n_cycles == DRY_MAX_CYCLES);
    CHECK(!r.cycles[DRY_MAX_CYCLES - 1u].complete);
    for (i = 1; i < r.n_cycles; i++) {
        CHECK(r.cycles[i].start_ms > r.cycles[i - 1u].start_ms);
    }
    dry_acc_free(&a);

    /* A small limit: the 21st bucket stops the read, after exactly 20 were kept. */
    dry_params_default(&p);
    p.max_buckets = 20;
    CHECK(dry_acc_init(&a, &p) == 0);
    (void)memset(&rec, 0, sizeof rec);
    rec.soil_raw = 700;
    for (t = 0; t < 30u; t++) {
        rec.ts_unix_ms = T0 + t * p.bucket_ms;
        stop = dry_acc_visit(&rec, GT_STORE_REC_OK, &a);
        if (stop != 0) {
            break;
        }
    }
    CHECK(t == 20u && stop == 1 && a.overflow && a.n == 20u && dry_acc_error(&a) == EOVERFLOW);
    dry_acc_free(&a);

    /* One bucket per second, until DRY_MAX_BUCKETS stops the read. */
    dry_params_default(&p);
    p.bucket_ms = 1000;
    CHECK(dry_acc_init(&a, &p) == 0);
    (void)memset(&rec, 0, sizeof rec);
    rec.soil_raw = 700;
    for (t = 0; t < (uint64_t)DRY_MAX_BUCKETS + 10u; t++) {
        rec.ts_unix_ms = T0 + t * 1000u;
        stop = dry_acc_visit(&rec, GT_STORE_REC_OK, &a);
        if (stop != 0) {
            break;
        }
    }
    CHECK(stop == 1 && a.overflow && a.n == DRY_MAX_BUCKETS);
    CHECK(dry_acc_error(&a) == EOVERFLOW);
    a.oom = true; /* an allocation failure outranks it */
    CHECK(dry_acc_error(&a) == ENOMEM);
    a.oom = false;
    CHECK(dry_acc_visit(&rec, GT_STORE_REC_OK, &a) == 1);
    dry_analyze(&a, &r);
    CHECK(r.buckets >= DRY_MAX_BUCKETS);
    dry_acc_free(&a);
}

int main(void)
{
    test_empty_and_params();
    test_clean_cycles();
    test_noise();
    test_skipped_records();
    test_gaps();
    test_short_cycles();
    test_order_and_partial_bucket();
    test_limits();
    (void)puts("analysis_dry_test: ok");
    return 0;
}
