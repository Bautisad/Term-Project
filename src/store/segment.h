/* segment.h: one log file per UTC day, named YYYY-MM-DD.log, inside the store directory.
 * A "day" is the number of days since 1970-01-01 UTC.  Shared by the append, recovery
 * and read paths. */
#ifndef GT_SEGMENT_H
#define GT_SEGMENT_H

#include <stdbool.h>
#include <stdint.h>

#include "record.h"
#include "store.h"

/* UTC day of a timestamp in Unix milliseconds. */
int64_t segment_day(uint64_t ts_unix_ms);

/* Formats "YYYY-MM-DD.log" into out (GT_STORE_SEGMENT_NAME_MAX bytes).  Returns 0, or -1
 * with errno EINVAL when the day cannot be written as a four-digit year. */
int segment_name(int64_t day, char out[GT_STORE_SEGMENT_NAME_MAX]);

/* True and *day set when name is exactly a segment file name for a real date. */
bool segment_parse(const char* name, int64_t* day);

/* Finds the newest segment in the directory whose day is below limit (INT64_MAX for the
 * newest overall).  Returns 0 and sets *day, 1 when there is none, or -1 with errno set. */
int segment_latest(int dir_fd, int64_t limit, int64_t* day);

/* Opens the segment for appending, creating it when missing.  A new segment is followed
 * by an fsync of the directory so the file cannot vanish after a crash.  *created tells
 * which case it was.  Returns the fd (close-on-exec), or -1 with errno set. */
int segment_open(int dir_fd, int64_t day, bool* created);

/* Checks that the segment ends cleanly: its length is a whole number of records and the
 * last record's CRC32 matches.  On success *has_last says whether the segment holds any
 * record and *last is that record.  A ragged or corrupt tail returns -1 with EUCLEAN
 * (truncating it is the recovery path's job); other failures set errno as usual. */
int segment_check(int dir_fd, int64_t day, gt_record* last, bool* has_last);

#endif /* GT_SEGMENT_H */
