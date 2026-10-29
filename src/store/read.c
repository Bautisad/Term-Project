/* read.c: store_read_range, the read-only side of the store.  It never takes the writer's
 * lock and opens every file read-only, so analysis and exports run while the hub appends. */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.h"
#include "segment.h"
#include "store.h"
#include "store_log.h"

#define CHUNK_RECORDS 128u

typedef struct reader {
    uint64_t          from_ms;
    uint64_t          to_ms;
    gt_store_visit_fn visit;
    void*             ctx;
    bool              seen;    /* a record in range has been visited */
    uint64_t          pending; /* bad records since the last in-range one, once seen */
    bool              stop;    /* visit asked to stop */
} reader;

static void deliver(reader* r, const gt_record* rec, gt_store_rec_status status)
{
    if (r->visit(rec, status, r->ctx) != 0) {
        r->stop = true;
    }
}

/* A valid record: visit it when in range, preceded by the bad records that sit between it
 * and the previous in-range record.  Out-of-range records leave that count alone. */
static void on_valid(reader* r, const gt_record* rec)
{
    gt_record zero;

    if (rec->ts_unix_ms < r->from_ms || rec->ts_unix_ms >= r->to_ms) {
        return;
    }
    memset(&zero, 0, sizeof zero);
    for (; r->pending > 0 && !r->stop; r->pending--) {
        deliver(r, &zero, GT_STORE_REC_CORRUPT);
    }
    if (!r->stop) {
        r->seen = true;
        deliver(r, rec, GT_STORE_REC_OK);
    }
}

static void log_run(const char* name, off_t at, uint64_t run)
{
    store_log("ERROR", "crc_mismatch segment=%s offset=%lld skipped=%llu", name, (long long)at,
              (unsigned long long)run);
}

/* Reads one segment front to back.  is_last: the newest segment, where a torn tail is
 * normal (the writer may be mid-append) and is skipped without a word. */
static int read_segment(int dir_fd, int64_t day, bool is_last, reader* r)
{
    char        name[GT_STORE_SEGMENT_NAME_MAX];
    uint8_t     chunk[GT_RECORD_SIZE * CHUNK_RECORDS];
    struct stat st;
    off_t       whole;
    off_t       at = 0;
    off_t       run_at = 0;
    uint64_t    run = 0; /* consecutive bad records so far */
    int         fd;
    int         err = 0;

    if (segment_name(day, name) != 0) {
        return -1;
    }
    fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = errno;
        store_log("ERROR", "segment open failed segment=%s errno=%d", name, err);
        goto out;
    }
    if (fstat(fd, &st) != 0) {
        err = errno;
        store_log("ERROR", "segment fstat failed segment=%s errno=%d", name, err);
        goto out;
    }
    whole = st.st_size - st.st_size % (off_t)GT_RECORD_SIZE;
    if (whole != st.st_size && !is_last) {
        store_log("WARN", "segment ragged_tail segment=%s size=%lld", name, (long long)st.st_size);
    }
    while (at < whole && !r->stop) {
        off_t  left = whole - at;
        size_t n = left < (off_t)sizeof chunk ? (size_t)left : sizeof chunk;

        if (segment_pread(fd, chunk, n, at) != 0) {
            err = errno;
            store_log("ERROR", "segment read failed segment=%s errno=%d", name, err);
            goto out;
        }
        for (size_t i = 0; i < n && !r->stop; i += GT_RECORD_SIZE) {
            gt_record rec;

            if (!record_decode(chunk + i, &rec)) {
                if (run++ == 0) {
                    run_at = at + (off_t)i;
                }
                if (r->seen) {
                    r->pending++;
                }
                continue;
            }
            if (run > 0) {
                log_run(name, run_at, run);
                run = 0;
            }
            on_valid(r, &rec);
        }
        at += (off_t)n;
    }
    if (run > 0 && !is_last && !r->stop) {
        log_run(name, run_at, run);
    }
out:
    store_close_fd(fd);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

int store_read_range(const char* dir, uint64_t from_ms, uint64_t to_ms, gt_store_visit_fn visit,
                     void* ctx)
{
    reader  r;
    int64_t day;
    int     dir_fd;
    int     rc;
    int     err = 0;

    if (dir == NULL || visit == NULL || from_ms > to_ms) {
        errno = EINVAL;
        return -1;
    }
    if (from_ms == to_ms) {
        return 0; /* an empty range */
    }
    memset(&r, 0, sizeof r);
    r.from_ms = from_ms;
    r.to_ms = to_ms;
    r.visit = visit;
    r.ctx = ctx;

    dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        err = errno;
        store_log("ERROR", "read open failed errno=%d", err);
        errno = err;
        return -1;
    }
    /* A record's own day is never later than its segment's, so older segments cannot hold
     * it.  Newer ones can: a record stamped while the clock was wrong sits in the newest
     * segment, so every later segment is read. */
    rc = segment_next(dir_fd, segment_day(from_ms) - 1, &day);
    while (rc == 0 && !r.stop) {
        int64_t cur = day;

        rc = segment_next(dir_fd, cur, &day); /* look ahead: 1 means cur is the newest */
        if (rc < 0 || read_segment(dir_fd, cur, rc > 0, &r) != 0) {
            err = errno;
            break;
        }
    }
    store_close_fd(dir_fd);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}
