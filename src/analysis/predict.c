#include "predict.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void predict_params_default(predict_params* p, double dry_raw)
{
    p->dry_raw = dry_raw;
    p->min_cycles = 3;
    p->fresh_ms = 30ull * 60ull * 1000ull;
}

const char* predict_status_name(predict_status s)
{
    switch (s) {
    case PREDICT_OK:
        return "ok";
    case PREDICT_NO_RECENT_DATA:
        return "no_recent_data";
    case PREDICT_INSUFFICIENT_CYCLES:
        return "insufficient_cycles";
    case PREDICT_NOT_DRYING:
        return "not_drying";
    case PREDICT_NOT_FALLING:
        return "not_falling";
    }
    return "unknown";
}

static bool params_ok(const predict_params* p)
{
    return isfinite(p->dry_raw) && p->dry_raw >= 0.0 && p->min_cycles > 0u && p->fresh_ms > 0u;
}

/* Sorts v[0..n) upward; n is at most DRY_MAX_CYCLES. */
static void sort_up(double* v, size_t n)
{
    size_t i;

    for (i = 1; i < n; i++) {
        double x = v[i];
        size_t j = i;

        while (j > 0u && v[j - 1u] > x) {
            v[j] = v[j - 1u];
            j--;
        }
        v[j] = x;
    }
}

int predict_time_until_dry(const dry_result* r, const predict_params* p, uint64_t now_ms,
                           predict_result* out)
{
    double           slopes[DRY_MAX_CYCLES];
    size_t           n = 0;
    size_t           i;
    const dry_cycle* open = NULL;
    double           gap_to_dry;

    if (!params_ok(p)) {
        errno = EINVAL;
        return -1;
    }
    (void)memset(out, 0, sizeof *out);
    out->need_cycles = p->min_cycles;
    out->dry_raw = p->dry_raw;

    for (i = 0; i < r->n_cycles; i++) {
        const dry_cycle* c = &r->cycles[i];

        if (c->complete && c->reject == DRY_OK) {
            slopes[n++] = c->slope_per_h;
        }
    }
    out->have_cycles = n;

    if (r->have_last) {
        out->level_raw = r->last_raw;
        out->age_ms = (now_ms > r->last_ts_ms) ? now_ms - r->last_ts_ms : 0u; /* a clock behind the data is not stale */
    }
    if (!r->have_last || out->age_ms > p->fresh_ms) {
        out->status = PREDICT_NO_RECENT_DATA;
        return 0;
    }
    if (n < p->min_cycles) {
        out->status = PREDICT_INSUFFICIENT_CYCLES;
        return 0;
    }
    if (r->n_cycles > 0u && !r->cycles[r->n_cycles - 1u].complete) {
        open = &r->cycles[r->n_cycles - 1u];
    }
    if (open == NULL) {
        out->status = PREDICT_NOT_DRYING;
        return 0;
    }
    if (open->reject == DRY_REJ_NOT_FALLING) {
        out->status = PREDICT_NOT_FALLING;
        return 0;
    }

    out->status = PREDICT_OK;
    if (out->level_raw <= p->dry_raw) {
        out->already_dry = true;
        return 0; /* hours stay 0 */
    }
    sort_up(slopes, n); /* most negative (fastest) first */
    out->rate_per_h = (n % 2u == 1u) ? slopes[n / 2u] : (slopes[n / 2u - 1u] + slopes[n / 2u]) / 2.0;
    gap_to_dry = out->level_raw - p->dry_raw;
    out->hours = gap_to_dry / -out->rate_per_h;
    out->hours_fast = gap_to_dry / -slopes[0];
    out->hours_slow = gap_to_dry / -slopes[n - 1u];
    return 0;
}

size_t predict_describe(const predict_result* r, char* buf, size_t cap)
{
    int n;

    if (r->status != PREDICT_OK) {
        n = snprintf(buf, cap, "prediction_withheld reason=%s have=%zu need=%zu",
                     predict_status_name(r->status), r->have_cycles, r->need_cycles);
    } else if (r->already_dry) {
        n = snprintf(buf, cap, "prediction hours=0.0 already_dry=1 level=%.0f dry=%.0f", r->level_raw,
                     r->dry_raw);
    } else {
        n = snprintf(buf, cap, "prediction hours=%.1f range=%.1f-%.1f rate_per_h=%.2f cycles=%zu level=%.0f dry=%.0f",
                     r->hours, r->hours_fast, r->hours_slow, r->rate_per_h, r->have_cycles, r->level_raw,
                     r->dry_raw);
    }
    return (n < 0) ? 0u : (size_t)n;
}

void predict_log(const predict_result* r)
{
    int       saved = errno;
    char      ts[32];
    char      line[192];
    time_t    t = time(NULL);
    struct tm tm;

    if (gmtime_r(&t, &tm) == NULL || strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)strcpy(ts, "-");
    }
    (void)predict_describe(r, line, sizeof line);
    (void)fprintf(stderr, "%s INFO analysis %s\n", ts, line);
    errno = saved;
}
