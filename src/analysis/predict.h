/* predict.h: time until the soil is dry, from the dry-down cycles of dry.h (#27 step 2).
 *
 * The rate is the MEDIAN slope of the accepted, complete cycles, so one odd cycle does not
 * move it.  The estimate counts down from the newest good soil value to the dry threshold.
 * The fastest and slowest accepted cycle give a range.  When there is not enough to say
 * something honest it says nothing and gives the reason (DESIGN.md failure-table row 20),
 * never a guess.
 *
 * The dry threshold is an INPUT.  It has not been calibrated (S1 was descoped, see
 * DESIGN.md), so a caller must not present it as measured.  All the numbers are drafts. */
#ifndef GT_PREDICT_H
#define GT_PREDICT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dry.h"

typedef struct predict_params {
    double   dry_raw;    /* the soil is dry at or below this; higher readings are wetter */
    size_t   min_cycles; /* accepted complete cycles needed before predicting */
    uint64_t fresh_ms;   /* the newest good sample may be at most this old */
} predict_params;

/* Draft: [3] cycles (row 20), [30 min] fresh, and the dry threshold the caller passes in. */
void predict_params_default(predict_params* p, double dry_raw);

typedef enum predict_status {
    PREDICT_OK = 0,
    PREDICT_NO_RECENT_DATA,      /* no good sample, or the newest is older than fresh_ms */
    PREDICT_INSUFFICIENT_CYCLES, /* fewer than min_cycles accepted complete cycles */
    PREDICT_NOT_DRYING,          /* no dry-down in progress (just watered, or still rising) */
    PREDICT_NOT_FALLING          /* the current cycle is long enough and is not falling */
} predict_status;

const char* predict_status_name(predict_status s); /* "ok", "no_recent_data", "insufficient_cycles",
                                                      "not_drying", "not_falling" */

typedef struct predict_result {
    predict_status status;
    size_t         have_cycles, need_cycles; /* accepted complete cycles; the number needed */
    uint64_t       age_ms;                   /* of the newest good sample (0 if there is none) */
    double         level_raw;                /* the newest good soil value (averaged) */
    double         dry_raw;
    double         rate_per_h;               /* median slope, counts per hour, negative (OK only) */
    double         hours;                    /* until dry; 0 if already dry (OK only) */
    double         hours_fast, hours_slow;   /* from the steepest and the shallowest cycle */
    bool           already_dry;              /* level_raw <= dry_raw */
} predict_result;

/* now_ms is the wall clock.  Returns 0 and fills *out (status says whether there is a
 * prediction), or -1 with errno EINVAL for nonsense parameters (a NaN or negative dry_raw,
 * zero min_cycles or fresh_ms). */
int predict_time_until_dry(const dry_result* r, const predict_params* p, uint64_t now_ms,
                           predict_result* out);

/* The event part of the log line, in the DESIGN.md format ("<event> key=value ..."), for example
 *   prediction_withheld reason=insufficient_cycles have=2 need=3
 *   prediction hours=16.9 range=13.5-22.5 rate_per_h=-8.00 cycles=3 level=700 dry=565
 * Always NUL-terminated (if cap > 0); returns the length it needed, like snprintf. */
size_t predict_describe(const predict_result* r, char* buf, size_t cap);

/* One line on stderr: "<ISO time> INFO analysis <describe>".  The caller decides when (not
 * on every poll). */
void predict_log(const predict_result* r);

#endif /* GT_PREDICT_H */
