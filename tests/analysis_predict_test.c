/* analysis_predict_test: time-until-dry from dry-down cycles (#27 step 2).  Most cases use
 * hand-built cycle lists so the expected hours can be worked out by hand; two run the whole way
 * from synthetic records.  Exits 1 at the first failed check. */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "predict.h"

#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            (void)fprintf(stderr, "analysis_predict_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

#define NOW     1700000000000ull
#define MIN_MS  60000ull
#define HOUR_MS 3600000ull

static void add(dry_result* r, double slope, bool complete, dry_reject rej)
{
    dry_cycle* c;

    CHECK(r->n_cycles < DRY_MAX_CYCLES);
    c = &r->cycles[r->n_cycles++];
    (void)memset(c, 0, sizeof *c);
    c->slope_per_h = slope;
    c->complete = complete;
    c->reject = rej;
    r->cycles_total++;
}

/* A result whose newest good sample is `level`, 5 minutes old. */
static void fresh(dry_result* r, double level)
{
    (void)memset(r, 0, sizeof *r);
    r->have_last = true;
    r->last_ts_ms = NOW - 5u * MIN_MS;
    r->last_raw = level;
}

static predict_result run(const dry_result* r, double dry, uint64_t now)
{
    predict_params p;
    predict_result out;

    predict_params_default(&p, dry);
    CHECK(predict_time_until_dry(r, &p, now, &out) == 0);
    return out;
}

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

static void test_estimate_by_hand(void)
{
    dry_result     r;
    predict_result o;

    /* Slopes -8, -10, -6 and an open cycle: median -8, level 700, dry 565. */
    fresh(&r, 700.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -10.0, true, DRY_OK);
    add(&r, -6.0, true, DRY_OK);
    add(&r, -7.5, false, DRY_OK);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_OK && !o.already_dry);
    CHECK(o.have_cycles == 3u && o.need_cycles == 3u);
    CHECK(near(o.rate_per_h, -8.0));
    CHECK(near(o.hours, 135.0 / 8.0));      /* 16.875 */
    CHECK(near(o.hours_fast, 135.0 / 10.0)); /* 13.5 */
    CHECK(near(o.hours_slow, 135.0 / 6.0));  /* 22.5 */
    CHECK(near(o.level_raw, 700.0) && near(o.dry_raw, 565.0) && o.age_ms == 5u * MIN_MS);

    /* An even number of cycles takes the middle two: -8 and -10 give -9. */
    fresh(&r, 700.0);
    add(&r, -10.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -12.0, true, DRY_OK);
    add(&r, -6.0, true, DRY_OK);
    add(&r, -7.0, false, DRY_OK);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_OK && near(o.rate_per_h, -9.0) && near(o.hours, 15.0));
    CHECK(near(o.hours_fast, 135.0 / 12.0) && near(o.hours_slow, 135.0 / 6.0));

    /* One wild cycle does not move the median. */
    fresh(&r, 700.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -80.0, true, DRY_OK);
    add(&r, -8.0, false, DRY_OK);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_OK && near(o.rate_per_h, -8.0));

    /* The open cycle with a short length is fine; it is not used for the rate. */
    fresh(&r, 700.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -1.0, false, DRY_REJ_SHORT);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_OK && near(o.rate_per_h, -8.0));
}

static void test_withheld(void)
{
    dry_result     r;
    predict_result o;
    char           line[200];
    predict_params p;

    /* Two accepted complete cycles are not enough; rejected ones and the open one do not count. */
    fresh(&r, 700.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_REJ_POOR_FIT);
    add(&r, -8.0, true, DRY_REJ_GAP);
    add(&r, -8.0, false, DRY_OK);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_INSUFFICIENT_CYCLES && o.have_cycles == 2u && o.need_cycles == 3u);
    CHECK(o.hours == 0.0 && o.rate_per_h == 0.0);
    (void)predict_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction_withheld reason=insufficient_cycles have=2 need=3") == 0);

    /* No cycles at all. */
    fresh(&r, 700.0);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_INSUFFICIENT_CYCLES && o.have_cycles == 0u);

    /* Nothing good recorded, or only old data (a sensor that went INVALID or STUCK). */
    (void)memset(&r, 0, sizeof r);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_NO_RECENT_DATA && o.age_ms == 0u);
    fresh(&r, 700.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, false, DRY_OK);
    r.last_ts_ms = NOW - 30u * MIN_MS; /* exactly the limit is still fresh */
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_OK);
    r.last_ts_ms = NOW - 30u * MIN_MS - 1u;
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_NO_RECENT_DATA && o.age_ms == 30u * MIN_MS + 1u);
    (void)predict_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction_withheld reason=no_recent_data have=3 need=3") == 0);
    /* Stale data is reported before missing cycles. */
    r.n_cycles = 0;
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_NO_RECENT_DATA);
    /* A clock behind the newest record is not stale data. */
    fresh(&r, 700.0);
    r.last_ts_ms = NOW + HOUR_MS;
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_INSUFFICIENT_CYCLES && o.age_ms == 0u);

    /* Just watered: the newest cycle is complete, so there is no dry-down in progress. */
    fresh(&r, 900.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_NOT_DRYING);

    /* The current cycle is long enough and is not falling. */
    add(&r, 0.5, false, DRY_REJ_NOT_FALLING);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_NOT_FALLING);
    (void)predict_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction_withheld reason=not_falling have=3 need=3") == 0);

    /* The cycles needed can be changed. */
    r.n_cycles--;
    add(&r, -8.0, false, DRY_OK);
    predict_params_default(&p, 565.0);
    p.min_cycles = 4;
    CHECK(predict_time_until_dry(&r, &p, NOW, &o) == 0);
    CHECK(o.status == PREDICT_INSUFFICIENT_CYCLES && o.have_cycles == 3u && o.need_cycles == 4u);
    p.min_cycles = 2;
    CHECK(predict_time_until_dry(&r, &p, NOW, &o) == 0 && o.status == PREDICT_OK);
}

static void test_already_dry_and_describe(void)
{
    dry_result     r;
    predict_result o;
    char           line[200];
    char           tiny[8];

    fresh(&r, 560.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -8.0, false, DRY_OK);
    o = run(&r, 565.0, NOW);
    CHECK(o.status == PREDICT_OK && o.already_dry && o.hours == 0.0);
    (void)predict_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction hours=0.0 already_dry=1 level=560 dry=565") == 0);
    r.last_raw = 565.0; /* exactly at the threshold is dry */
    o = run(&r, 565.0, NOW);
    CHECK(o.already_dry);
    r.last_raw = 565.5;
    o = run(&r, 565.0, NOW);
    CHECK(!o.already_dry && o.hours > 0.0);

    fresh(&r, 700.0);
    add(&r, -8.0, true, DRY_OK);
    add(&r, -10.0, true, DRY_OK);
    add(&r, -6.0, true, DRY_OK);
    add(&r, -8.0, false, DRY_OK);
    o = run(&r, 565.0, NOW);
    (void)predict_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction hours=16.9 range=13.5-22.5 rate_per_h=-8.00 cycles=3 level=700 dry=565") == 0);

    /* A short buffer is cut and still ends in NUL; the length is what was needed. */
    CHECK(predict_describe(&o, tiny, sizeof tiny) == strlen(line));
    CHECK(strlen(tiny) == sizeof tiny - 1u && strncmp(tiny, line, sizeof tiny - 1u) == 0);
    CHECK(predict_describe(&o, NULL, 0) == strlen(line));
    predict_log(&o); /* must not crash; the line goes to stderr */

    CHECK(strcmp(predict_status_name(PREDICT_OK), "ok") == 0);
    CHECK(strcmp(predict_status_name(PREDICT_NO_RECENT_DATA), "no_recent_data") == 0);
    CHECK(strcmp(predict_status_name(PREDICT_NOT_DRYING), "not_drying") == 0);
}

static void test_bad_params(void)
{
    dry_result     r;
    predict_result o;
    predict_params p;

    fresh(&r, 700.0);
    predict_params_default(&p, 565.0);
    p.dry_raw = NAN;
    errno = 0;
    CHECK(predict_time_until_dry(&r, &p, NOW, &o) == -1 && errno == EINVAL);
    p.dry_raw = -1.0;
    CHECK(predict_time_until_dry(&r, &p, NOW, &o) == -1);
    p.dry_raw = INFINITY;
    CHECK(predict_time_until_dry(&r, &p, NOW, &o) == -1);
    predict_params_default(&p, 565.0);
    p.min_cycles = 0;
    CHECK(predict_time_until_dry(&r, &p, NOW, &o) == -1);
    predict_params_default(&p, 565.0);
    p.fresh_ms = 0;
    CHECK(predict_time_until_dry(&r, &p, NOW, &o) == -1);
}

/* ---- the whole way, from records ------------------------------------------------------ */

static uint64_t g_seed = 987654321u;

static double noise(double amp)
{
    double u;

    g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
    u = (double)(g_seed >> 33) / 2147483648.0;
    return (u * 2.0 - 1.0) * amp;
}

typedef struct sim {
    dry_acc* a;
    uint64_t t;
    uint32_t seq;
} sim;

static void emit(sim* s, double y)
{
    gt_record rec;

    (void)memset(&rec, 0, sizeof rec);
    rec.ts_unix_ms = s->t;
    rec.seq = s->seq++;
    rec.soil_raw = (uint16_t)lround(y + noise(16.0));
    CHECK(dry_acc_visit(&rec, GT_STORE_REC_OK, s->a) == 0);
    s->t += MIN_MS;
}

/* `done` finished 30 hour dry-downs at -8 counts/hour, then watering and one that is `open_h`
 * hours old. */
static void history(sim* s, unsigned done, double open_h)
{
    unsigned k;
    unsigned m;
    double   y = 900.0;

    for (k = 0; k <= done; k++) {
        double hours = (k < done) ? 30.0 : open_h;

        if (k > 0) {
            for (m = 1; m <= 30; m++) {
                emit(s, y + (900.0 - y) * (double)m / 30.0);
            }
        }
        for (m = 0; m < (unsigned)(hours * 60.0); m++) {
            emit(s, 900.0 - 8.0 * (double)m / 60.0);
        }
        y = 900.0 - 8.0 * hours;
    }
}

static void test_from_records(void)
{
    dry_acc        a;
    dry_result     dr;
    predict_result o;
    sim            s;
    double         truth;

    /* Four finished dry-downs and a fifth 10 hours old: level about 820, so (820-565)/8 hours. */
    CHECK(dry_acc_init(&a, NULL) == 0);
    s.a = &a;
    s.t = 1700000400000ull;
    s.seq = 1;
    history(&s, 4, 10.0);
    dry_analyze(&a, &dr);
    o = run(&dr, 565.0, s.t);
    truth = (900.0 - 8.0 * 10.0 - 565.0) / 8.0;
    CHECK(o.status == PREDICT_OK && o.have_cycles == 4u);
    CHECK(fabs(o.hours - truth) < 0.05 * truth);
    CHECK(o.hours_fast <= o.hours && o.hours <= o.hours_slow);
    dry_acc_free(&a);

    /* The same plant after only two finished dry-downs: no prediction yet. */
    CHECK(dry_acc_init(&a, NULL) == 0);
    s.a = &a;
    s.t = 1700000400000ull;
    s.seq = 1;
    history(&s, 2, 10.0);
    dry_analyze(&a, &dr);
    o = run(&dr, 565.0, s.t);
    CHECK(o.status == PREDICT_INSUFFICIENT_CYCLES && o.have_cycles == 2u);
    dry_acc_free(&a);
}

int main(void)
{
    test_estimate_by_hand();
    test_withheld();
    test_already_dry_and_describe();
    test_bad_params();
    test_from_records();
    (void)puts("analysis_predict_test: ok");
    return 0;
}
