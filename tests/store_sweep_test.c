/* store_sweep_test: fault-injection sweep over the store's error paths.
 *
 * For each store operation, each wrapped libc call (the test is linked with -Wl,--wrap=...), and
 * each N = 1, 2, 3, ...: make the Nth call to that function fail, run the operation, and check
 *   - it fails with the injected errno (or, for close/closedir/gmtime_r failures that the store
 *     can only log, still logs and carries on),
 *   - something was logged at ERROR or WARN level,
 *   - no file descriptor is left open,
 *   - the store is still recoverable afterwards, and a read-only operation changed nothing.
 * The sweep for a function ends when a run completes without the Nth call ever happening, so
 * every call site is visited without listing them by hand.  Every wrapped function must have
 * been made to fail at least once, so a build that bypasses a wrapper fails instead of quietly
 * testing nothing. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "store.h"

static const char* g_ctx = "";

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            (void)fprintf(stderr, "%s:%d: CHECK failed: %s  [%s]\n", __FILE__, __LINE__, #cond, \
                          g_ctx);                                                             \
            exit(1);                                                                          \
        }                                                                                     \
    } while (0)

#define T_BASE 1700000000000ull /* 2023-11-14 */
#define T_DAY2 1700006400000ull
#define T_DAY3 1700092800000ull
#define SEG1   "2023-11-14.log"
#define SEG2   "2023-11-15.log"
#define REC    24

/* ---- injection ------------------------------------------------------------------------- */

enum site {
    S_OPEN, S_OPENAT, S_FSTAT, S_PREAD, S_READDIR, S_FDOPENDIR, S_CLOSEDIR, S_MKDIR, S_FLOCK,
    S_FSYNC, S_FTRUNC, S_WRITE, S_CLOSE, S_CALLOC, S_GMTIME, S_COUNT
};

static const char* const site_name[S_COUNT] = {
    "open",  "openat", "fstat", "pread", "readdir", "fdopendir", "closedir", "mkdir",
    "flock", "fsync",  "ftruncate", "write", "close", "calloc", "gmtime_r"};

enum { M_ERR, M_ZERO }; /* fail with an errno, or (write, pread) return 0 */

typedef struct fault {
    long countdown; /* the call that makes this reach 0 fails; 0 = not armed */
    int  err;
    int  mode;
} fault;

static fault         g_f[S_COUNT];
static unsigned long g_fired[S_COUNT]; /* over the whole sweep */
static bool          g_hit;            /* a fault fired during the current run */

static void arm(int site, long nth, int err, int mode)
{
    g_f[site].countdown = nth;
    g_f[site].err = err;
    g_f[site].mode = mode;
}

static void disarm(void)
{
    memset(g_f, 0, sizeof g_f);
}

/* True when this call must fail; sets errno. */
static bool fire(int site)
{
    if (g_f[site].countdown > 0 && --g_f[site].countdown == 0) {
        g_fired[site]++;
        g_hit = true;
        errno = g_f[site].err;
        return true;
    }
    return false;
}

int            __real_open(const char* path, int flags, ...);
int            __real_openat(int dirfd, const char* path, int flags, ...);
int            __real_fstat(int fd, struct stat* st);
ssize_t        __real_pread(int fd, void* buf, size_t n, off_t off);
struct dirent* __real_readdir(DIR* d);
DIR*           __real_fdopendir(int fd);
int            __real_closedir(DIR* d);
int            __real_mkdir(const char* path, mode_t mode);
int            __real_flock(int fd, int op);
int            __real_fsync(int fd);
int            __real_ftruncate(int fd, off_t len);
ssize_t        __real_write(int fd, const void* buf, size_t n);
int            __real_close(int fd);
void*          __real_calloc(size_t n, size_t m);
struct tm*     __real_gmtime_r(const time_t* t, struct tm* out);

static mode_t mode_arg(int flags, va_list ap)
{
    return (flags & (O_CREAT | O_TMPFILE)) != 0 ? (mode_t)va_arg(ap, unsigned int) : 0;
}

int __wrap_open(const char* path, int flags, ...)
{
    va_list ap;
    mode_t  mode;

    va_start(ap, flags);
    mode = mode_arg(flags, ap);
    va_end(ap);
    return fire(S_OPEN) ? -1 : __real_open(path, flags, mode);
}

int __wrap_openat(int dirfd, const char* path, int flags, ...)
{
    va_list ap;
    mode_t  mode;

    va_start(ap, flags);
    mode = mode_arg(flags, ap);
    va_end(ap);
    return fire(S_OPENAT) ? -1 : __real_openat(dirfd, path, flags, mode);
}

int __wrap_fstat(int fd, struct stat* st)
{
    return fire(S_FSTAT) ? -1 : __real_fstat(fd, st);
}

ssize_t __wrap_pread(int fd, void* buf, size_t n, off_t off)
{
    if (fire(S_PREAD)) {
        return g_f[S_PREAD].mode == M_ZERO ? 0 : -1;
    }
    return __real_pread(fd, buf, n, off);
}

struct dirent* __wrap_readdir(DIR* d)
{
    return fire(S_READDIR) ? NULL : __real_readdir(d); /* NULL with errno set: a read error */
}

DIR* __wrap_fdopendir(int fd)
{
    return fire(S_FDOPENDIR) ? NULL : __real_fdopendir(fd);
}

/* closedir and close release the descriptor first and then report a failure, as a failing
 * close(2) does, so the test itself never leaks. */
int __wrap_closedir(DIR* d)
{
    bool f = fire(S_CLOSEDIR);
    int  r = __real_closedir(d);

    if (f) {
        errno = g_f[S_CLOSEDIR].err;
        return -1;
    }
    return r;
}

int __wrap_close(int fd)
{
    bool f = fire(S_CLOSE);
    int  r = __real_close(fd);

    if (f) {
        errno = g_f[S_CLOSE].err;
        return -1;
    }
    return r;
}

int __wrap_mkdir(const char* path, mode_t mode)
{
    return fire(S_MKDIR) ? -1 : __real_mkdir(path, mode);
}

int __wrap_flock(int fd, int op)
{
    return fire(S_FLOCK) ? -1 : __real_flock(fd, op);
}

int __wrap_fsync(int fd)
{
    return fire(S_FSYNC) ? -1 : __real_fsync(fd);
}

int __wrap_ftruncate(int fd, off_t len)
{
    return fire(S_FTRUNC) ? -1 : __real_ftruncate(fd, len);
}

ssize_t __wrap_write(int fd, const void* buf, size_t n)
{
    if (fire(S_WRITE)) {
        return g_f[S_WRITE].mode == M_ZERO ? 0 : -1;
    }
    return __real_write(fd, buf, n);
}

void* __wrap_calloc(size_t n, size_t m)
{
    return fire(S_CALLOC) ? NULL : __real_calloc(n, m);
}

struct tm* __wrap_gmtime_r(const time_t* t, struct tm* out)
{
    return fire(S_GMTIME) ? NULL : __real_gmtime_r(t, out);
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
    int         n = snprintf(dir, cap, "%s/gt-sweep-XXXXXX", base != NULL ? base : "/tmp");

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

/* Copies the regular files of src into a new directory dst. */
static void copy_tree(const char* src, const char* dst)
{
    DIR*           dp = opendir(src);
    struct dirent* ent;

    CHECK(dp != NULL);
    CHECK(mkdir(dst, 0755) == 0);
    while ((ent = readdir(dp)) != NULL) {
        char    from[512];
        char    to[512];
        char    buf[4096];
        ssize_t n;
        int     in;
        int     out;

        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || ent->d_name[1] == '.')) {
            continue;
        }
        path_of(from, sizeof from, src, ent->d_name);
        path_of(to, sizeof to, dst, ent->d_name);
        in = open(from, O_RDONLY);
        out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0644);
        CHECK(in >= 0 && out >= 0);
        while ((n = read(in, buf, sizeof buf)) > 0) {
            CHECK(write(out, buf, (size_t)n) == n);
        }
        CHECK(n == 0 && close(in) == 0 && close(out) == 0);
    }
    CHECK(closedir(dp) == 0);
}

/* A hash of every file name and content in dir; equal before and after means unchanged. */
static uint64_t dir_digest(const char* dir)
{
    DIR*           dp = opendir(dir);
    struct dirent* ent;
    uint64_t       sum = 0;

    if (dp == NULL) {
        return 0;
    }
    while ((ent = readdir(dp)) != NULL) {
        char     path[512];
        uint8_t  buf[4096];
        uint64_t h = 1469598103934665603ull;
        ssize_t  n;
        int      fd;

        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || ent->d_name[1] == '.')) {
            continue;
        }
        for (const char* p = ent->d_name; *p != '\0'; p++) {
            h = (h ^ (uint8_t)*p) * 1099511628211ull;
        }
        path_of(path, sizeof path, dir, ent->d_name);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        while ((n = read(fd, buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                h = (h ^ buf[i]) * 1099511628211ull;
            }
        }
        CHECK(n == 0 && close(fd) == 0);
        sum += h;
    }
    CHECK(closedir(dp) == 0);
    return sum;
}

static size_t count_fds(void)
{
    DIR*           dp = opendir("/proc/self/fd");
    struct dirent* ent;
    size_t         n = 0;

    CHECK(dp != NULL);
    while ((ent = readdir(dp)) != NULL) {
        n++;
    }
    CHECK(closedir(dp) == 0);
    return n;
}

typedef struct capture {
    int  fd;
    int  saved;
    char path[256];
} capture;

static void cap_start(capture* c)
{
    const char* base = getenv("TMPDIR");

    CHECK(snprintf(c->path, sizeof c->path, "%s/gt-sweep-log-XXXXXX", base != NULL ? base : "/tmp") > 0);
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

static int put_at(gt_store* s, uint32_t seq, uint64_t ts)
{
    gt_record r;

    memset(&r, 0, sizeof r);
    r.ts_unix_ms = ts;
    r.seq = seq;
    r.soil_raw = 500;
    return store_append(s, &r);
}

static gt_store* open_store(const char* dir)
{
    gt_store* s = NULL;

    CHECK(store_recover(dir, &s, NULL) == 0 && s != NULL);
    return s;
}

static int count_fn(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    uint32_t* n = ctx;

    CHECK(status == GT_STORE_REC_OK);
    CHECK(rec->seq == *n + 1);
    (*n)++;
    return 0;
}

/* Number of records in a store that must hold seq 1..n and nothing damaged. */
static uint32_t count_records(const char* dir)
{
    uint32_t n = 0;

    CHECK(store_read_range(dir, 0, UINT64_MAX, count_fn, &n) == 0);
    return n;
}

static void flip_byte(const char* dir, const char* name, off_t at)
{
    char          path[512];
    unsigned char b;
    int           fd;

    path_of(path, sizeof path, dir, name);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0 && pread(fd, &b, 1, at) == 1);
    b ^= 0x5Au;
    CHECK(pwrite(fd, &b, 1, at) == 1 && close(fd) == 0);
}

static void add_junk(const char* dir, const char* name, size_t n)
{
    char    path[512];
    uint8_t junk[64];
    int     fd;

    CHECK(n <= sizeof junk);
    memset(junk, 0xAB, sizeof junk);
    path_of(path, sizeof path, dir, name);
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, junk, n) == (ssize_t)n && close(fd) == 0);
}

/* ---- the stores the operations start from ---------------------------------------------- */

static char g_root[256];
static char g_tpl_mid[300];    /* 3 segments, a bad record in the middle of the first */
static char g_tpl_ragged[300]; /* an older segment with a ragged tail */
static char g_tpl_clean[300];  /* 2 clean segments, seq 1..6 */
static char g_tpl_torn[300];   /* newest segment with a torn tail: 7 stray bytes */
static char g_tpl_empty[300];  /* seq 1..3 in the first segment, an empty newest segment */
static char g_tpl_blank[300];  /* two segments and no record at all */
static char g_tpl_badtail[300]; /* newest segment ends in a record that fails its CRC */

static void build(const char* name, char* out, size_t cap)
{
    path_of(out, cap, g_root, name);
}

static void put_range(gt_store* s, uint32_t from, uint32_t to, uint64_t day_ts)
{
    for (uint32_t i = from; i <= to; i++) {
        CHECK(put_at(s, i, day_ts + i * 1000ull) == 0);
    }
}

static void touch_file(const char* dir, const char* name)
{
    char path[512];
    int  fd;

    path_of(path, sizeof path, dir, name);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0 && close(fd) == 0);
}

static void build_templates(void)
{
    gt_store* s;

    build("tpl-mid", g_tpl_mid, sizeof g_tpl_mid);
    s = open_store(g_tpl_mid);
    put_range(s, 1, 4, T_BASE);
    put_range(s, 5, 6, T_DAY2);
    put_range(s, 7, 8, T_DAY3);
    store_close(s);
    flip_byte(g_tpl_mid, SEG1, 2 * REC + 5); /* record 3 */

    build("tpl-ragged", g_tpl_ragged, sizeof g_tpl_ragged);
    s = open_store(g_tpl_ragged);
    put_range(s, 1, 3, T_BASE);
    put_range(s, 4, 5, T_DAY2);
    store_close(s);
    add_junk(g_tpl_ragged, SEG1, 7);

    build("tpl-clean", g_tpl_clean, sizeof g_tpl_clean);
    s = open_store(g_tpl_clean);
    put_range(s, 1, 3, T_BASE);
    put_range(s, 4, 6, T_DAY2);
    store_close(s);

    build("tpl-torn", g_tpl_torn, sizeof g_tpl_torn);
    s = open_store(g_tpl_torn);
    put_range(s, 1, 3, T_BASE);
    put_range(s, 4, 5, T_DAY2);
    store_close(s);
    add_junk(g_tpl_torn, SEG2, 7);

    build("tpl-empty", g_tpl_empty, sizeof g_tpl_empty);
    s = open_store(g_tpl_empty);
    put_range(s, 1, 3, T_BASE);
    store_close(s);
    touch_file(g_tpl_empty, SEG2); /* created, then the process died */

    build("tpl-blank", g_tpl_blank, sizeof g_tpl_blank);
    CHECK(mkdir(g_tpl_blank, 0755) == 0);
    touch_file(g_tpl_blank, SEG1);
    touch_file(g_tpl_blank, SEG2);

    build("tpl-badtail", g_tpl_badtail, sizeof g_tpl_badtail);
    s = open_store(g_tpl_badtail);
    put_range(s, 1, 3, T_BASE);
    put_range(s, 4, 5, T_DAY2);
    store_close(s);
    flip_byte(g_tpl_badtail, SEG2, REC + 5); /* record 5, the last one */
}

/* ---- the operations -------------------------------------------------------------------- */

static int visit_nop(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    (void)rec;
    (void)status;
    (void)ctx;
    return 0;
}

static int run_read(const char* dir)
{
    return store_read_range(dir, 0, UINT64_MAX, visit_nop, NULL);
}

static int run_recover(const char* dir)
{
    gt_store* s = NULL;

    if (store_recover(dir, &s, NULL) != 0) {
        return -1;
    }
    store_close(s);
    return 0;
}

/* Recover, append the first record of a new UTC day (creates a segment), close. */
static int run_append_new_day(const char* dir)
{
    gt_store* s = NULL;
    int       err;

    if (store_recover(dir, &s, NULL) != 0) {
        return -1;
    }
    if (put_at(s, 7, T_DAY3 + 7000) != 0) {
        err = errno;
        store_close(s);
        errno = err;
        return -1;
    }
    store_close(s);
    return 0;
}

static void post_read(const char* dir, uint64_t digest0)
{
    CHECK(dir_digest(dir) == digest0); /* reading never changes a file */
}

/* A fault-free recovery must work, whatever the failed one left behind. */
static void post_recover(const char* dir, uint32_t want)
{
    gt_store* s = NULL;

    CHECK(store_recover(dir, &s, NULL) == 0 && s != NULL);
    store_close(s);
    CHECK(count_records(dir) == want);
}

static void post_append_new_day(const char* dir)
{
    gt_store* s = NULL;
    uint32_t  n;

    CHECK(store_recover(dir, &s, NULL) == 0 && s != NULL);
    n = count_records(dir);
    CHECK(n == 6 || n == 7); /* the failed append left no record, or the run completed */
    if (n == 6) {
        CHECK(put_at(s, 7, T_DAY3 + 7000) == 0);
    }
    store_close(s);
    CHECK(count_records(dir) == 7);
}

typedef struct op {
    const char* name;
    const char* tpl;                    /* template directory to copy, NULL for a missing dir */
    int (*run)(const char* dir);
    void (*post)(const char* dir, uint64_t digest0);
} op;

static void post_recover_clean(const char* dir, uint64_t d)  { (void)d; post_recover(dir, 6); }
static void post_recover_torn(const char* dir, uint64_t d)   { (void)d; post_recover(dir, 5); }
static void post_recover_new(const char* dir, uint64_t d)    { (void)d; post_recover(dir, 0); }
static void post_append(const char* dir, uint64_t d)         { (void)d; post_append_new_day(dir); }
static void post_recover_empty(const char* dir, uint64_t d)  { (void)d; post_recover(dir, 3); }
static void post_recover_blank(const char* dir, uint64_t d)  { (void)d; post_recover(dir, 0); }
static void post_recover_badtail(const char* dir, uint64_t d) { (void)d; post_recover(dir, 4); }

/* ---- the sweep ------------------------------------------------------------------------- */

typedef struct variant {
    const char* name;
    int         err;
    int         mode;
    bool        succeeds; /* the store retries it, so the operation still succeeds */
} variant;

static const variant v_err = {"EIO", EIO, M_ERR, false};
static const variant v_zero = {"zero-length", EIO, M_ZERO, false};
static const variant v_eintr = {"EINTR", EINTR, M_ERR, true};
static const variant v_nomem = {"ENOMEM", ENOMEM, M_ERR, false};

static unsigned long g_runs;

static void run_one(const op* o, int site, long nth, const variant* v, bool* fired)
{
    char     work[300];
    char     log[16384];
    char     ctx[160];
    capture  c;
    uint64_t digest0;
    size_t   fds0;
    int      rc;
    int      err;

    (void)snprintf(ctx, sizeof ctx, "%s: %s call #%ld fails with %s", o->name, site_name[site], nth,
                   v->name);
    g_ctx = ctx;
    build("work", work, sizeof work);
    if (o->tpl != NULL) {
        copy_tree(o->tpl, work);
    }
    digest0 = dir_digest(work);
    fds0 = count_fds();

    g_hit = false;
    cap_start(&c);
    arm(site, nth, v->err, v->mode);
    errno = 0;
    rc = o->run(work);
    err = errno;
    disarm();
    cap_stop(&c, log, sizeof log);

    CHECK(count_fds() == fds0); /* no orphaned descriptor */
    *fired = g_hit;
    g_runs++;
    if (!g_hit) {
        CHECK(rc == 0); /* the run reached the end without ever making the Nth call */
    } else if (v->succeeds) {
        CHECK(rc == 0); /* an interrupted call is simply retried */
    } else if (site == S_CLOSE || site == S_CLOSEDIR || site == S_GMTIME) {
        /* A failing close of a read-only descriptor cannot change the outcome; the store logs
         * it.  closedir failures end a directory scan, so they do fail the operation. */
        CHECK(site == S_CLOSEDIR ? (rc == -1 && err == v->err) : rc == 0);
    } else {
        CHECK(rc == -1 && err == v->err); /* the error is passed up unchanged, never swallowed */
    }
    if (g_hit && !v->succeeds) {
        if (site == S_GMTIME) {
            CHECK(strstr(log, "- INFO ") || strstr(log, "- WARN ") || strstr(log, "- ERROR "));
        } else {
            CHECK(strstr(log, " ERROR ") != NULL || strstr(log, " WARN ") != NULL);
        }
    }
    o->post(work, digest0);
    if (o->tpl != NULL || access(work, F_OK) == 0) {
        rm_tree(work);
    }
    g_ctx = "";
}

static void sweep(const op* o, int site, const variant* v)
{
    bool fired = true;

    for (long nth = 1; nth <= 200 && fired; nth++) {
        run_one(o, site, nth, v, &fired);
    }
    CHECK(!fired); /* the sweep must end because the calls ran out, not at the cap */
}

int main(void)
{
    op ops[9];
    unsigned long fired_before;
    size_t n_ops = 0;

    make_tmp(g_root, sizeof g_root);
    build_templates();

    ops[n_ops++] = (op){"read_range(3 segments, bad record)", g_tpl_mid, run_read, post_read};
    ops[n_ops++] = (op){"read_range(ragged older segment)", g_tpl_ragged, run_read, post_read};
    ops[n_ops++] = (op){"recover(clean)", g_tpl_clean, run_recover, post_recover_clean};
    ops[n_ops++] = (op){"recover(torn tail)", g_tpl_torn, run_recover, post_recover_torn};
    ops[n_ops++] = (op){"recover(new directory)", NULL, run_recover, post_recover_new};
    ops[n_ops++] = (op){"recover+append(new day)", g_tpl_clean, run_append_new_day, post_append};
    ops[n_ops++] = (op){"recover(empty newest segment)", g_tpl_empty, run_recover, post_recover_empty};
    ops[n_ops++] = (op){"recover(only empty segments)", g_tpl_blank, run_recover, post_recover_blank};
    ops[n_ops++] = (op){"recover(bad last record)", g_tpl_badtail, run_recover, post_recover_badtail};

    for (size_t i = 0; i < n_ops; i++) {
        for (int site = 0; site < S_COUNT; site++) {
            sweep(&ops[i], site, site == S_CALLOC ? &v_nomem : &v_err);
            if (site == S_WRITE || site == S_PREAD) {
                sweep(&ops[i], site, &v_zero);
                sweep(&ops[i], site, &v_eintr);
            }
        }
    }

    fired_before = 0;
    for (int site = 0; site < S_COUNT; site++) {
        CHECK(g_fired[site] > 0); /* a wrapper that never fired tested nothing */
        fired_before += g_fired[site];
        (void)printf("  %-10s made to fail %lu times\n", site_name[site], g_fired[site]);
    }
    (void)printf("store_sweep: %lu runs, %lu injected failures, %zu operations, %d call sites\n",
                 g_runs, fired_before, n_ops, (int)S_COUNT);
    rm_tree(g_root);
    (void)puts("PASS");
    return 0;
}
