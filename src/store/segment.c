#include "segment.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.h"
#include "store_log.h"

#define DAY_MS        86400000u
#define SEGMENT_CHARS 14u /* "YYYY-MM-DD.log" */

/* Calendar arithmetic on days since 1970-01-01 (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    int64_t era;
    int64_t yoe;
    int64_t doy;
    int64_t doe;

    y -= m <= 2 ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t* y, int* m, int* d)
{
    int64_t era;
    int64_t doe;
    int64_t yoe;
    int64_t doy;
    int64_t mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2 ? 1 : 0);
}

int64_t segment_day(uint64_t ts_unix_ms)
{
    return (int64_t)(ts_unix_ms / DAY_MS);
}

int segment_name(int64_t day, char out[GT_STORE_SEGMENT_NAME_MAX])
{
    int64_t y;
    int     m;
    int     d;
    int     n;

    civil_from_days(day, &y, &m, &d);
    if (y < 0 || y > 9999) {
        errno = EINVAL;
        return -1;
    }
    n = snprintf(out, GT_STORE_SEGMENT_NAME_MAX, "%04d-%02d-%02d.log", (int)y, m, d);
    if (n < 0 || n >= GT_STORE_SEGMENT_NAME_MAX) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static bool all_digits(const char* s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

static int to_int(const char* s, size_t n)
{
    int v = 0;

    for (size_t i = 0; i < n; i++) {
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

bool segment_parse(const char* name, int64_t* day)
{
    char    again[GT_STORE_SEGMENT_NAME_MAX];
    int64_t parsed;
    int     m;
    int     d;

    if (strlen(name) != SEGMENT_CHARS || name[4] != '-' || name[7] != '-' ||
        strcmp(name + 10, ".log") != 0 || !all_digits(name, 4) || !all_digits(name + 5, 2) ||
        !all_digits(name + 8, 2)) {
        return false;
    }
    m = to_int(name + 5, 2);
    d = to_int(name + 8, 2);
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    parsed = days_from_civil(to_int(name, 4), m, d);
    /* Reject dates that do not exist, such as 2026-02-30: they must round-trip. */
    if (segment_name(parsed, again) != 0 || strcmp(again, name) != 0) {
        return false;
    }
    *day = parsed;
    return true;
}

/* The one directory scan behind segment_latest and segment_next: the newest segment whose
 * day is below bound, or (newest_below false) the oldest whose day is above it. */
static int segment_scan(int dir_fd, int64_t bound, bool newest_below, int64_t* day)
{
    int            fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR*           dp;
    struct dirent* ent;
    int64_t        best = 0;
    bool           found = false;
    int            err = 0;

    if (fd < 0) {
        store_log("ERROR", "scan open failed errno=%d", errno);
        return -1;
    }
    dp = fdopendir(fd);
    if (dp == NULL) {
        err = errno;
        store_log("ERROR", "scan fdopendir failed errno=%d", err);
        store_close_fd(fd);
        errno = err;
        return -1;
    }
    for (;;) {
        int64_t d;

        errno = 0;
        ent = readdir(dp);
        if (ent == NULL) {
            err = errno;
            break;
        }
        if (segment_parse(ent->d_name, &d) &&
            (newest_below ? d < bound && (!found || d > best) : d > bound && (!found || d < best))) {
            best = d;
            found = true;
        }
    }
    if (err != 0) {
        store_log("ERROR", "scan readdir failed errno=%d", err);
    }
    if (closedir(dp) != 0 && err == 0) {
        err = errno;
        store_log("ERROR", "scan closedir failed errno=%d", err);
    }
    if (err != 0) {
        errno = err;
        return -1;
    }
    if (!found) {
        return 1;
    }
    *day = best;
    return 0;
}

int segment_latest(int dir_fd, int64_t limit, int64_t* day)
{
    return segment_scan(dir_fd, limit, true, day);
}

int segment_next(int dir_fd, int64_t after, int64_t* day)
{
    return segment_scan(dir_fd, after, false, day);
}

int segment_open(int dir_fd, int64_t day, bool* created)
{
    char name[GT_STORE_SEGMENT_NAME_MAX];
    int  fd;
    int  err;

    if (segment_name(day, name) != 0) {
        return -1;
    }
    fd = openat(dir_fd, name, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (fsync(dir_fd) != 0) {
            err = errno;
            store_log("ERROR", "segment dir fsync failed segment=%s errno=%d", name, err);
            store_close_fd(fd);
            errno = err;
            return -1;
        }
        store_log("INFO", "segment_created segment=%s", name);
        *created = true;
        return fd;
    }
    if (errno != EEXIST) {
        store_log("ERROR", "segment open failed segment=%s errno=%d", name, errno);
        return -1;
    }
    fd = openat(dir_fd, name, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        store_log("ERROR", "segment open failed segment=%s errno=%d", name, errno);
        return -1;
    }
    *created = false;
    return fd;
}

int segment_pread(int fd, uint8_t* buf, size_t len, off_t off)
{
    size_t got = 0;

    while (got < len) {
        ssize_t n = pread(fd, buf + got, len - got, off + (off_t)got);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

int segment_check(int dir_fd, int64_t day, gt_record* last, bool* has_last)
{
    char        name[GT_STORE_SEGMENT_NAME_MAX];
    uint8_t     buf[GT_RECORD_SIZE];
    struct stat st;
    int         fd;
    int         err = 0;

    *has_last = false;
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
    if (st.st_size % (off_t)GT_RECORD_SIZE != 0) {
        err = EUCLEAN;
        store_log("WARN", "segment ragged_tail segment=%s size=%lld", name, (long long)st.st_size);
        goto out;
    }
    if (st.st_size > 0) {
        off_t at = st.st_size - (off_t)GT_RECORD_SIZE;

        if (segment_pread(fd, buf, sizeof buf, at) != 0) {
            err = errno;
            store_log("ERROR", "segment read failed segment=%s errno=%d", name, err);
            goto out;
        }
        if (!record_decode(buf, last)) {
            err = EUCLEAN;
            store_log("WARN", "segment crc_mismatch_tail segment=%s offset=%lld", name,
                      (long long)at);
            goto out;
        }
        *has_last = true;
    }
out:
    store_close_fd(fd);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

/* Reads the record at offset at; *good says whether its CRC32 matched.  Logs a failure. */
static int read_record(int fd, off_t at, const char* name, gt_record* rec, bool* good)
{
    uint8_t buf[GT_RECORD_SIZE];

    if (segment_pread(fd, buf, sizeof buf, at) != 0) {
        store_log("ERROR", "segment read failed segment=%s errno=%d", name, errno);
        return -1;
    }
    *good = record_decode(buf, rec);
    return 0;
}

int segment_recover(int dir_fd, int64_t day, gt_record* last, bool* has_last, uint64_t* dropped)
{
    const off_t rsz = (off_t)GT_RECORD_SIZE;
    char        name[GT_STORE_SEGMENT_NAME_MAX];
    struct stat st;
    off_t       ragged;
    off_t       keep;
    bool        good = true;
    bool        too_much = false;
    int         fd;
    int         err = 0;

    *has_last = false;
    *dropped = 0;
    if (segment_name(day, name) != 0) {
        return -1;
    }
    fd = openat(dir_fd, name, O_RDWR | O_CLOEXEC);
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
    ragged = st.st_size % rsz;
    keep = st.st_size - ragged; /* whole records only */

    /* Decide the cut first: nothing is modified until the kept prefix is known to be sound. */
    if (keep > 0) {
        if (read_record(fd, keep - rsz, name, last, &good) != 0) {
            err = errno;
            goto out;
        }
        if (!good && ragged != 0) {
            too_much = true; /* a bad record before a torn one is two interrupted writes */
        } else if (!good) {
            keep -= rsz; /* the record that was being written when the process died */
            if (keep > 0) {
                if (read_record(fd, keep - rsz, name, last, &good) != 0) {
                    err = errno;
                    goto out;
                }
                too_much = !good;
            }
        }
    }
    if (too_much) {
        err = EUCLEAN;
        store_log("ERROR", "segment damaged_tail segment=%s size=%lld", name,
                  (long long)st.st_size);
        goto out;
    }
    *has_last = keep > 0;
    if (keep != st.st_size) {
        if (ftruncate(fd, keep) != 0) {
            err = errno;
            store_log("ERROR", "segment truncate failed segment=%s errno=%d", name, err);
            goto out;
        }
        if (fsync(fd) != 0) {
            err = errno;
            store_log("ERROR", "segment fsync failed segment=%s errno=%d", name, err);
            goto out;
        }
        *dropped = (uint64_t)(st.st_size - keep);
    }
out:
    store_close_fd(fd);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}
