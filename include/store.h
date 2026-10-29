/* store.h: storage API.  DRAFT: shared contract, needs both owners' sign-off.
 *
 * Append-only log of gt_record (see record.h), one segment file per UTC day named
 * YYYY-MM-DD.log inside the store directory.  The hub is the only writer; interface
 * and analysis only read.
 *
 *   hub                 store_recover -> store_append (every cycle) -> store_close
 *   interface/analysis  store_read_range (takes a path, never modifies anything)
 *
 * Durability: store_append returns only after fsync, and creating a segment also
 * fsyncs the directory, so a power cut costs at most the one record in flight.
 *
 * Errors: every call returns 0 on success, or -1 with errno set.  The storage errors
 * the hub reacts to (ENOSPC, EROFS, EIO) are passed through unchanged.  The store
 * logs its own events (recovered, crc_mismatch, append_failed) in the DESIGN.md log
 * format; the caller does not need to log them again.
 */
#ifndef GT_STORE_H
#define GT_STORE_H

#include <stdint.h>

#include "record.h"

#define GT_STORE_SEGMENT_NAME_MAX 16 /* "YYYY-MM-DD.log" plus NUL */

/* Opaque writer handle.  Holds the directory lock and the open newest segment. */
typedef struct gt_store gt_store;

/* What store_recover found.  last_seq is valid only when has_records is nonzero. */
typedef struct gt_store_recovery {
    int      has_records;   /* 0 for an empty or new store */
    uint32_t last_seq;      /* seq of the last valid record; the hub resumes at +1 */
    uint64_t dropped_bytes; /* bytes cut from the tail of the newest segment */
    char     segment[GT_STORE_SEGMENT_NAME_MAX]; /* newest segment, "" if none */
} gt_store_recovery;

/* Writer only.  Creates dir if it is missing, takes an exclusive lock on it (released
 * on store_close or when the process dies, even by kill -9), scans the tail of the
 * newest segment, and truncates after the last valid record.  Any dropped bytes are
 * logged and reported in *info.  info may be NULL.
 * errno: EBUSY another writer holds the lock; EINVAL bad arguments; ENOMEM; or the
 * error from the failing syscall. */
int store_recover(const char* dir, gt_store** out, gt_store_recovery* info);

/* Serializes rec field by field in little-endian, computes the CRC32 and writes it,
 * so rec->crc32 is ignored and rec is not modified.  The caller assigns ts_unix_ms,
 * seq, soil_raw, flags and the light fields; the store does not check seq (the log
 * checker does).  A record never goes into a segment older than the newest one: if
 * its UTC day is earlier (clock went backwards, GT_REC_TIME_UNSET), it is appended to
 * the newest segment.  Returns only after fsync.
 * On failure the store cuts the file back to the last good record, so no partial
 * record is left behind and the caller may retry.  If even that fails, later appends
 * return -1 with EIO until the hub calls store_recover again. */
int store_append(gt_store* store, const gt_record* rec);

/* Releases the lock and closes the segment.  A failure is logged.  NULL is a no-op. */
void store_close(gt_store* store);

typedef enum gt_store_rec_status {
    GT_STORE_REC_OK      = 0,
    GT_STORE_REC_CORRUPT = 1 /* CRC mismatch mid-log; every field of *rec is zero */
} gt_store_rec_status;

/* Called once per record, oldest first.  rec is never NULL and is valid only during
 * the call.  Return 0 to continue or nonzero to stop early. */
typedef int (*gt_store_visit_fn)(const gt_record* rec, gt_store_rec_status status,
                                 void* ctx);

/* Read-only.  Visits every record with from_ms <= ts_unix_ms < to_ms in segment and
 * file order.  A torn tail on the newest segment is skipped silently (the writer may
 * be mid-append).  A record that fails its CRC is reported as GT_STORE_REC_CORRUPT
 * only when it lies between two records that are in range, because its own timestamp
 * cannot be trusted; the next record boundary resyncs the reader.  Stopping early
 * through visit is not an error.  Does not allocate. */
int store_read_range(const char* dir, uint64_t from_ms, uint64_t to_ms,
                     gt_store_visit_fn visit, void* ctx);

#endif /* GT_STORE_H */
