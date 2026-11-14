/* analysis_light_test: the light-adjusted prediction (#26) and the C3 comparison against the
 * soil-only estimate.  ALL DATA HERE IS SYNTHETIC: hand-built cycle lists, whose expected
 * numbers are worked out by hand in the comments, and one run from generated records through
 * dry.h.  None of it is a measurement.  Exits 1 at the first failed check. */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lightadj.h"

#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) {                                                                      \
            (void)fprintf(stderr, "analysis_light_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

#define NOW     1700000000000ull
#define MIN_MS  60000ull
#define HOUR_MS 3600000ull

static bool near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

static void fresh(dry_result* r, double level)
{
    (void)memset(r, 0, sizeof *r);
    r->dry_raw = 565.0;
    r->have_last = true;
    r->last_ts_ms = NOW - 5u * MIN_MS;
    r->last_raw = level;
}

static dry_cycle* add(dry_result* r, double slope, double light, double coverage, bool complete,
                      dry_reject rej)
{
    dry_cycle* c;

    CHECK(r->n_cycles < DRY_MAX_CYCLES);
    c = &r->cycles[r->n_cycles++];
    (void)memset(c, 0, sizeof *c);
    c->slope_per_h = slope;
    c->bright_h_per_day = light;
    c->light_coverage = coverage;
    c->complete = complete;
    c->reject = rej;
    r->cycles_total++;
    return c;
}

/* A finished, accepted cycle that reached the dry level, for the comparison: at 12 hours before
 * the crossing the soil stood at dry + 12 * `true_rate` (so it really was 12 hours away if it
 * fell at `true_rate`), with `light` bright hours per day so far. */
static void add_eval(dry_result* r, double slope, double light, double true_rate)
{
    dry_cycle* c = add(r, slope, light, 1.0, true, DRY_OK);

    c->cross_ms = NOW;
    c->have_eval = true;
    c->level_eval = r->dry_raw - DRY_EVAL_AHEAD_H * true_rate; /* true_rate is negative */
    c->bright_eval = light;
}

static lightadj_result run(const dry_result* r, uint64_t now)
{
    predict_params  pp;
    lightadj_params lp;
    lightadj_result o;

    predict_params_default(&pp, 565.0);
    lightadj_params_default(&lp);
    CHECK(lightadj_predict(r, &pp, &lp, now, &o) == 0);
    return o;
}

/* Five accepted cycles on (light, rate): (4,-4.5) (8,-5.5) (12,-8) (16,-9.5) (20,-12.5), then
 * an open cycle with `open_light`.  By hand: mean light 12, mean rate -8, Sxy = -80, Sxx = 160,
 * so b = -0.5, a = -8 + 0.5*12 = -2.  Residuals -0.5 +0.5 0 +0.5 -0.5 (largest 0.5).
 * Syy = 41, so r2 = 80^2 / (160*41) = 0.97561. */
static void five(dry_result* r, double open_light, double level)
{
    fresh(r, level);
    (void)add(r, -4.5, 4.0, 1.0, true, DRY_OK);
    (void)add(r, -5.5, 8.0, 1.0, true, DRY_OK);
    (void)add(r, -8.0, 12.0, 1.0, true, DRY_OK);
    (void)add(r, -9.5, 16.0, 1.0, true, DRY_OK);
    (void)add(r, -12.5, 20.0, 1.0, true, DRY_OK);
    (void)add(r, -7.0, open_light, 1.0, false, DRY_OK);
}

static void test_estimate_by_hand(void)
{
    dry_result      r;
    lightadj_result o;
    char            line[256];

    /* Open cycle at 10 bright h/day: rate -2 - 0.5*10 = -7.  Level 700, dry 565: gap 135.
     * Adjusted 135/7 = 19.2857 h; range from rate -7.5 and -6.5: 18 h and 20.7692 h.
     * Soil-only: the median of the five slopes is -8, so 135/8 = 16.875 h. */
    five(&r, 10.0, 700.0);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_OK && !o.already_dry && o.have_range);
    CHECK(o.have_cycles == 5u && o.need_cycles == 5u);
    CHECK(near(o.fit_a, -2.0, 1e-9) && near(o.fit_b, -0.5, 1e-9));
    CHECK(near(o.fit_r2, 6400.0 / 6560.0, 1e-9));
    CHECK(near(o.light_min, 4.0, 1e-12) && near(o.light_max, 20.0, 1e-12) && near(o.light_now, 10.0, 1e-12));
    CHECK(near(o.rate_per_h, -7.0, 1e-9) && near(o.hours, 135.0 / 7.0, 1e-9));
    CHECK(near(o.hours_fast, 18.0, 1e-9) && near(o.hours_slow, 135.0 / 6.5, 1e-9));
    CHECK(o.soil.status == PREDICT_OK && near(o.soil.hours, 16.875, 1e-9));
    (void)lightadj_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction_light hours=19.3 range=18.0-20.8 rate_per_h=-7.00 light_h=10.0 "
                       "slope=-0.500 cycles=5 level=700 dry=565") == 0);

    /* More light, faster drying: 20 bright h/day gives -12, so 135/12 = 11.25 h. */
    five(&r, 20.0, 700.0);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_OK && near(o.hours, 11.25, 1e-9) && near(o.soil.hours, 16.875, 1e-9));

    /* Exactly on the edge of the allowed span (4 - margin 2 = 2) is still inside. */
    five(&r, 2.0, 700.0);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_OK && near(o.rate_per_h, -3.0, 1e-9));

    /* Already dry: an estimate of 0 hours, same as soil-only. */
    five(&r, 10.0, 560.0);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_OK && o.already_dry && o.hours == 0.0);
    (void)lightadj_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction_light hours=0.0 already_dry=1 level=560 dry=565") == 0);

    /* Rates that the line does not explain: the spread of the residuals is as large as the rate,
     * so there is no honest range, and the estimate says so.  Rates -1 -10 -1 -10 -1 give b = 0,
     * a = -4.6, a largest residual of 5.4, and -4.6 + 5.4 is not below zero. */
    fresh(&r, 700.0);
    (void)add(&r, -1.0, 4.0, 1.0, true, DRY_OK);
    (void)add(&r, -10.0, 8.0, 1.0, true, DRY_OK);
    (void)add(&r, -1.0, 12.0, 1.0, true, DRY_OK);
    (void)add(&r, -10.0, 16.0, 1.0, true, DRY_OK);
    (void)add(&r, -1.0, 20.0, 1.0, true, DRY_OK);
    (void)add(&r, -5.0, 10.0, 1.0, false, DRY_OK);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_OK && !o.have_range && near(o.rate_per_h, -4.6, 1e-9));
    (void)lightadj_describe(&o, line, sizeof line);
    CHECK(strstr(line, " range=none ") != NULL);
}

static void test_withheld(void)
{
    dry_result      r;
    lightadj_result o;
    char            line[256];
    size_t          i;

    /* Four cycles: soil-only (3 needed) still answers, the adjusted estimate does not. */
    fresh(&r, 700.0);
    (void)add(&r, -4.5, 4.0, 1.0, true, DRY_OK);
    (void)add(&r, -5.5, 8.0, 1.0, true, DRY_OK);
    (void)add(&r, -8.0, 12.0, 1.0, true, DRY_OK);
    (void)add(&r, -9.5, 16.0, 1.0, true, DRY_OK);
    (void)add(&r, -7.0, 10.0, 1.0, false, DRY_OK);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_INSUFFICIENT_CYCLES && o.have_cycles == 4u && o.need_cycles == 5u);
    CHECK(o.soil.status == PREDICT_OK);
    (void)lightadj_describe(&o, line, sizeof line);
    CHECK(strcmp(line, "prediction_light_withheld reason=insufficient_cycles have=4 need=5") == 0);

    /* Rejected and incomplete cycles do not count towards the five. */
    five(&r, 10.0, 700.0);
    r.cycles[0].reject = DRY_REJ_POOR_FIT;
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_INSUFFICIENT_CYCLES && o.have_cycles == 4u);

    /* The light never changes: the slope would be noise. */
    five(&r, 10.0, 700.0);
    for (i = 0; i < 5; i++) {
        r.cycles[i].bright_h_per_day = 10.0;
    }
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_NO_LIGHT_SPREAD);
    r.cycles[0].bright_h_per_day = 9.2; /* a spread of 1.6 h/day is still under 2 */
    r.cycles[2].bright_h_per_day = 10.8;
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_NO_LIGHT_SPREAD);

    /* A cycle with a bad light module has too little valid light: it cannot be used. */
    five(&r, 10.0, 700.0);
    r.cycles[2].light_coverage = 0.2;
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_LIGHT_COVERAGE && o.have_cycles == 4u);
    five(&r, 10.0, 700.0);
    for (i = 0; i < 5; i++) {
        r.cycles[i].light_coverage = 0.0; /* every record LIGHT_INVALID */
    }
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_LIGHT_COVERAGE && o.have_cycles == 0u);

    /* The current cycle's own light is missing. */
    five(&r, 10.0, 700.0);
    r.cycles[5].light_coverage = 0.2;
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_LIGHT_COVERAGE);

    /* The current light is outside what the fit has seen (4..20, margin 2): no extrapolating. */
    five(&r, 22.5, 700.0);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_OUTSIDE_RANGE && near(o.light_now, 22.5, 1e-12));
    five(&r, 1.5, 700.0);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_OUTSIDE_RANGE);

    /* Rates -4 -3 -2 -1 -0.1 at 4 8 12 16 20 h/day: the line rises with light.  By hand: mean light 12,
     * mean rate -2.02, Sxy = 39.2, Sxx = 160, so b = 0.245 and a = -2.02 - 0.245*12 = -4.96.  At 22 h/day
     * (inside the span plus margin) that is -4.96 + 5.39 = +0.43 counts per hour: not falling. */
    fresh(&r, 700.0);
    (void)add(&r, -4.0, 4.0, 1.0, true, DRY_OK);
    (void)add(&r, -3.0, 8.0, 1.0, true, DRY_OK);
    (void)add(&r, -2.0, 12.0, 1.0, true, DRY_OK);
    (void)add(&r, -1.0, 16.0, 1.0, true, DRY_OK);
    (void)add(&r, -0.1, 20.0, 1.0, true, DRY_OK);
    (void)add(&r, -0.5, 22.0, 1.0, false, DRY_OK);
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_NOT_FALLING && near(o.fit_b, 0.245, 1e-9) && near(o.fit_a, -4.96, 1e-9));
    CHECK(o.soil.status == PREDICT_OK);

    /* The soil-only reasons carry over: an old sample, and no dry-down in progress. */
    five(&r, 10.0, 700.0);
    o = run(&r, NOW + 2u * HOUR_MS);
    CHECK(o.status == LIGHTADJ_NO_RECENT_DATA);
    five(&r, 10.0, 700.0);
    r.n_cycles--; /* the open cycle is gone: just watered */
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_NOT_DRYING);
    five(&r, 10.0, 700.0);
    r.cycles[5].reject = DRY_REJ_NOT_FALLING; /* the open cycle is not falling */
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_NOT_FALLING);

    /* Nothing at all. */
    fresh(&r, 700.0);
    r.have_last = false;
    o = run(&r, NOW);
    CHECK(o.status == LIGHTADJ_NO_RECENT_DATA && o.have_cycles == 0u);
}

static void test_bad_parameters(void)
{
    dry_result      r;
    predict_params  pp;
    lightadj_params lp;
    lightadj_result o;
    lightadj_backtest_result b;

    five(&r, 10.0, 700.0);
    predict_params_default(&pp, 565.0);
    lightadj_params_default(&lp);

    lp.min_cycles = 1;
    errno = 0;
    CHECK(lightadj_predict(&r, &pp, &lp, NOW, &o) == -1 && errno == EINVAL);
    CHECK(lightadj_backtest(&r, &lp, &b) == -1 && errno == EINVAL);
    lightadj_params_default(&lp);
    lp.min_spread_h = NAN;
    errno = 0;
    CHECK(lightadj_predict(&r, &pp, &lp, NOW, &o) == -1 && errno == EINVAL);
    lightadj_params_default(&lp);
    lp.min_spread_h = 0.0;
    CHECK(lightadj_predict(&r, &pp, &lp, NOW, &o) == -1);
    lightadj_params_default(&lp);
    lp.margin_h = -1.0;
    CHECK(lightadj_predict(&r, &pp, &lp, NOW, &o) == -1);
    lightadj_params_default(&lp);
    lp.min_coverage = 1.5;
    CHECK(lightadj_predict(&r, &pp, &lp, NOW, &o) == -1);
    lightadj_params_default(&lp);
    lp.backtest_frac = 0.0;
    CHECK(lightadj_backtest(&r, &lp, &b) == -1 && errno == EINVAL);
    lightadj_params_default(&lp);

    pp.dry_raw = NAN; /* predict.h refuses it, and so do we */
    errno = 0;
    CHECK(lightadj_predict(&r, &pp, &lp, NOW, &o) == -1 && errno == EINVAL);

    CHECK(strcmp(lightadj_status_name(LIGHTADJ_OK), "ok") == 0);
    CHECK(strcmp(lightadj_status_name(LIGHTADJ_NO_LIGHT_SPREAD), "no_light_spread") == 0);
    CHECK(strcmp(lightadj_status_name((lightadj_status)99), "unknown") == 0);
    CHECK(strcmp(lightadj_verdict_name(LIGHTADJ_VERDICT_PASS), "pass") == 0);
    CHECK(strcmp(lightadj_verdict_name(LIGHTADJ_VERDICT_INSUFFICIENT), "insufficient") == 0);
}

static lightadj_backtest_result backtest(const dry_result* r)
{
    lightadj_params          lp;
    lightadj_backtest_result b;

    lightadj_params_default(&lp);
    CHECK(lightadj_backtest(r, &lp, &b) == 0);
    return b;
}

/* Seven finished cycles whose rate is exactly -2 - 0.5*L at L = 4 8 12 ... 28, each with the soil
 * standing at dry + 12*|rate| twelve hours before it crosses (so it really is 12 h away).
 * `scale` makes the plant dry more slowly than its light says (1 = as the light says). */
static void seven_on_the_line(dry_result* r, double scale)
{
    fresh(r, 700.0);
    for (int k = 0; k < 7; k++) {
        double light = 4.0 + 4.0 * (double)k;
        double rate = -2.0 - 0.5 * light;

        add_eval(r, rate, light, rate * scale);
    }
}

static void test_backtest(void)
{
    dry_result               r;
    lightadj_backtest_result b;

    /* The light says everything.  The first and last cycles' light is outside what the OTHER six
     * span (8..28 and 4..24, margin 2), so the adjusted estimate is withheld there: 7 compared,
     * 5 paired.  Adjusted: exactly 12 h, error 0.
     * Soil-only is the median of the other six slopes (rates -4 -6 -8 -10 -12 -14 -16):
     *   cycle L=8  (-6):  others' median -11  -> 12*6/11  = 6.545 h, error 5.4545
     *   cycle L=12 (-8):  median -11          -> 12*8/11  = 8.727 h, error 3.2727
     *   cycle L=16 (-10): median -10          -> 12*10/10 = 12 h,    error 0
     *   cycle L=20 (-12): median -9           -> 12*12/9  = 16 h,    error 4
     *   cycle L=24 (-14): median -9           -> 12*14/9  = 18.667,  error 6.6667
     * sorted 0 3.2727 4 5.4545 6.6667: median 4.  So adjusted 0 < soil-only 4, and 0 < 3: pass. */
    seven_on_the_line(&r, 1.0);
    b = backtest(&r);
    CHECK(b.n_eval == 7u && b.paired == 5u && b.need == 5u);
    CHECK(!b.eval[0].have_light && !b.eval[6].have_light);
    CHECK(b.eval[1].have_light && near(b.eval[1].soil_hours, 72.0 / 11.0, 1e-9));
    CHECK(near(b.eval[1].light_hours, 12.0, 1e-9) && near(b.eval[1].level, 565.0 + 72.0, 1e-9));
    CHECK(near(b.eval[3].soil_hours, 12.0, 1e-9) && near(b.eval[4].soil_hours, 16.0, 1e-9));
    CHECK(near(b.soil_median_err_h, 4.0, 1e-9) && near(b.light_median_err_h, 0.0, 1e-9));
    CHECK(b.verdict == LIGHTADJ_VERDICT_PASS);

    /* The plant dries at half the speed its light says: the adjusted estimate is now off by 6 h
     * (it predicts 6 when the truth is 12), which is over 25% of 12 h (3 h): fail. */
    seven_on_the_line(&r, 0.5);
    b = backtest(&r);
    CHECK(b.paired == 5u && near(b.light_median_err_h, 6.0, 1e-9));
    CHECK(b.verdict == LIGHTADJ_VERDICT_FAIL);

    /* Every cycle dries at -8 whatever the light: the line is flat, the adjusted estimate equals
     * the soil-only one (both exact, error 0), and 0 is not lower than 0: fail. */
    fresh(&r, 700.0);
    for (int k = 0; k < 7; k++) {
        add_eval(&r, -8.0, 4.0 + 4.0 * (double)k, -8.0);
    }
    b = backtest(&r);
    CHECK(b.paired == 5u && b.soil_median_err_h == 0.0 && b.light_median_err_h == 0.0);
    CHECK(b.verdict == LIGHTADJ_VERDICT_FAIL);

    /* Six cycles (L = 4..24): the first and last are outside what the others span, so four are
     * paired, not the five the verdict needs. */
    seven_on_the_line(&r, 1.0);
    r.n_cycles--;
    b = backtest(&r);
    CHECK(b.n_eval == 6u && b.paired == 4u && b.verdict == LIGHTADJ_VERDICT_INSUFFICIENT);

    /* A cycle that never reached the dry level, one without light before the crossing, a rejected
     * one and the open one are not compared. */
    seven_on_the_line(&r, 1.0);
    r.cycles[2].have_eval = false;
    r.cycles[3].reject = DRY_REJ_SHORT;
    r.cycles[4].complete = false;
    b = backtest(&r);
    CHECK(b.n_eval == 4u && b.verdict == LIGHTADJ_VERDICT_INSUFFICIENT);

    /* Training needs four other cycles: five cycles can each be compared, four cannot. */
    fresh(&r, 700.0);
    for (int k = 0; k < 5; k++) {
        add_eval(&r, -8.0, 4.0 + 4.0 * (double)k, -8.0);
    }
    b = backtest(&r);
    CHECK(b.n_eval == 5u);
    r.n_cycles = 4;
    b = backtest(&r);
    CHECK(b.n_eval == 0u && b.paired == 0u && b.verdict == LIGHTADJ_VERDICT_INSUFFICIENT);

    /* No cycles. */
    fresh(&r, 700.0);
    b = backtest(&r);
    CHECK(b.n_eval == 0u && b.soil_median_err_h == 0.0 && b.verdict == LIGHTADJ_VERDICT_INSUFFICIENT);
}

/* ---- from generated records, through dry.h -------------------------------------------- */

#define STEP_MS 600000ull /* one record per 10 minutes = one bucket per record */

static uint64_t g_t;
static dry_acc  g_acc;

static void put(double soil, double light_frac, uint16_t flags)
{
    gt_record rec;

    (void)memset(&rec, 0, sizeof rec);
    rec.ts_unix_ms = g_t;
    rec.soil_raw = (uint16_t)lround(soil);
    rec.light_bright_ms = (uint16_t)lround(light_frac * 60000.0);
    rec.flags = flags;
    CHECK(dry_acc_visit(&rec, GT_STORE_REC_OK, &g_acc) == 0);
    g_t += STEP_MS;
}

/* One drying cycle: from 800 at `rate` counts per hour for `hours`, with the light on for
 * `light_h` hours of 24. */
static void cycle(double rate, double hours, double light_h, uint16_t flags)
{
    int steps = (int)(hours * 6.0);

    for (int s = 0; s <= steps; s++) {
        put(800.0 + rate * (double)s / 6.0, light_h / 24.0, flags);
    }
}

static void test_from_records(void)
{
    /* Seven finished cycles drying at -(2 + 0.5*L) where L is the bright hours per day, each run
     * until the soil is well below the dry level (565) and then watered back to 800; and an open
     * cycle at L = 12 (rate -8) that has run 20 hours (800 - 160 = 640). */
    static const double light[7] = {6.0, 10.0, 14.0, 18.0, 22.0, 9.0, 15.0};
    dry_result          r;
    predict_params      pp;
    lightadj_params     lp;
    lightadj_result     o;
    lightadj_backtest_result b;
    uint64_t            now;
    dry_params          dp;

    /* The fixture is built around a dry line of 565, so say so rather than lean on the default. */
    dry_params_default(&dp);
    dp.dry_raw = 565.0;
    CHECK(dry_acc_init(&g_acc, &dp) == 0);
    g_t = NOW;
    for (int k = 0; k < 7; k++) {
        double rate = -(2.0 + 0.5 * light[k]);

        cycle(rate, 260.0 / -rate, light[k], 0);
    }
    cycle(-8.0, 20.0, 12.0, 0);
    now = g_t + 5u * MIN_MS; /* the last record is 15 minutes old */
    dry_analyze(&g_acc, &r);
    dry_acc_free(&g_acc);

    CHECK(r.dry_raw == 565.0 && r.n_cycles == 8u);
    for (int k = 0; k < 7; k++) {
        const dry_cycle* c = &r.cycles[k];
        double           rate = -(2.0 + 0.5 * light[k]);

        CHECK(c->complete && c->reject == DRY_OK);
        CHECK(near(c->slope_per_h, rate, 0.1));
        CHECK(near(c->bright_h_per_day, light[k], 0.01) && near(c->light_coverage, 1.0, 1e-12));
        /* It crosses 565 about (800-565)/|rate| hours in, and twelve hours earlier the soil stood
         * about 12*|rate| above it. */
        CHECK(c->cross_ms != 0u && c->have_eval);
        CHECK(near(c->level_eval, 565.0 + 12.0 * -rate, 0.2 * -rate + 3.0));
        CHECK(near(c->bright_eval, light[k], 0.01));
    }
    CHECK(!r.cycles[7].complete && near(r.cycles[7].bright_h_per_day, 12.0, 0.01));

    predict_params_default(&pp, 565.0);
    lightadj_params_default(&lp);
    CHECK(lightadj_predict(&r, &pp, &lp, now, &o) == 0);
    CHECK(o.status == LIGHTADJ_OK && o.have_cycles == 7u);
    CHECK(near(o.fit_a, -2.0, 0.2) && near(o.fit_b, -0.5, 0.02));
    CHECK(near(o.rate_per_h, -8.0, 0.2));
    /* From the level it ended at (about 640): 75/8 = 9.4 h adjusted, and soil-only uses the median of
     * the seven slopes (-9.5 .. -5: sorted -13 -11 -9.5 -9 -7 -6.5 -5), -9: 75/9 = 8.3 h. */
    CHECK(near(o.hours, (o.soil.level_raw - 565.0) / -o.rate_per_h, 1e-9));
    CHECK(near(o.hours, 75.0 / 8.0, 0.8));
    CHECK(near(o.soil.rate_per_h, -9.0, 0.2));

    /* The comparison on the same history: the cycles at L=6 and L=22 are outside what the other
     * six span (9..22 and 6..18, margin 2), so 5 are paired.  Soil-only errors work out by hand
     * to 1.09 2.25 2.92 3.57 4.5 (median 2.92); the adjusted ones are only the rounding of the
     * soil values, well under an hour. */
    CHECK(lightadj_backtest(&r, &lp, &b) == 0);
    CHECK(b.n_eval == 7u && b.paired == 5u);
    CHECK(!b.eval[0].have_light && !b.eval[4].have_light);
    CHECK(near(b.soil_median_err_h, 2.92, 0.6));
    CHECK(b.light_median_err_h < 1.0 && b.light_median_err_h < b.soil_median_err_h);
    CHECK(b.verdict == LIGHTADJ_VERDICT_PASS);
}

/* Records with a bad light flag count as unknown, not as dark. */
static void test_invalid_light(void)
{
    dry_result r;

    CHECK(dry_acc_init(&g_acc, NULL) == 0);
    g_t = NOW;
    /* Cycle A: the light module has failed for the first half, and its garbage says "always bright"
     * (60000 ms).  Only the second half is valid, and says 6 h/day. */
    for (int s = 0; s <= 120; s++) {
        bool bad = s < 60;

        put(800.0 - 4.0 * (double)s / 6.0, bad ? 1.0 : 0.25, bad ? GT_REC_LIGHT_INVALID : 0u);
    }
    /* Watering, then a second cycle with no valid light at all. */
    for (int s = 0; s <= 120; s++) {
        put(800.0 - 4.0 * (double)s / 6.0, 1.0, GT_REC_LIGHT_INVALID);
    }
    put(900.0, 0.0, 0);
    dry_analyze(&g_acc, &r);
    dry_acc_free(&g_acc);

    CHECK(r.n_cycles >= 2u);
    CHECK(near(r.cycles[0].bright_h_per_day, 6.0, 0.01)); /* the garbage "always bright" is ignored */
    CHECK(near(r.cycles[0].light_coverage, 61.0 / 121.0, 1e-9));
    CHECK(r.cycles[1].bright_h_per_day == 0.0 && r.cycles[1].light_coverage == 0.0);
    CHECK(!r.cycles[1].have_eval); /* never crossed, and there is no light either */

    /* A light value above one cycle's length is clamped, not trusted. */
    CHECK(dry_acc_init(&g_acc, NULL) == 0);
    g_t = NOW;
    for (int s = 0; s <= 120; s++) {
        gt_record rec;

        (void)memset(&rec, 0, sizeof rec);
        rec.ts_unix_ms = g_t;
        rec.soil_raw = (uint16_t)lround(800.0 - 4.0 * (double)s / 6.0);
        rec.light_bright_ms = 65535u;
        CHECK(dry_acc_visit(&rec, GT_STORE_REC_OK, &g_acc) == 0);
        g_t += STEP_MS;
    }
    dry_analyze(&g_acc, &r);
    dry_acc_free(&g_acc);
    CHECK(r.n_cycles == 1u && near(r.cycles[0].bright_h_per_day, 24.0, 1e-9));

    /* dry_params.dry_raw is checked like the others. */
    {
        dry_params p;
        dry_acc    a;

        dry_params_default(&p);
        CHECK(p.dry_raw == 450.0);
        p.dry_raw = NAN;
        errno = 0;
        CHECK(dry_acc_init(&a, &p) == -1 && errno == EINVAL);
        dry_acc_free(&a);
        p.dry_raw = -1.0;
        CHECK(dry_acc_init(&a, &p) == -1);
        dry_acc_free(&a);
    }
}

int main(void)
{
    test_estimate_by_hand();
    test_withheld();
    test_bad_parameters();
    test_backtest();
    test_from_records();
    test_invalid_light();
    (void)puts("analysis_light_test: ok");
    return 0;
}
