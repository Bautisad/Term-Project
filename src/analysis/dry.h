/* dry.h: finds the dry-down cycles in the soil history (#27 step 1).  The soil sensor reads
 * HIGHER when the soil is wetter, so watering shows as a jump up and a dry-down as a slow fall.
 *
 * Pure functions on records, no I/O.  Feed the records of a time range through dry_acc_visit
 * (it is a gt_store_visit_fn, so store_read_range can call it directly), then call dry_analyze.
 * Records that are not real soil measurements are skipped and counted: INVALID, STUCK,
 * TIME_UNSET and CRC-corrupt ones.
 *
 * How a cycle is found:
 *   1. The good samples are averaged into buckets (draft [10 min]), which cuts the sensor
 *      noise (about +-16 counts for a single read) and keeps memory small.
 *   2. Watering is a rise of at least rise_counts above the lowest bucket since the cycle
 *      began.  The cycle that was falling ends at that lowest bucket.
 *   3. The next cycle starts at the peak after the watering.  The peak is accepted once a
 *      bucket sits settle_counts below the highest one, so a still-rising sensor is not
 *      cut short.  Right after watering there is no open cycle until that happens.
 *   4. A straight line is fitted through each cycle.  A cycle is accepted only if it has no
 *      long gap, is long enough, falls, and fits the line.  Every other cycle is kept in the
 *      result with the reason it was rejected, so nothing is forced into a prediction.
 *
 * A straight line is a draft assumption: a real dry-down curves as the soil dries.  The
 * R^2 check is what notices when that is badly wrong.  All the numbers are drafts. */
#ifndef GT_DRY_H
#define GT_DRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "record.h"
#include "store.h"

#define DRY_MAX_CYCLES  64u     /* the newest are kept; cycles_total says how many were found */
#define DRY_MAX_BUCKETS 100000u /* about 2 years at 10 minutes; more stops the read */

typedef struct dry_params {
    uint64_t bucket_ms;     /* averaging window, at least 1000 */
    double   rise_counts;   /* a rise this big above the lowest bucket is watering */
    double   settle_counts; /* the peak after watering ends when a bucket is this far below it */
    uint64_t min_cycle_ms;  /* shorter cycles are rejected */
    uint64_t max_gap_ms;    /* a hole this long between buckets inside a cycle rejects it */
    double   min_r2;        /* the line must explain at least this much of the variation */
    size_t   max_buckets;   /* a read with more buckets than this is stopped; 1..DRY_MAX_BUCKETS */
    double   dry_raw;       /* #26: only used to find where each cycle reaches the dry level */
} dry_params;

/* Draft: [10 min], [60], [12], [12 h], [6 h], [0.8], DRY_MAX_BUCKETS, dry_raw [450].  60 is
 * about four times the noise.  450 is the top of the typical STEMMA "dry" range, 350-450. */
void dry_params_default(dry_params* p);

/* #26: the light module reports how long the flag read bright in each record's cycle
 * (light_bright_ms, at most this long).  Longer values are clamped. */
#define DRY_LIGHT_WINDOW_MS 60000.0

typedef struct dry_bucket {
    uint64_t id;        /* timestamp / bucket_ms */
    double   sum;       /* of soil_raw */
    double   t_sum;     /* of the milliseconds past the start of the bucket */
    uint32_t n;
    double   bright_ms; /* of light_bright_ms, over the records whose light is valid */
    uint32_t light_n;   /* records with a valid light reading (n - light_n have LIGHT_INVALID) */
} dry_bucket;

typedef struct dry_acc {
    dry_params  p;
    dry_bucket* b; /* finished buckets, oldest first */
    size_t      n, cap;
    bool        have_cur;
    dry_bucket  cur; /* the bucket still being filled */
    uint64_t    records, used;
    uint64_t    synthetic; /* readable records with GT_REC_SYNTHETIC: counted, never filtered out */
    uint64_t    skipped_invalid, skipped_stuck, skipped_time_unset, skipped_corrupt;
    uint64_t    out_of_order;
    bool        overflow; /* DRY_MAX_BUCKETS reached; the read was stopped */
    bool        oom;      /* an allocation failed; the read was stopped */
} dry_acc;

/* p NULL means the defaults.  Returns 0, or -1 with errno EINVAL for a nonsense parameter
 * (nothing is allocated then, and dry_acc_free is still safe). */
int  dry_acc_init(dry_acc* a, const dry_params* p);
void dry_acc_free(dry_acc* a); /* safe twice and after a failed init */

/* A gt_store_visit_fn.  Records must come oldest first; one that goes back to an earlier
 * bucket is counted in out_of_order and ignored.  Returns 0 to continue, or 1 to stop when
 * an allocation failed or DRY_MAX_BUCKETS was reached. */
int dry_acc_visit(const gt_record* rec, gt_store_rec_status status, void* ctx);

/* Why a read was stopped: 0 if it was not, ENOMEM (an allocation failed) or EOVERFLOW
 * (DRY_MAX_BUCKETS reached: the window is too long for the bucket size).  A result from a
 * stopped read is incomplete and must not be used for a prediction. */
int dry_acc_error(const dry_acc* a);

typedef enum dry_reject {
    DRY_OK = 0,           /* accepted: usable for a prediction */
    DRY_REJ_GAP,          /* a hole longer than max_gap_ms inside the cycle */
    DRY_REJ_SHORT,        /* shorter than min_cycle_ms */
    DRY_REJ_NOT_FALLING,  /* the fitted slope is not downward */
    DRY_REJ_POOR_FIT      /* R^2 below min_r2 */
} dry_reject;

const char* dry_reject_name(dry_reject r); /* "ok", "gap", "short", "not_falling", "poor_fit" */

typedef struct dry_cycle {
    uint64_t   start_ms, end_ms;   /* the first and last bucket of the cycle (mean timestamps) */
    double     start_raw, end_raw; /* their averaged soil values */
    double     slope_per_h;        /* counts per hour; negative for a dry-down */
    double     r2;
    size_t     buckets;
    bool       complete;           /* ended by watering; false for the open (current) cycle */
    dry_reject reject;
    /* #26: light over the whole cycle.  bright_h_per_day is hours of "bright" per 24 h, from
     * the records with a valid light reading; light_coverage is the share of the cycle's
     * records that had one (0..1).  With no valid light at all both are 0. */
    double     bright_h_per_day;
    double     light_coverage;
    /* #26, for the light-versus-soil-only comparison (C3).  cross_ms is the first bucket at or
     * below dry_raw (0 if the cycle never got there).  have_eval says there is a bucket at
     * least DRY_EVAL_AHEAD_H hours before it, inside the cycle, with valid light so far:
     * level_eval and bright_eval (bright hours/day from the cycle start to that bucket) are
     * what the device would have known then. */
    uint64_t   cross_ms;
    bool       have_eval;
    double     level_eval;
    double     bright_eval;
} dry_cycle;

/* How long before the crossing the C3 comparison predicts (DESIGN.md C3: 12 hours). */
#define DRY_EVAL_AHEAD_H 12.0

typedef struct dry_result {
    double    dry_raw;      /* the parameter the cycles' cross_ms refer to */
    size_t    n_cycles;     /* cycles[0..n_cycles), oldest first */
    size_t    cycles_total; /* found, including any older ones that no longer fit */
    dry_cycle cycles[DRY_MAX_CYCLES];
    size_t    buckets;
    bool      have_last;    /* there is at least one good sample */
    uint64_t  last_ts_ms;   /* the newest good bucket */
    double    last_raw;
    uint64_t  records, used;
    uint64_t  synthetic;    /* of records: written by a fake source, not a sensor */
    uint64_t  skipped_invalid, skipped_stuck, skipped_time_unset, skipped_corrupt;
    uint64_t  out_of_order;
} dry_result;

/* Reads the accumulator without changing it (the bucket still being filled counts), so it
 * can be called again after more records.  Always fills *out; it allocates nothing. */
void dry_analyze(const dry_acc* a, dry_result* out);

#endif /* GT_DRY_H */
