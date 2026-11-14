/* thirst.h: does the plant need water now? (#43)
 *
 * A Schmitt trigger over the good soil samples, oldest first.  Readings rise as the soil gets
 * wetter, so "dry" means at or below dry_raw.  The state flips to NEEDS_WATER when a sample is
 * at or below dry_raw, and back to OK only when a sample reaches dry_raw + band.  The band is
 * wider than the sensor noise (about 16 counts, one standard deviation, DESIGN.md), so noise
 * around the threshold does not flip the state.
 *
 * It says UNKNOWN, never OK, when it cannot be sure: no good sample, the newest record is
 * INVALID, STUCK or has an unset clock (it does not fall back to an older good sample), the
 * newest record is too old, or its time is ahead of now.
 *
 * Skipped for the state: INVALID, STUCK and TIME_UNSET records, the same set dry.c skips.
 * The caller skips corrupt records (the store reports them with a status, not a record).
 *
 * The dry threshold is an INPUT.  It has not been calibrated (S1 was descoped, DESIGN.md), so
 * a caller must not present it as measured.  All the numbers are drafts. */
#ifndef GT_THIRST_H
#define GT_THIRST_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"

#define THIRST_DRY_DEFAULT 450u /* typical STEMMA range: dry (ready to water soon) is 350-450 */

typedef struct thirst_params {
    uint16_t dry_raw;  /* dry at or below this */
    uint16_t band;     /* back to OK at dry_raw + band; 0 acts as 1 (no hysteresis) */
    uint64_t fresh_ms; /* the newest record may be at most this old */
    uint64_t ahead_ms; /* ... and at most this far ahead of now */
} thirst_params;

/* Draft: [48] counts (three times the 16-count noise), [3 min] fresh (three 60 s cycles, like
 * the page), [60 s] ahead, and the dry threshold the caller passes in. */
void thirst_params_default(thirst_params* p, uint16_t dry_raw);

typedef enum thirst_state {
    THIRST_UNKNOWN = 0, /* not enough to say; never shown as OK */
    THIRST_OK,
    THIRST_NEEDS_WATER
} thirst_state;

typedef enum thirst_reason {
    THIRST_R_NONE = 0,   /* the state is known */
    THIRST_R_NO_DATA,    /* no good soil sample */
    THIRST_R_NEWEST_BAD, /* the newest record is INVALID, STUCK or has an unset clock */
    THIRST_R_STALE,      /* the newest record is older than fresh_ms */
    THIRST_R_CLOCK_AHEAD /* the newest record is more than ahead_ms in the future */
} thirst_reason;

const char* thirst_state_name(thirst_state s);   /* "unknown", "ok", "needs_water" */
const char* thirst_reason_name(thirst_reason r); /* "none", "no_data", "newest_bad", "stale",
                                                    "clock_ahead" */

typedef struct thirst_acc {
    thirst_params p;
    bool          have_record;
    uint64_t      newest_ts_ms; /* of the newest record fed, good or not */
    uint16_t      newest_flags;
    bool          have_good;
    bool          needs; /* the trigger */
    uint16_t      level_raw;
    uint64_t      good_ts_ms;
} thirst_acc;

/* Copies *p.  Returns 0, or -1 with errno EINVAL for nonsense (zero fresh_ms). */
int thirst_acc_init(thirst_acc* a, const thirst_params* p);

/* Feed records oldest first (store_read_range order).  Allocates nothing, cannot fail. */
void thirst_acc_feed(thirst_acc* a, const gt_record* r);

typedef struct thirst_result {
    thirst_state  state;
    thirst_reason reason;
    uint16_t      level_raw; /* the newest good soil value (0 if there is none) */
    uint16_t      dry_raw;
    uint64_t      age_ms;    /* of the newest record (0 if none or ahead of now) */
} thirst_result;

/* now_ms is the wall clock.  Reads the accumulator without changing it. */
void thirst_judge(const thirst_acc* a, uint64_t now_ms, thirst_result* out);

#endif /* GT_THIRST_H */
