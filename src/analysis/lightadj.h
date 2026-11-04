/* lightadj.h: time until the soil is dry, adjusted for light (#26), and the comparison with the
 * soil-only estimate of predict.h (DESIGN.md test C3).
 *
 * The idea: a plant in a bright spot may dry faster than one in a dim spot.  For every accepted
 * dry-down cycle we know the drying rate r (counts per hour, dry.h) and the light L (bright
 * hours per day, from the LM393 flag).  A straight line r = a + b*L is fitted by least squares
 * through the accepted complete cycles, and the current rate is read off it at the light the
 * open cycle has had so far.  predict.h's median slope is the soil-only estimate and is left
 * as it is, so the two can be compared.
 *
 * With only a handful of cycles the fit says little, so the adjusted estimate is withheld, with
 * the reason, unless: there are enough cycles; their light actually varies (otherwise the slope
 * b is noise); every cycle has enough valid light data; the open cycle's light lies inside the
 * range the fit has seen (no extrapolating); and the fitted rate is falling.  The soil-only
 * estimate does not depend on any of that.
 *
 * Temperature is not measured, so a link between light and drying speed may also be a link
 * with temperature (DESIGN.md "Narrowed claims").  All the numbers are drafts. */
#ifndef GT_LIGHTADJ_H
#define GT_LIGHTADJ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dry.h"
#include "predict.h"

typedef struct lightadj_params {
    size_t min_cycles;     /* accepted complete cycles needed to fit and predict */
    double min_spread_h;   /* their light must span at least this many bright hours per day */
    double margin_h;       /* the open cycle's light may be this far outside that span */
    double min_coverage;   /* each cycle needs this share of its records to have valid light */
    size_t backtest_train; /* cycles needed to train when one is held out (C3) */
    size_t backtest_need;  /* comparisons needed before the C3 verdict says pass or fail */
    double backtest_frac;  /* C3: the adjusted median error must be under this share of 12 h */
} lightadj_params;

/* Draft: [5] cycles (DESIGN C3 "at least 5 real dry-down cycles"), [2] h/day, [2] h/day, [0.5],
 * [4] (one less than min_cycles, because the cycle being tested is held out), [5], [0.25]. */
void lightadj_params_default(lightadj_params* p);

typedef enum lightadj_status {
    LIGHTADJ_OK = 0,
    LIGHTADJ_NO_RECENT_DATA,      /* as predict.h: no good soil sample, or an old one */
    LIGHTADJ_INSUFFICIENT_CYCLES, /* fewer than min_cycles accepted complete cycles */
    LIGHTADJ_LIGHT_COVERAGE,      /* too little valid light data in a cycle (or the open one) */
    LIGHTADJ_NO_LIGHT_SPREAD,     /* the cycles' light barely differs, so b means nothing */
    LIGHTADJ_NOT_DRYING,          /* as predict.h: no dry-down in progress */
    LIGHTADJ_OUTSIDE_RANGE,       /* the open cycle's light is outside what the fit has seen */
    LIGHTADJ_NOT_FALLING          /* the fitted rate is not falling */
} lightadj_status;

/* "ok", "no_recent_data", "insufficient_cycles", "light_coverage", "no_light_spread",
 * "not_drying", "outside_range", "not_falling". */
const char* lightadj_status_name(lightadj_status s);

typedef struct lightadj_result {
    lightadj_status status;
    size_t          have_cycles, need_cycles; /* usable accepted complete cycles; the number needed */
    double          fit_a, fit_b, fit_r2;     /* r = a + b*L, counts/h and counts/h per bright h/day */
    double          light_now;                /* bright h/day of the open cycle so far */
    double          light_min, light_max;     /* the span of the cycles in the fit */
    double          rate_per_h;               /* the adjusted rate, negative (OK only) */
    double          hours;                    /* until dry; 0 if already dry (OK only) */
    bool            have_range;               /* hours_fast/hours_slow are filled */
    double          hours_fast, hours_slow;   /* rate -/+ the largest fit residual */
    bool            already_dry;
    predict_result  soil;                     /* the soil-only estimate, for side by side */
} lightadj_result;

/* Returns 0 and fills *out (status says whether there is an adjusted estimate; *out->soil is
 * always the predict.h result), or -1 with errno EINVAL for nonsense parameters. */
int lightadj_predict(const dry_result* r, const predict_params* pp, const lightadj_params* lp,
                     uint64_t now_ms, lightadj_result* out);

/* The event part of the log line ("<event> key=value ..."), for example
 *   prediction_light hours=12.5 range=10.0-16.2 rate_per_h=-10.80 light_h=14.0 slope=-0.35 cycles=6 level=700 dry=565
 *   prediction_light_withheld reason=insufficient_cycles have=3 need=5
 * Always NUL-terminated (if cap > 0); returns the length it needed, like snprintf. */
size_t lightadj_describe(const lightadj_result* r, char* buf, size_t cap);

/* C3.  Every accepted complete cycle that reached the dry level, with enough light data
 * before the crossing (dry_cycle.have_eval), is predicted twice at the moment
 * DRY_EVAL_AHEAD_H hours before the crossing, from all the OTHER accepted cycles: soil-only
 * (median slope) and light-adjusted (the fit at that cycle's light so far).  The true answer is
 * DRY_EVAL_AHEAD_H hours by definition, so the error is |predicted - 12|. */
#define LIGHTADJ_MAX_EVAL DRY_MAX_CYCLES

typedef struct lightadj_eval {
    size_t cycle;        /* index into dry_result.cycles */
    double level, light; /* what the device knew at the time: soil value, bright h/day so far */
    double soil_hours;   /* predicted, soil-only */
    bool   have_light;   /* the adjusted estimate exists for this cycle (it can be withheld) */
    double light_hours;  /* predicted, light-adjusted */
} lightadj_eval;

typedef enum lightadj_verdict {
    LIGHTADJ_VERDICT_INSUFFICIENT = 0, /* fewer than backtest_need comparisons */
    LIGHTADJ_VERDICT_PASS,
    LIGHTADJ_VERDICT_FAIL
} lightadj_verdict;

const char* lightadj_verdict_name(lightadj_verdict v); /* "insufficient", "pass", "fail" */

typedef struct lightadj_backtest_result {
    size_t           n_eval;                 /* entries in eval[] */
    lightadj_eval    eval[LIGHTADJ_MAX_EVAL];
    size_t           paired;                 /* entries with both estimates; the verdict uses these */
    size_t           need;                   /* backtest_need */
    double           soil_median_err_h;      /* over the paired entries (0 if none) */
    double           light_median_err_h;
    lightadj_verdict verdict;                /* pass: paired >= need, adjusted error lower than
                                                soil-only and under backtest_frac of 12 h */
} lightadj_backtest_result;

/* Uses r->dry_raw as the dry level.  Returns 0, or -1 with errno EINVAL for nonsense
 * parameters. */
int lightadj_backtest(const dry_result* r, const lightadj_params* lp,
                      lightadj_backtest_result* out);

#endif /* GT_LIGHTADJ_H */
