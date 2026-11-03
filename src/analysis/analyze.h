/* analyze.h: reads a time range of the store and finds its dry-down cycles (#27 step 3).
 * The glue between store_read_range and dry.h; use predict.h on the result.
 *
 * Needs gt_store at link time (store_read_range). */
#ifndef GT_ANALYZE_H
#define GT_ANALYZE_H

#include <stdint.h>

#include "dry.h"

/* Visits the records with from_ms <= timestamp < to_ms of the store in dir (read-only, safe
 * while the hub writes) and fills *out.  p NULL means the dry.h defaults.
 *
 * Returns 0, or -1 with errno set and *out untouched:
 *   EINVAL     nonsense parameters
 *   ENOMEM     an allocation failed
 *   EOVERFLOW  the range holds more than DRY_MAX_BUCKETS buckets (use a shorter range)
 *   otherwise  the errno store_read_range reported (for example ENOENT for a missing dir)
 * An empty store is not an error: *out then has no cycles and have_last false. */
int analysis_run(const char* dir, uint64_t from_ms, uint64_t to_ms, const dry_params* p,
                 dry_result* out);

#endif /* GT_ANALYZE_H */
