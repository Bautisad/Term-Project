#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.h"
#include "segment.h"
#include "store_log.h"

struct gt_store {
    int     dir_fd;
    int     lock_fd;
    int     seg_fd;   /* open segment, -1 until the first append of an empty store */
    int64_t day;      /* UTC day of the open segment, INT64_MIN when there is none */
    off_t   good_len; /* length of the open segment after its last good record */
    bool    failed;   /* an append could not be rolled back; refuse until store_recover */
};

/* Single-writer lock: held on dir/.lock until store_close, or until the process dies. */
static int lock_dir(gt_store* s)
{
    s->lock_fd = openat(s->dir_fd, ".lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (s->lock_fd < 0) {
        store_log("ERROR", "lock open failed errno=%d", errno);
        return -1;
    }
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        int err = errno == EWOULDBLOCK ? EBUSY : errno;

        store_log("ERROR", "lock failed errno=%d", err);
        errno = err;
        return -1;
    }
    return 0;
}

/* Opens the newest segment for appending and reports the last sequence number.  An empty
 * newest segment (created, then the process died) falls back to the older ones. */
static int scan_segments(gt_store* s, gt_store_recovery* info)
{
    struct stat st;
    bool        created;
    int64_t     newest;
    int64_t     day;
    uint64_t    dropped = 0;
    int         rc = segment_latest(s->dir_fd, INT64_MAX, &newest);

    if (rc != 0) {
        return rc < 0 ? -1 : 0; /* 1: a new, empty store */
    }
    day = newest;
    for (;;) {
        gt_record last;
        bool      has_last;
        int       chk;

        /* Only the newest segment can hold a torn write; older ones must be clean. */
        if (day == newest) {
            chk = segment_recover(s->dir_fd, day, &last, &has_last, &dropped);
        } else {
            chk = segment_check(s->dir_fd, day, &last, &has_last);
        }
        if (chk != 0) {
            return -1;
        }
        if (has_last) {
            info->has_records = 1;
            info->last_seq = last.seq;
            break;
        }
        rc = segment_latest(s->dir_fd, day, &day);
        if (rc < 0) {
            return -1;
        }
        if (rc > 0) {
            break;
        }
    }
    if (segment_name(newest, info->segment) != 0) {
        return -1;
    }
    info->dropped_bytes = dropped;
    if (dropped > 0 && info->has_records) {
        store_log("WARN", "recovered segment=%s dropped_bytes=%llu last_seq=%u", info->segment,
                  (unsigned long long)dropped, (unsigned)info->last_seq);
    } else if (dropped > 0) {
        store_log("WARN", "recovered segment=%s dropped_bytes=%llu last_seq=none", info->segment,
                  (unsigned long long)dropped);
    }
    s->seg_fd = segment_open(s->dir_fd, newest, &created);
    if (s->seg_fd < 0) {
        return -1;
    }
    if (fstat(s->seg_fd, &st) != 0) {
        store_log("ERROR", "segment fstat failed segment=%s errno=%d", info->segment, errno);
        return -1;
    }
    s->day = newest;
    s->good_len = st.st_size;
    return 0;
}

int store_recover(const char* dir, gt_store** out, gt_store_recovery* info)
{
    gt_store_recovery local;
    gt_store_recovery* r = info != NULL ? info : &local;
    gt_store*         s;
    int               err;

    if (dir == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    memset(r, 0, sizeof *r);
    s = calloc(1, sizeof *s);
    if (s == NULL) {
        store_log("ERROR", "recover alloc failed");
        errno = ENOMEM;
        return -1;
    }
    s->dir_fd = -1;
    s->lock_fd = -1;
    s->seg_fd = -1;
    s->day = INT64_MIN;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        store_log("ERROR", "recover mkdir failed errno=%d", errno);
        goto fail;
    }
    s->dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (s->dir_fd < 0) {
        store_log("ERROR", "recover open failed errno=%d", errno);
        goto fail;
    }
    if (lock_dir(s) != 0 || scan_segments(s, r) != 0) {
        goto fail;
    }
    *out = s;
    return 0;

fail:
    err = errno;
    store_close(s);
    errno = err;
    return -1;
}

/* Switches the open segment to day, which is never older than the current one. */
static int switch_segment(gt_store* s, int64_t day)
{
    struct stat st;
    bool        created;
    int         err;
    int         fd = segment_open(s->dir_fd, day, &created);

    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        err = errno;
        store_log("ERROR", "segment fstat failed errno=%d", err);
        store_close_fd(fd);
        errno = err;
        return -1;
    }
    if (st.st_size % (off_t)GT_RECORD_SIZE != 0) {
        store_log("WARN", "segment ragged_tail size=%lld", (long long)st.st_size);
        store_close_fd(fd);
        errno = EUCLEAN;
        return -1;
    }
    store_close_fd(s->seg_fd); /* every record in it was fsynced when it was appended */
    s->seg_fd = fd;
    s->day = day;
    s->good_len = st.st_size;
    return 0;
}

int store_append(gt_store* store, const gt_record* rec)
{
    uint8_t buf[GT_RECORD_SIZE];
    size_t  off = 0;
    int64_t day;
    int     err;

    if (store == NULL || rec == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (store->failed) {
        errno = EIO;
        return -1;
    }
    /* A clock that went backwards must not reopen an older segment. */
    day = segment_day(rec->ts_unix_ms);
    if (day < store->day) {
        day = store->day;
    }
    if ((day != store->day || store->seg_fd < 0) && switch_segment(store, day) != 0) {
        store_log("ERROR", "append_failed errno=%d seq=%u", errno, (unsigned)rec->seq);
        return -1;
    }
    record_encode(rec, buf);
    while (off < sizeof buf) {
        ssize_t n = write(store->seg_fd, buf + off, sizeof buf - off);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (n == 0) {
                errno = EIO;
            }
            goto fail;
        }
        off += (size_t)n;
    }
    if (fsync(store->seg_fd) != 0) {
        goto fail;
    }
    store->good_len += (off_t)GT_RECORD_SIZE;
    return 0;

fail:
    err = errno;
    store_log("ERROR", "append_failed errno=%d seq=%u", err, (unsigned)rec->seq);
    if (ftruncate(store->seg_fd, store->good_len) != 0) {
        store_log("ERROR", "append_rollback_failed errno=%d", errno);
        store->failed = true;
    }
    errno = err;
    return -1;
}

void store_close(gt_store* store)
{
    if (store == NULL) {
        return;
    }
    store_close_fd(store->seg_fd);
    store_close_fd(store->lock_fd); /* releases the lock */
    store_close_fd(store->dir_fd);
    free(store);
}
