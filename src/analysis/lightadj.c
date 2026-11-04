#include "lightadj.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void lightadj_params_default(lightadj_params* p)
{
    p->min_cycles = 5;
    p->min_spread_h = 2.0;
    p->margin_h = 2.0;
    p->min_coverage = 0.5;
    p->backtest_train = 4;
    p->backtest_need = 5;
    p->backtest_frac = 0.25;
}

const char* lightadj_status_name(lightadj_status s)
{
    switch (s) {
    case LIGHTADJ_OK:
        return "ok";
    case LIGHTADJ_NO_RECENT_DATA:
        return "no_recent_data";
    case LIGHTADJ_INSUFFICIENT_CYCLES:
        return "insufficient_cycles";
    case LIGHTADJ_LIGHT_COVERAGE:
        return "light_coverage";
    case LIGHTADJ_NO_LIGHT_SPREAD:
        return "no_light_spread";
    case LIGHTADJ_NOT_DRYING:
        return "not_drying";
    case LIGHTADJ_OUTSIDE_RANGE:
        return "outside_range";
    case LIGHTADJ_NOT_FALLING:
        return "not_falling";
    }
    return "unknown";
}

const char* lightadj_verdict_name(lightadj_verdict v)
{
    switch (v) {
    case LIGHTADJ_VERDICT_INSUFFICIENT:
        return "insufficient";
    case LIGHTADJ_VERDICT_PASS:
        return "pass";
    case LIGHTADJ_VERDICT_FAIL:
        return "fail";
    }
    return "unknown";
}

/* Written so that a NaN fails every comparison. */
static bool params_ok(const lightadj_params* p)
{
    return p->min_cycles >= 2u && p->min_spread_h > 0.0 && isfinite(p->min_spread_h) &&
           p->margin_h >= 0.0 && isfinite(p->margin_h) && p->min_coverage >= 0.0 &&
           p->min_coverage <= 1.0 && p->backtest_train >= 2u && p->backtest_need >= 1u &&
           p->backtest_frac > 0.0 && isfinite(p->backtest_frac);
}

/* Sorts v[0..n) upward; n is at most DRY_MAX_CYCLES (same as predict.c). */
static void sort_up(double* v, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        double x = v[i];
        size_t j = i;

        while (j > 0u && v[j - 1u] > x) {
            v[j] = v[j - 1u];
            j--;
        }
        v[j] = x;
    }
}

static double median_sorted(const double* v, size_t n)
{
    return (n % 2u == 1u) ? v[n / 2u] : (v[n / 2u - 1u] + v[n / 2u]) / 2.0;
}

/* The accepted complete cycles, except `skip` (SIZE_MAX for none).  all_rate gets every one's
 * slope; light[] and rate[] get the ones with enough valid light, as pairs. */
typedef struct training {
    double all_rate[DRY_MAX_CYCLES];
    double light[DRY_MAX_CYCLES];
    double rate[DRY_MAX_CYCLES];
    size_t n_acc;
    size_t n_cov;
} training;

static void collect(const dry_result* r, double min_coverage, size_t skip, training* t)
{
    t->n_acc = 0;
    t->n_cov = 0;
    for (size_t i = 0; i < r->n_cycles; i++) {
        const dry_cycle* c = &r->cycles[i];

        if (i == skip || !c->complete || c->reject != DRY_OK) {
            continue;
        }
        t->all_rate[t->n_acc++] = c->slope_per_h;
        if (c->light_coverage >= min_coverage && c->light_coverage > 0.0) {
            t->light[t->n_cov] = c->bright_h_per_day;
            t->rate[t->n_cov] = c->slope_per_h;
            t->n_cov++;
        }
    }
}

typedef struct fit {
    double a, b, r2;
    double lmin, lmax;
    double max_resid;
} fit;

/* Least squares r = a + b*L through n pairs.  False if the light spans less than min_spread
 * (then b is not meaningful). */
static bool fit_line(const double* l, const double* rate, size_t n, double min_spread, fit* f)
{
    double lm = 0.0;
    double rm = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;

    (void)memset(f, 0, sizeof *f);
    f->lmin = l[0];
    f->lmax = l[0];
    for (size_t i = 0; i < n; i++) {
        lm += l[i];
        rm += rate[i];
        f->lmin = (l[i] < f->lmin) ? l[i] : f->lmin;
        f->lmax = (l[i] > f->lmax) ? l[i] : f->lmax;
    }
    if (f->lmax - f->lmin < min_spread) {
        return false;
    }
    lm /= (double)n;
    rm /= (double)n;
    for (size_t i = 0; i < n; i++) {
        double dx = l[i] - lm;
        double dy = rate[i] - rm;

        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }
    f->b = sxy / sxx; /* sxx > 0: the span is at least min_spread > 0 */
    f->a = rm - f->b * lm;
    f->r2 = (syy > 0.0) ? (sxy * sxy) / (sxx * syy) : 0.0;
    for (size_t i = 0; i < n; i++) {
        double e = fabs(rate[i] - (f->a + f->b * l[i]));

        f->max_resid = (e > f->max_resid) ? e : f->max_resid;
    }
    return true;
}

int lightadj_predict(const dry_result* r, const predict_params* pp, const lightadj_params* lp,
                     uint64_t now_ms, lightadj_result* out)
{
    training         t;
    fit              f;
    const dry_cycle* open;
    size_t           need;
    double           rate;
    double           gap;

    if (!params_ok(lp)) {
        errno = EINVAL;
        return -1;
    }
    (void)memset(out, 0, sizeof *out);
    if (predict_time_until_dry(r, pp, now_ms, &out->soil) != 0) {
        return -1; /* errno is EINVAL */
    }
    need = (lp->min_cycles > pp->min_cycles) ? lp->min_cycles : pp->min_cycles;
    out->need_cycles = need;
    collect(r, lp->min_coverage, SIZE_MAX, &t);
    out->have_cycles = t.n_acc;

    if (out->soil.status == PREDICT_NO_RECENT_DATA) {
        out->status = LIGHTADJ_NO_RECENT_DATA;
        return 0;
    }
    if (t.n_acc < need) {
        out->status = LIGHTADJ_INSUFFICIENT_CYCLES;
        return 0;
    }
    if (out->soil.status == PREDICT_NOT_DRYING) {
        out->status = LIGHTADJ_NOT_DRYING;
        return 0;
    }
    if (out->soil.status == PREDICT_NOT_FALLING) {
        out->status = LIGHTADJ_NOT_FALLING;
        return 0;
    }
    out->have_cycles = t.n_cov;
    if (t.n_cov < need) {
        out->status = LIGHTADJ_LIGHT_COVERAGE;
        return 0;
    }
    if (!fit_line(t.light, t.rate, t.n_cov, lp->min_spread_h, &f)) {
        out->status = LIGHTADJ_NO_LIGHT_SPREAD;
        return 0;
    }
    out->fit_a = f.a;
    out->fit_b = f.b;
    out->fit_r2 = f.r2;
    out->light_min = f.lmin;
    out->light_max = f.lmax;

    open = &r->cycles[r->n_cycles - 1u]; /* soil.status is OK, so the open cycle exists */
    if (open->light_coverage < lp->min_coverage || open->light_coverage <= 0.0) {
        out->status = LIGHTADJ_LIGHT_COVERAGE;
        return 0;
    }
    out->light_now = open->bright_h_per_day;
    if (out->light_now < f.lmin - lp->margin_h || out->light_now > f.lmax + lp->margin_h) {
        out->status = LIGHTADJ_OUTSIDE_RANGE;
        return 0;
    }
    rate = f.a + f.b * out->light_now;
    if (!(rate < 0.0)) {
        out->status = LIGHTADJ_NOT_FALLING;
        return 0;
    }

    out->status = LIGHTADJ_OK;
    out->rate_per_h = rate;
    if (out->soil.already_dry) {
        out->already_dry = true;
        return 0; /* hours stay 0 */
    }
    gap = out->soil.level_raw - pp->dry_raw;
    out->hours = gap / -rate;
    if (rate + f.max_resid < 0.0) { /* even the slowest rate in the fit's spread still falls */
        out->have_range = true;
        out->hours_fast = gap / -(rate - f.max_resid);
        out->hours_slow = gap / -(rate + f.max_resid);
    }
    return 0;
}

size_t lightadj_describe(const lightadj_result* r, char* buf, size_t cap)
{
    int n;

    if (r->status != LIGHTADJ_OK) {
        n = snprintf(buf, cap, "prediction_light_withheld reason=%s have=%zu need=%zu",
                     lightadj_status_name(r->status), r->have_cycles, r->need_cycles);
    } else if (r->already_dry) {
        n = snprintf(buf, cap, "prediction_light hours=0.0 already_dry=1 level=%.0f dry=%.0f",
                     r->soil.level_raw, r->soil.dry_raw);
    } else if (r->have_range) {
        n = snprintf(buf, cap,
                     "prediction_light hours=%.1f range=%.1f-%.1f rate_per_h=%.2f light_h=%.1f slope=%.3f "
                     "cycles=%zu level=%.0f dry=%.0f",
                     r->hours, r->hours_fast, r->hours_slow, r->rate_per_h, r->light_now, r->fit_b,
                     r->have_cycles, r->soil.level_raw, r->soil.dry_raw);
    } else {
        n = snprintf(buf, cap,
                     "prediction_light hours=%.1f range=none rate_per_h=%.2f light_h=%.1f slope=%.3f "
                     "cycles=%zu level=%.0f dry=%.0f",
                     r->hours, r->rate_per_h, r->light_now, r->fit_b, r->have_cycles,
                     r->soil.level_raw, r->soil.dry_raw);
    }
    return (n < 0) ? 0u : (size_t)n;
}

int lightadj_backtest(const dry_result* r, const lightadj_params* lp, lightadj_backtest_result* out)
{
    double soil_err[LIGHTADJ_MAX_EVAL];
    double light_err[LIGHTADJ_MAX_EVAL];

    if (!params_ok(lp)) {
        errno = EINVAL;
        return -1;
    }
    (void)memset(out, 0, sizeof *out);
    out->need = lp->backtest_need;

    for (size_t i = 0; i < r->n_cycles; i++) {
        const dry_cycle* c = &r->cycles[i];
        training         t;
        fit              f;
        lightadj_eval*   e;
        double           gap;
        double           soil_rate;
        double           rate;

        if (!c->complete || c->reject != DRY_OK || !c->have_eval ||
            c->light_coverage < lp->min_coverage) {
            continue;
        }
        collect(r, lp->min_coverage, i, &t);
        if (t.n_acc < lp->backtest_train) {
            continue;
        }
        e = &out->eval[out->n_eval++];
        e->cycle = i;
        e->level = c->level_eval;
        e->light = c->bright_eval;
        gap = c->level_eval - r->dry_raw;

        sort_up(t.all_rate, t.n_acc);
        soil_rate = median_sorted(t.all_rate, t.n_acc); /* accepted slopes are all negative */
        e->soil_hours = gap / -soil_rate;

        if (t.n_cov >= lp->backtest_train && fit_line(t.light, t.rate, t.n_cov, lp->min_spread_h, &f) &&
            e->light >= f.lmin - lp->margin_h && e->light <= f.lmax + lp->margin_h) {
            rate = f.a + f.b * e->light;
            if (rate < 0.0) {
                e->have_light = true;
                e->light_hours = gap / -rate;
                soil_err[out->paired] = fabs(e->soil_hours - DRY_EVAL_AHEAD_H);
                light_err[out->paired] = fabs(e->light_hours - DRY_EVAL_AHEAD_H);
                out->paired++;
            }
        }
    }

    if (out->paired > 0u) {
        sort_up(soil_err, out->paired);
        sort_up(light_err, out->paired);
        out->soil_median_err_h = median_sorted(soil_err, out->paired);
        out->light_median_err_h = median_sorted(light_err, out->paired);
    }
    if (out->paired < out->need) {
        out->verdict = LIGHTADJ_VERDICT_INSUFFICIENT;
    } else if (out->light_median_err_h < out->soil_median_err_h &&
               out->light_median_err_h < lp->backtest_frac * DRY_EVAL_AHEAD_H) {
        out->verdict = LIGHTADJ_VERDICT_PASS;
    } else {
        out->verdict = LIGHTADJ_VERDICT_FAIL;
    }
    return 0;
}
