#include "thirst.h"

#include <errno.h>
#include <string.h>

#define NOISY_FLAGS (GT_REC_SOIL_INVALID | GT_REC_SOIL_STUCK | GT_REC_TIME_UNSET)

void thirst_params_default(thirst_params* p, uint16_t dry_raw)
{
    p->dry_raw = dry_raw;
    p->band = 48u;
    p->fresh_ms = 3ull * 60ull * 1000ull;
    p->ahead_ms = 60ull * 1000ull;
}

const char* thirst_state_name(thirst_state s)
{
    switch (s) {
    case THIRST_UNKNOWN:
        return "unknown";
    case THIRST_OK:
        return "ok";
    case THIRST_NEEDS_WATER:
        return "needs_water";
    }
    return "unknown";
}

const char* thirst_reason_name(thirst_reason r)
{
    switch (r) {
    case THIRST_R_NONE:
        return "none";
    case THIRST_R_NO_DATA:
        return "no_data";
    case THIRST_R_NEWEST_BAD:
        return "newest_bad";
    case THIRST_R_STALE:
        return "stale";
    case THIRST_R_CLOCK_AHEAD:
        return "clock_ahead";
    }
    return "none";
}

int thirst_acc_init(thirst_acc* a, const thirst_params* p)
{
    (void)memset(a, 0, sizeof *a);
    if (p->fresh_ms == 0u) {
        errno = EINVAL;
        return -1;
    }
    a->p = *p;
    return 0;
}

void thirst_acc_feed(thirst_acc* a, const gt_record* r)
{
    a->have_record = true;
    a->newest_ts_ms = r->ts_unix_ms;
    a->newest_flags = r->flags;

    if ((r->flags & NOISY_FLAGS) != 0u) {
        return;
    }

    /* uint32_t so dry_raw + band cannot wrap at 65535.  The way back to OK is always above
     * dry_raw, so a band of 0 still never calls a reading at the threshold OK. */
    if (!a->have_good) {
        a->needs = (uint32_t)r->soil_raw <= (uint32_t)a->p.dry_raw;
    } else if (a->needs) {
        uint32_t back = (uint32_t)a->p.dry_raw + (a->p.band > 0u ? a->p.band : 1u);

        if ((uint32_t)r->soil_raw >= back) {
            a->needs = false;
        }
    } else if ((uint32_t)r->soil_raw <= (uint32_t)a->p.dry_raw) {
        a->needs = true;
    }
    a->have_good = true;
    a->level_raw = r->soil_raw;
    a->good_ts_ms = r->ts_unix_ms;
}

void thirst_judge(const thirst_acc* a, uint64_t now_ms, thirst_result* out)
{
    (void)memset(out, 0, sizeof *out);
    out->state = THIRST_UNKNOWN;
    out->dry_raw = a->p.dry_raw;

    if (!a->have_good) {
        out->reason = THIRST_R_NO_DATA;
        return;
    }
    out->level_raw = a->level_raw;

    /* The newest record decides: an older good sample must not stand in for a bad one. */
    if ((a->newest_flags & NOISY_FLAGS) != 0u) {
        out->reason = THIRST_R_NEWEST_BAD;
        return;
    }
    if (a->newest_ts_ms > now_ms) {
        if (a->newest_ts_ms - now_ms > a->p.ahead_ms) {
            out->reason = THIRST_R_CLOCK_AHEAD;
            return;
        }
    } else {
        out->age_ms = now_ms - a->newest_ts_ms;
        if (out->age_ms > a->p.fresh_ms) {
            out->reason = THIRST_R_STALE;
            return;
        }
    }
    out->state = a->needs ? THIRST_NEEDS_WATER : THIRST_OK;
}
