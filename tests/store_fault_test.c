/* store_fault_test: what store_append and store_recover do when the disk fails (DESIGN failure
 * rows 16 and 17).  ENOSPC and EROFS come from the real kernel: a 64 KB tmpfs mounted in a
 * private user+mount namespace, so no root is needed.  EIO, and an error while the writer
 * already has the file open, cannot be produced that way, so those are injected by wrapping
 * write, fsync and ftruncate at link time (-Wl,--wrap=...); the store itself is unchanged. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "store.h"

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            (void)fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

/* 1700000000000 is 2023-11-14 22:13:20 UTC; T_DAY2 is the start of the next day. */
#define T_BASE 1700000000000ull
#define T_DAY2 1700006400000ull
#define SEG1   "2023-11-14.log"
#define SEG2   "2023-11-15.log"
#define REC    24

/* ---- fault injection: the next call to a wrapped function can be made to fail ---------- */

typedef struct fault {
    bool   armed;
    int    err;
    size_t partial; /* write only: really write this many bytes first, then fail the next call */
} fault;

enum { SITE_WRITE, SITE_FSYNC, SITE_TRUNC };

static fault    g_fault[3];
static unsigned g_calls[3];
static unsigned g_short_writes; /* writes the kernel itself cut short (a full disk) */

ssize_t __real_write(int fd, const void* buf, size_t n);
int     __real_fsync(int fd);
int     __real_ftruncate(int fd, off_t len);

static void arm(int site, int err, size_t partial)
{
    g_fault[site].armed = true;
    g_fault[site].err = err;
    g_fault[site].partial = partial;
}

/* True when this call must fail; sets errno. */
static bool fails_now(int site)
{
    g_calls[site]++;
    if (!g_fault[site].armed) {
        return false;
    }
    g_fault[site].armed = false;
    errno = g_fault[site].err;
    return true;
}

ssize_t __wrap_write(int fd, const void* buf, size_t n)
{
    ssize_t r;

    if (g_fault[SITE_WRITE].armed && g_fault[SITE_WRITE].partial > 0 &&
        g_fault[SITE_WRITE].partial < n) {
        size_t k = g_fault[SITE_WRITE].partial;

        g_calls[SITE_WRITE]++;
        r = __real_write(fd, buf, k);
        CHECK(r == (ssize_t)k);
        g_fault[SITE_WRITE].partial = 0; /* the caller's next write fails */
        return r;
    }
    if (fails_now(SITE_WRITE)) {
        return -1;
    }
    r = __real_write(fd, buf, n);
    if (r > 0 && (size_t)r < n) {
        g_short_writes++;
    }
    return r;
}

int __wrap_fsync(int fd)
{
    return fails_now(SITE_FSYNC) ? -1 : __real_fsync(fd);
}

int __wrap_ftruncate(int fd, off_t len)
{
    return fails_now(SITE_TRUNC) ? -1 : __real_ftruncate(fd, len);
}

/* ---- helpers --------------------------------------------------------------------------- */

static void path_of(char* out, size_t cap, const char* dir, const char* name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);

    CHECK(n > 0 && (size_t)n < cap);
}

static void make_tmp(char* dir, size_t cap)
{
    const char* base = getenv("TMPDIR");
    int         n = snprintf(dir, cap, "%s/gt-fault-XXXXXX", base != NULL ? base : "/tmp");

    CHECK(n > 0 && (size_t)n < cap);
    CHECK(mkdtemp(dir) != NULL);
}

static void rm_tree(const char* dir)
{
    DIR*           dp = opendir(dir);
    struct dirent* ent;

    CHECK(dp != NULL);
    while ((ent = readdir(dp)) != NULL) {
        char path[512];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        path_of(path, sizeof path, dir, ent->d_name);
        if (unlink(path) != 0) {
            CHECK(errno == EISDIR);
            rm_tree(path);
        }
    }
    CHECK(closedir(dp) == 0);
    CHECK(rmdir(dir) == 0);
}

static off_t file_size(const char* dir, const char* name)
{
    char        path[512];
    struct stat st;

    path_of(path, sizeof path, dir, name);
    CHECK(stat(path, &st) == 0);
    return st.st_size;
}

static int put_at(gt_store* s, uint32_t seq, uint64_t ts)
{
    gt_record r;

    memset(&r, 0, sizeof r);
    r.ts_unix_ms = ts;
    r.seq = seq;
    r.soil_raw = 500;
    return store_append(s, &r);
}

static int put(gt_store* s, uint32_t seq)
{
    return put_at(s, seq, T_BASE + seq * 1000ull);
}

static void put_ok(gt_store* s, uint32_t seq)
{
    CHECK(put(s, seq) == 0);
}

static int count_fn(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    uint32_t* n = ctx;

    CHECK(status == GT_STORE_REC_OK); /* never a damaged record */
    CHECK(rec->seq == *n + 1);        /* contiguous from 1: no duplicate, no gap */
    (*n)++;
    return 0;
}

/* Reads the whole store, checking every record, and returns how many there are. */
static uint32_t count_records(const char* dir)
{
    uint32_t n = 0;

    CHECK(store_read_range(dir, 0, UINT64_MAX, count_fn, &n) == 0);
    return n;
}

typedef struct capture {
    int  fd;
    int  saved;
    char path[256];
} capture;

/* Sends stderr to a temp file until cap_stop, which returns what was written. */
static void cap_start(capture* c)
{
    const char* base = getenv("TMPDIR");

    CHECK(snprintf(c->path, sizeof c->path, "%s/gt-fault-log-XXXXXX", base != NULL ? base : "/tmp") > 0);
    c->fd = mkstemp(c->path);
    CHECK(c->fd >= 0);
    CHECK(fflush(stderr) == 0);
    c->saved = dup(STDERR_FILENO);
    CHECK(c->saved >= 0 && dup2(c->fd, STDERR_FILENO) >= 0);
}

static void cap_stop(capture* c, char* out, size_t cap)
{
    ssize_t n;

    CHECK(fflush(stderr) == 0);
    CHECK(dup2(c->saved, STDERR_FILENO) >= 0 && close(c->saved) == 0);
    n = pread(c->fd, out, cap - 1, 0);
    CHECK(n >= 0);
    out[n] = '\0';
    CHECK(close(c->fd) == 0 && unlink(c->path) == 0);
}

/* Runs store_append for seq with its log captured; returns the result, errno preserved. */
static int put_logged(gt_store* s, uint32_t seq, char* log, size_t cap)
{
    capture c;
    int     rc;
    int     err;

    cap_start(&c);
    errno = 0;
    rc = put(s, seq);
    err = errno;
    cap_stop(&c, log, cap);
    errno = err;
    return rc;
}

static gt_store* open_store(const char* dir, gt_store_recovery* info)
{
    gt_store* s = NULL;

    CHECK(store_recover(dir, &s, info) == 0 && s != NULL);
    return s;
}

/* ---- part B: injected errors ----------------------------------------------------------- */

/* Every failure passes its errno through unchanged, is logged in the DESIGN row 16/17 shape,
 * leaves the file as it was, and the same record can be appended again. */
static void test_errno_passes_through(void)
{
    static const int errs[] = {ENOSPC, EROFS, EIO};
    static const int sites[] = {SITE_WRITE, SITE_FSYNC};

    for (size_t e = 0; e < sizeof errs / sizeof errs[0]; e++) {
        for (size_t k = 0; k < sizeof sites / sizeof sites[0]; k++) {
            char      dir[256];
            char      log[2048];
            char      want[64];
            gt_store* s;
            off_t     before;

            make_tmp(dir, sizeof dir);
            s = open_store(dir, NULL);
            for (uint32_t i = 1; i <= 3; i++) {
                put_ok(s, i);
            }
            before = file_size(dir, SEG1);

            arm(sites[k], errs[e], 0);
            CHECK(put_logged(s, 4, log, sizeof log) == -1);
            CHECK(errno == errs[e]);
            (void)snprintf(want, sizeof want, "ERROR store append_failed errno=%d seq=4", errs[e]);
            CHECK(strstr(log, want) != NULL);
            CHECK(file_size(dir, SEG1) == before); /* a failed fsync leaves no record behind */

            put_ok(s, 4); /* the retry */
            CHECK(file_size(dir, SEG1) == before + REC);
            CHECK(count_records(dir) == 4); /* seq 4 exactly once */
            store_close(s);
            rm_tree(dir);
        }
    }
}

/* Part of the record reaches the disk, then the write fails: the partial bytes are cut off. */
static void test_short_write_then_error(void)
{
    char      dir[256];
    char      log[2048];
    gt_store* s;
    off_t     before;

    make_tmp(dir, sizeof dir);
    s = open_store(dir, NULL);
    for (uint32_t i = 1; i <= 3; i++) {
        put_ok(s, i);
    }
    before = file_size(dir, SEG1);

    arm(SITE_WRITE, EIO, 10); /* 10 bytes are really written, then the next write fails */
    CHECK(put_logged(s, 4, log, sizeof log) == -1 && errno == EIO);
    CHECK(strstr(log, "ERROR store append_failed errno=5 seq=4") != NULL);
    CHECK(file_size(dir, SEG1) == before);
    put_ok(s, 4);
    CHECK(count_records(dir) == 4);
    store_close(s);
    rm_tree(dir);
}

/* Even the cut-back fails (the disk went read-only under an open file): the original errno
 * is still the one returned, the handle refuses further appends without touching the disk,
 * and close + store_recover repairs the partial record. */
static void test_rollback_fails(void)
{
    char              dir[256];
    char              log[2048];
    gt_store*         s;
    gt_store*         other = NULL;
    gt_store_recovery info;
    off_t             before;
    unsigned          calls[3];

    make_tmp(dir, sizeof dir);
    s = open_store(dir, NULL);
    for (uint32_t i = 1; i <= 3; i++) {
        put_ok(s, i);
    }
    before = file_size(dir, SEG1);

    arm(SITE_WRITE, EROFS, 10);
    arm(SITE_TRUNC, EROFS, 0);
    CHECK(put_logged(s, 4, log, sizeof log) == -1 && errno == EROFS);
    CHECK(strstr(log, "ERROR store append_failed errno=30 seq=4") != NULL);
    CHECK(strstr(log, "ERROR store append_rollback_failed errno=30") != NULL);
    CHECK(file_size(dir, SEG1) == before + 10); /* the partial record is still on disk */

    memcpy(calls, g_calls, sizeof calls);
    CHECK(put(s, 4) == -1 && errno == EIO); /* the handle is poisoned... */
    CHECK(memcmp(calls, g_calls, sizeof calls) == 0); /* ...and did not touch the disk */
    CHECK(file_size(dir, SEG1) == before + 10);

    /* The old handle still holds the lock, so it has to be closed before recovering. */
    CHECK(store_recover(dir, &other, NULL) == -1 && errno == EBUSY && other == NULL);
    store_close(s);

    s = open_store(dir, &info);
    CHECK(info.has_records && info.last_seq == 3 && info.dropped_bytes == 10);
    CHECK(file_size(dir, SEG1) == before);
    put_ok(s, 4);
    CHECK(count_records(dir) == 4);
    store_close(s);
    rm_tree(dir);
}

/* ---- part A: real ENOSPC and EROFS on a 64 KB tmpfs in a private namespace -------------- */

static void write_file(const char* path, const char* text)
{
    int fd = open(path, O_WRONLY);

    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    CHECK(close(fd) == 0);
}

/* Gives this process its own user and mount namespace and mounts a small tmpfs on mnt.  Fails
 * the test (it does not skip) when the system does not allow unprivileged namespaces. */
static void enter_small_fs(const char* mnt)
{
    char  map[64];
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
        (void)fprintf(stderr,
                      "store_fault_test: cannot create a user+mount namespace (errno %d): this "
                      "test needs unprivileged user namespaces\n",
                      errno);
        exit(1);
    }
    write_file("/proc/self/setgroups", "deny");
    (void)snprintf(map, sizeof map, "0 %u 1", (unsigned)uid);
    write_file("/proc/self/uid_map", map);
    (void)snprintf(map, sizeof map, "0 %u 1", (unsigned)gid);
    write_file("/proc/self/gid_map", map);
    CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    CHECK(mount("tmpfs", mnt, "tmpfs", 0, "size=64k") == 0);
}

/* Fills every free page of the small filesystem with a file named filler. */
static void fill_fs(const char* mnt)
{
    static char page[4096];
    char        path[300];
    int         fd;

    memset(page, 'f', sizeof page);
    path_of(path, sizeof path, mnt, "filler");
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0);
    for (;;) {
        if (write(fd, page, sizeof page) < 0) {
            CHECK(errno == ENOSPC);
            break;
        }
    }
    CHECK(close(fd) == 0);
}

static void free_fs(const char* mnt)
{
    char path[300];

    path_of(path, sizeof path, mnt, "filler");
    CHECK(unlink(path) == 0);
}

/* The page holds n = page / REC whole records (170 on 4 KB pages, 682 on the Pi 5's 16 KB
 * pages) and a remainder smaller than one record, so record n + 1 straddles the page boundary:
 * the kernel writes its first bytes (16 of them, either way), then fails to get a new page.
 * That partial record must be cut off, or the retry would sit behind it. */
static void test_enospc_mid_record(const char* mnt)
{
    char              dir[300];
    char              log[2048];
    char              want[64];
    gt_store*         s;
    gt_store_recovery info;
    long              page = sysconf(_SC_PAGESIZE);
    uint32_t          n;

    CHECK(page > 0 && page % REC != 0); /* else no record straddles the boundary */
    n = (uint32_t)page / REC;
    CHECK(snprintf(want, sizeof want, "ERROR store append_failed errno=28 seq=%u", n + 1) > 0);

    path_of(dir, sizeof dir, mnt, "a");
    s = open_store(dir, NULL);
    for (uint32_t i = 1; i <= n; i++) {
        put_ok(s, i);
    }
    CHECK(file_size(dir, SEG1) == n * REC);
    fill_fs(mnt);

    g_short_writes = 0;
    CHECK(put_logged(s, n + 1, log, sizeof log) == -1 && errno == ENOSPC);
    CHECK(g_short_writes == 1); /* the partial write really happened */
    CHECK(strstr(log, want) != NULL);
    CHECK(file_size(dir, SEG1) == n * REC); /* cut back to the last good record */
    CHECK(count_records(dir) == n);

    CHECK(put(s, n + 1) == -1 && errno == ENOSPC); /* still full: fails the same way */
    CHECK(file_size(dir, SEG1) == n * REC);

    free_fs(mnt);
    put_ok(s, n + 1); /* the retry */
    CHECK(file_size(dir, SEG1) == (n + 1) * REC);
    CHECK(count_records(dir) == n + 1);
    store_close(s);

    s = open_store(dir, &info); /* nothing left for recovery to repair */
    CHECK(info.has_records && info.last_seq == n + 1 && info.dropped_bytes == 0);
    store_close(s);
    rm_tree(dir);
}

/* The first append of a new UTC day creates the new segment, then cannot write into it. */
static void test_enospc_on_new_segment(const char* mnt)
{
    char              dir[300];
    char              log[2048];
    gt_store*         s;
    gt_store_recovery info;

    path_of(dir, sizeof dir, mnt, "b");
    s = open_store(dir, NULL);
    for (uint32_t i = 1; i <= 3; i++) {
        put_ok(s, i);
    }
    fill_fs(mnt);

    capture c;
    cap_start(&c);
    errno = 0;
    CHECK(put_at(s, 4, T_DAY2 + 1000) == -1 && errno == ENOSPC);
    cap_stop(&c, log, sizeof log);
    CHECK(strstr(log, "ERROR store append_failed errno=28 seq=4") != NULL);
    CHECK(file_size(dir, SEG1) == 3 * REC);
    CHECK(file_size(dir, SEG2) == 0); /* the new, empty segment is left in place */
    CHECK(count_records(dir) == 3);

    free_fs(mnt);
    CHECK(put_at(s, 4, T_DAY2 + 1000) == 0);
    CHECK(file_size(dir, SEG2) == REC);
    CHECK(count_records(dir) == 4);
    store_close(s);

    s = open_store(dir, &info);
    CHECK(info.has_records && info.last_seq == 4 && info.dropped_bytes == 0);
    store_close(s);
    rm_tree(dir);
}

static void remount(const char* mnt, unsigned long flags)
{
    CHECK(mount(NULL, mnt, NULL, MS_REMOUNT | flags, NULL) == 0);
}

/* A read-only disk: the writer cannot start and says why (EROFS), the data stays readable,
 * and once the disk is writable again everything carries on. */
static void test_read_only(const char* mnt)
{
    char              dir[300];
    char              log[2048];
    gt_store*         s;
    gt_store*         none = NULL;
    gt_store_recovery info;
    capture           c;

    path_of(dir, sizeof dir, mnt, "c");
    s = open_store(dir, NULL);
    for (uint32_t i = 1; i <= 5; i++) {
        put_ok(s, i);
    }
    store_close(s);

    remount(mnt, MS_RDONLY);
    cap_start(&c);
    errno = 0;
    CHECK(store_recover(dir, &none, NULL) == -1 && errno == EROFS && none == NULL);
    cap_stop(&c, log, sizeof log);
    CHECK(strstr(log, "ERROR store lock open failed errno=30") != NULL);
    CHECK(count_records(dir) == 5); /* reading a read-only store still works */

    remount(mnt, 0);
    s = open_store(dir, &info);
    CHECK(info.has_records && info.last_seq == 5 && info.dropped_bytes == 0);
    put_ok(s, 6);
    CHECK(count_records(dir) == 6);
    store_close(s);
    rm_tree(dir);
}

/* Runs part A in a child process: its namespace and mount vanish when it exits. */
static void test_small_filesystem(void)
{
    char  mnt[256];
    pid_t pid;
    int   st;

    make_tmp(mnt, sizeof mnt);
    CHECK(fflush(NULL) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        enter_small_fs(mnt);
        test_enospc_mid_record(mnt);
        test_enospc_on_new_segment(mnt);
        test_read_only(mnt);
        exit(0);
    }
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(rmdir(mnt) == 0);
}

int main(void)
{
    test_errno_passes_through();
    test_short_write_then_error();
    test_rollback_fails();
    test_small_filesystem();
    (void)puts("PASS");
    return 0;
}
