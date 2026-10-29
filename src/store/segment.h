/* segment.h: one log file per UTC day, named YYYY-MM-DD.log, inside the store directory.
 * A "day" is the number of days since 1970-01-01 UTC.  Shared by the append, recovery
 * and read paths. */
#ifndef GT_SEGMENT_H
#define GT_SEGMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

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

/* Finds the oldest segment whose day is above after.  Same returns as segment_latest. */
int segment_next(int dir_fd, int64_t after, int64_t* day);

/* pread() until buf is full; a short file counts as EIO.  Returns 0 or -1 with errno set. */
int segment_pread(int fd, uint8_t* buf, size_t len, off_t off);

/* Opens the segment for appending, creating it when missing.  A new segment is followed
 * by an fsync of the directory so the file cannot vanish after a crash.  *created tells
 * which case it was.  Returns the fd (close-on-exec), or -1 with errno set. */
int segment_open(int dir_fd, int64_t day, bool* created);

/* Checks that the segment ends cleanly: its length is a whole number of records and the
 * last record's CRC32 matches.  On success *has_last says whether the segment holds any
 * record and *last is that record.  A ragged or corrupt tail returns -1 with EUCLEAN
 * (the newest segment goes through segment_recover instead); other failures set errno
 * as usual.  Read-only. */
int segment_check(int dir_fd, int64_t day, gt_record* last, bool* has_last);

/* Recovery for the newest segment; the caller holds the directory lock.  It repairs what
 * one interrupted append can leave behind and nothing more: a short final record, or one
 * full-size final record with a bad CRC.  Both together, or two bad final records, are
 * too much for one interrupted write (a failing card, not a torn write), so it returns
 * -1 with EUCLEAN and leaves the file as it found it.  The cut is decided before
 * anything is modified; the file is then truncated and fsynced.  *dropped is the number
 * of bytes cut, and *has_last / *last describe the last good record that remains. */
int segment_recover(int dir_fd, int64_t day, gt_record* last, bool* has_last, uint64_t* dropped);

#endif /* GT_SEGMENT_H */
