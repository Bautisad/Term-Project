#include "dry.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MS_PER_H 3600000.0

void dry_params_default(dry_params* p)
{
    p->bucket_ms = 10ull * 60ull * 1000ull;
    p->rise_counts = 60.0;
    p->settle_counts = 12.0;
    p->min_cycle_ms = 12ull * 3600ull * 1000ull;
    p->max_gap_ms = 6ull * 3600ull * 1000ull;
    p->min_r2 = 0.8;
    p->max_buckets = DRY_MAX_BUCKETS;
    p->dry_raw = 565.0;
}

/* Written so that a NaN fails every comparison. */
static bool params_ok(const dry_params* p)
{
    return p->bucket_ms >= 1000u && p->rise_counts > 0.0 && p->settle_counts > 0.0 &&
           p->settle_counts < p->rise_counts && p->min_cycle_ms > 0u &&
           p->max_gap_ms >= p->bucket_ms && p->min_r2 >= 0.0 && p->min_r2 <= 1.0 &&
           p->max_buckets >= 1u && p->max_buckets <= DRY_MAX_BUCKETS && isfinite(p->dry_raw) &&
           p->dry_raw >= 0.0;
}

int dry_acc_init(dry_acc* a, const dry_params* p)
{
    dry_params use;

    (void)memset(a, 0, sizeof *a);
    if (p != NULL) {
        use = *p;
    } else {
        dry_params_default(&use);
    }
    if (!params_ok(&use)) {
        errno = EINVAL;
        return -1;
    }
    a->p = use;
    return 0;
}

void dry_acc_free(dry_acc* a)
{
    free(a->b);
    a->b = NULL;
    a->n = 0;
    a->cap = 0;
    a->have_cur = false;
}

static int push(dry_acc* a, const dry_bucket* bk)
{
    if (a->n >= a->p.max_buckets) {
        a->overflow = true;
        return -1;
    }
    if (a->n == a->cap) {
        size_t      nc = (a->cap != 0u) ? a->cap * 2u : 256u;
        dry_bucket* nb;

        if (nc > a->p.max_buckets) {
            nc = a->p.max_buckets;
        }
        nb = realloc(a->b, nc * sizeof *nb);
        if (nb == NULL) {
            a->oom = true;
            return -1;
        }
        a->b = nb;
        a->cap = nc;
    }
    a->b[a->n++] = *bk;
    return 0;
}

int dry_acc_visit(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    dry_acc* a = ctx;
    uint64_t id;

    if (a->overflow || a->oom) {
        return 1;
    }
    if (status != GT_STORE_REC_OK) {
        a->skipped_corrupt++;
        return 0;
    }
    a->records++;
    if ((rec->flags & GT_REC_SOIL_INVALID) != 0u) {
        a->skipped_invalid++;
        return 0;
    }
    if ((rec->flags & GT_REC_SOIL_STUCK) != 0u) {
        a->skipped_stuck++;
        return 0;
    }
    if ((rec->flags & GT_REC_TIME_UNSET) != 0u) {
        a->skipped_time_unset++;
        return 0;
    }
    id = rec->ts_unix_ms / a->p.bucket_ms;
    if (a->have_cur) {
        if (id < a->cur.id) {
            a->out_of_order++;
            return 0;
        }
        if (id > a->cur.id) {
            if (push(a, &a->cur) != 0) {
                return 1;
            }
            a->have_cur = false;
            if (a->n >= a->p.max_buckets) { /* the bucket about to start would be one too many */
                a->overflow = true;
                return 1;
            }
        }
    }
    if (!a->have_cur) {
        (void)memset(&a->cur, 0, sizeof a->cur);
        a->cur.id = id;
        a->have_cur = true;
    }
    a->cur.sum += (double)rec->soil_raw;
    a->cur.t_sum += (double)(rec->ts_unix_ms - id * a->p.bucket_ms);
    a->cur.n++;
    if ((rec->flags & GT_REC_LIGHT_INVALID) == 0u) { /* a bad light flag never counts as dark */
        double bright = (double)rec->light_bright_ms;

        a->cur.bright_ms += (bright > DRY_LIGHT_WINDOW_MS) ? DRY_LIGHT_WINDOW_MS : bright;
        a->cur.light_n++;
    }
    a->used++;
    return 0;
}

int dry_acc_error(const dry_acc* a)
{
    if (a->oom) {
        return ENOMEM;
    }
    return a->overflow ? EOVERFLOW : 0;
}

const char* dry_reject_name(dry_reject r)
{
    switch (r) {
    case DRY_OK:
        return "ok";
    case DRY_REJ_GAP:
        return "gap";
    case DRY_REJ_SHORT:
        return "short";
    case DRY_REJ_NOT_FALLING:
        return "not_falling";
    case DRY_REJ_POOR_FIT:
        return "poor_fit";
    }
    return "unknown";
}

typedef struct pt {
    double t; /* ms since the epoch */
    double y; /* averaged soil_raw */
} pt;

static size_t npoints(const dry_acc* a)
{
    return a->n + (a->have_cur ? 1u : 0u);
}

/* Point i of the series: the finished buckets, then the one still being filled. */
static pt point(const dry_acc* a, size_t i)
{
    const dry_bucket* bk = (i < a->n) ? &a->b[i] : &a->cur;
    pt                p;

    p.t = (double)(bk->id * a->p.bucket_ms) + bk->t_sum / (double)bk->n;
    p.y = bk->sum / (double)bk->n;
    return p;
}

/* Bright hours per 24 h over buckets first..last, from the records with a valid light reading
 * (0 if there are none), and the share of those buckets' records that had one. */
static void light_over(const dry_acc* a, size_t first, size_t last, double* h_per_day,
                       double* coverage)
{
    double bright = 0.0;
    double light_n = 0.0;
    double n = 0.0;

    for (size_t i = first; i <= last; i++) {
        const dry_bucket* bk = (i < a->n) ? &a->b[i] : &a->cur;

        bright += bk->bright_ms;
        light_n += (double)bk->light_n;
        n += (double)bk->n;
    }
    *h_per_day = (light_n > 0.0) ? 24.0 * bright / (light_n * DRY_LIGHT_WINDOW_MS) : 0.0;
    *coverage = (n > 0.0) ? light_n / n : 0.0;
}

/* The C3 comparison needs the state DRY_EVAL_AHEAD_H hours before the cycle reaches dry_raw.
 * Leaves the c->cross_ms / have_eval fields at 0 / false when the cycle never got there, was
 * already dry when it began, or has no valid light before that moment. */
static void find_eval(const dry_acc* a, size_t first, size_t last, dry_cycle* c)
{
    double start_t = point(a, first).t;
    size_t cross = first;
    size_t j = first;
    double cross_t;
    double eval_t;
    double h;
    double cov;

    while (cross <= last && point(a, cross).y > a->p.dry_raw) {
        cross++;
    }
    if (cross > last) {
        return;
    }
    cross_t = point(a, cross).t;
    c->cross_ms = (uint64_t)cross_t;
    eval_t = cross_t - DRY_EVAL_AHEAD_H * MS_PER_H;
    if (eval_t < start_t) {
        return;
    }
    while (j < cross && point(a, j + 1u).t <= eval_t) {
        j++;
    }
    if (eval_t - point(a, j).t > (double)a->p.max_gap_ms) {
        return;
    }
    light_over(a, first, j, &h, &cov);
    if (cov <= 0.0) {
        return;
    }
    c->have_eval = true;
    c->level_eval = point(a, j).y;
    c->bright_eval = h;
}

/* Least-squares line through points first..last (x in hours), then the verdict. */
static void fit_cycle(const dry_acc* a, size_t first, size_t last, bool complete, dry_cycle* c)
{
    pt     p0 = point(a, first);
    pt     pl = point(a, last);
    double n = (double)(last - first + 1u);
    double xm = 0.0;
    double ym = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    bool   gap = false;
    size_t i;

    (void)memset(c, 0, sizeof *c);
    c->start_ms = (uint64_t)p0.t;
    c->end_ms = (uint64_t)pl.t;
    c->start_raw = p0.y;
    c->end_raw = pl.y;
    c->buckets = last - first + 1u;
    c->complete = complete;

    for (i = first; i <= last; i++) {
        pt p = point(a, i);

        xm += (p.t - p0.t) / MS_PER_H;
        ym += p.y;
        if (i > first && p.t - point(a, i - 1u).t > (double)a->p.max_gap_ms) {
            gap = true;
        }
    }
    xm /= n;
    ym /= n;
    for (i = first; i <= last; i++) {
        pt     p = point(a, i);
        double dx = (p.t - p0.t) / MS_PER_H - xm;
        double dy = p.y - ym;

        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }
    if (sxx > 0.0) {
        c->slope_per_h = sxy / sxx;
        if (syy > 0.0) {
            c->r2 = (sxy * sxy) / (sxx * syy);
        }
    }

    light_over(a, first, last, &c->bright_h_per_day, &c->light_coverage);
    find_eval(a, first, last, c);

    if (gap) {
        c->reject = DRY_REJ_GAP;
    } else if (pl.t - p0.t < (double)a->p.min_cycle_ms) {
        c->reject = DRY_REJ_SHORT;
    } else if (!(c->slope_per_h < 0.0)) {
        c->reject = DRY_REJ_NOT_FALLING;
    } else if (c->r2 < a->p.min_r2) {
        c->reject = DRY_REJ_POOR_FIT;
    } else {
        c->reject = DRY_OK;
    }
}

static void add_cycle(dry_result* r, const dry_cycle* c)
{
    r->cycles_total++;
    if (r->n_cycles == DRY_MAX_CYCLES) {
        (void)memmove(&r->cycles[0], &r->cycles[1], (DRY_MAX_CYCLES - 1u) * sizeof r->cycles[0]);
        r->n_cycles--;
    }
    r->cycles[r->n_cycles++] = *c;
}

void dry_analyze(const dry_acc* a, dry_result* out)
{
    size_t    n = npoints(a);
    size_t    start = 0;
    size_t    low = 0;
    size_t    hi = 0;
    bool      falling = true;
    size_t    i;
    dry_cycle c;

    (void)memset(out, 0, sizeof *out);
    out->buckets = n;
    out->dry_raw = a->p.dry_raw;
    out->records = a->records;
    out->used = a->used;
    out->skipped_invalid = a->skipped_invalid;
    out->skipped_stuck = a->skipped_stuck;
    out->skipped_time_unset = a->skipped_time_unset;
    out->skipped_corrupt = a->skipped_corrupt;
    out->out_of_order = a->out_of_order;
    if (n == 0u) {
        return;
    }
    out->have_last = true;
    out->last_ts_ms = (uint64_t)point(a, n - 1u).t;
    out->last_raw = point(a, n - 1u).y;

    for (i = 1; i < n; i++) {
        double y = point(a, i).y;

        if (falling) {
            if (y < point(a, low).y) {
                low = i;
            } else if (y - point(a, low).y >= a->p.rise_counts) {
                fit_cycle(a, start, low, true, &c);
                add_cycle(out, &c);
                falling = false;
                hi = i;
            }
        } else if (y > point(a, hi).y) {
            hi = i;
        } else if (point(a, hi).y - y >= a->p.settle_counts) {
            start = hi;
            low = i;
            falling = true;
        }
    }
    if (falling) {
        fit_cycle(a, start, n - 1u, false, &c);
        add_cycle(out, &c);
    }
}
