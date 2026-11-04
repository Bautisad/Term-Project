/* csvexport_test: runs the real gt-csv as a child process against stores built with
 * store_append: every column and flag, flagged readings kept, ranges, a store that spans two
 * days, a corrupt record, an empty and a missing store, a full stdout and bad arguments.
 * argv[1] is gt-csv. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* 1700000000000 is 2023-11-14 22:13:20 UTC. */
#define T_BASE 1700000000000ull
#define DAY_MS 86400000ull
#define SEG1   "2023-11-14.log"
#define REC    24

#define HEADER                                                                             \
    "time_utc,unix_ms,seq,soil_raw,soil_valid,soil_stuck,light,light_valid,"               \
    "light_bright_ms,light_changes,clock_unset,undervoltage\n"

static const char* g_csv;

static void path_of(char* out, size_t cap, const char* dir, const char* name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);

    CHECK(n > 0 && (size_t)n < cap);
}

static void make_tmp(char* dir, size_t cap)
{
    const char* base = getenv("TMPDIR");
    int         n = snprintf(dir, cap, "%s/gt-csv-XXXXXX", base != NULL ? base : "/tmp");

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

/* Starts gt-csv with stdout and stderr appended to the given files; returns its exit code,
 * or 128 + signal if it was killed. */
static int run_to(const char* out_path, const char* err_path, char* const args[])
{
    char*  argv[16];
    size_t n = 0;
    pid_t  pid;
    int    st;

    argv[n++] = (char*)g_csv;
    for (size_t i = 0; args[i] != NULL; i++) {
        CHECK(n < 15);
        argv[n++] = args[i];
    }
    argv[n] = NULL;
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int out = open(out_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        int err = open(err_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (out < 0 || err < 0 || dup2(out, STDOUT_FILENO) < 0 || dup2(err, STDERR_FILENO) < 0) {
            _exit(127);
        }
        (void)close(out);
        (void)close(err);
        execv(g_csv, argv);
        _exit(127);
    }
    CHECK(waitpid(pid, &st, 0) == pid);
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    CHECK(WIFSIGNALED(st));
    return 128 + WTERMSIG(st);
}

static void slurp(const char* path, char* buf, size_t cap)
{
    FILE*  f = fopen(path, "r");
    size_t n;

    CHECK(f != NULL);
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    CHECK(fclose(f) == 0);
}

/* A scratch dir holding the store at root/store. */
typedef struct env {
    char root[256];
    char store[300];
    char out[300];
    char err[300];
} env;

static void env_init(env* e)
{
    make_tmp(e->root, sizeof e->root);
    path_of(e->store, sizeof e->store, e->root, "store");
    path_of(e->out, sizeof e->out, e->root, "csv.out");
    path_of(e->err, sizeof e->err, e->root, "csv.err");
}

/* Runs gt-csv on the store; stdout goes to out and stderr to err (both cleared first). */
static int export_store(const env* e, char* const extra[], char* out, size_t out_cap, char* err,
                        size_t err_cap)
{
    char*  args[12];
    size_t n = 0;
    int    rc;

    args[n++] = "--dir";
    args[n++] = (char*)e->store;
    for (size_t i = 0; extra != NULL && extra[i] != NULL; i++) {
        CHECK(n < 10);
        args[n++] = extra[i];
    }
    args[n] = NULL;
    CHECK(unlink(e->out) == 0 || errno == ENOENT);
    CHECK(unlink(e->err) == 0 || errno == ENOENT);
    rc = run_to(e->out, e->err, args);
    slurp(e->out, out, out_cap);
    slurp(e->err, err, err_cap);
    return rc;
}

static gt_record rec(uint64_t ts, uint32_t seq, uint16_t soil, uint16_t flags, uint16_t bright_ms,
                     uint16_t edges)
{
    gt_record r;

    memset(&r, 0, sizeof r);
    r.ts_unix_ms = ts;
    r.seq = seq;
    r.soil_raw = soil;
    r.flags = flags;
    r.light_bright_ms = bright_ms;
    r.light_edges = edges;
    return r;
}

static void build(const char* dir, const gt_record* recs, size_t n)
{
    gt_store* s = NULL;

    CHECK(store_recover(dir, &s, NULL) == 0);
    for (size_t i = 0; i < n; i++) {
        CHECK(store_append(s, &recs[i]) == 0);
    }
    store_close(s);
}

static void flip(const char* dir, const char* name, off_t at)
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

static size_t count_lines(const char* s)
{
    size_t n = 0;

    for (; *s != '\0'; s++) {
        n += (*s == '\n');
    }
    return n;
}

/* Every column, and every flag on its own: one record each, plus a clean one. */
static void test_columns_and_flags(void)
{
    env       e;
    char      out[4096];
    char      err[1024];
    gt_record recs[] = {
        rec(T_BASE, 1, 812, GT_REC_LIGHT_BRIGHT, 45000, 3),
        rec(T_BASE + 1000, 2, 0, GT_REC_SOIL_INVALID, 0, 0),
        rec(T_BASE + 2000, 3, 790, GT_REC_SOIL_STUCK, 60000, 0),
        rec(T_BASE + 3000, 4, 801, GT_REC_LIGHT_INVALID, 0, 65535),
        rec(T_BASE + 4000, 5, 800, GT_REC_TIME_UNSET, 12, 1),
        rec(T_BASE + 5000, 6, 799, GT_REC_UNDERVOLT | GT_REC_LIGHT_BRIGHT, 1, 2),
        rec(T_BASE + 6007, 7, 65535, 0, 0, 0),
    };

    env_init(&e);
    build(e.store, recs, sizeof recs / sizeof recs[0]);
    CHECK(export_store(&e, NULL, out, sizeof out, err, sizeof err) == 0);
    CHECK(strcmp(out,
                 HEADER
                 "2023-11-14T22:13:20.000Z,1700000000000,1,812,yes,no,bright,yes,45000,3,no,no\n"
                 "2023-11-14T22:13:21.000Z,1700000001000,2,0,no,no,dark,yes,0,0,no,no\n"
                 "2023-11-14T22:13:22.000Z,1700000002000,3,790,yes,yes,dark,yes,60000,0,no,no\n"
                 "2023-11-14T22:13:23.000Z,1700000003000,4,801,yes,no,dark,no,0,65535,no,no\n"
                 "2023-11-14T22:13:24.000Z,1700000004000,5,800,yes,no,dark,yes,12,1,yes,no\n"
                 "2023-11-14T22:13:25.000Z,1700000005000,6,799,yes,no,bright,yes,1,2,no,yes\n"
                 "2023-11-14T22:13:26.007Z,1700000006007,7,65535,yes,no,dark,yes,0,0,no,no\n") ==
          0);
    CHECK(strstr(err, "rows=7 corrupt_skipped=0") != NULL);
    rm_tree(e.root);
}

/* --from is inclusive, --to exclusive, and a store that spans two days reads oldest first. */
static void test_ranges_and_days(void)
{
    env       e;
    char      out[4096];
    char      err[1024];
    char*     from_3[] = {"--from", "1700000002000", NULL};
    char*     to_3[] = {"--to", "1700000002000", NULL};
    char*     window[] = {"--from", "1700000001000", "--to", "1700000003000", NULL};
    char*     none[] = {"--from", "1800000000000", NULL};
    char*     same[] = {"--from", "1700000002000", "--to", "1700000002000", NULL};
    gt_record recs[] = {
        rec(T_BASE, 1, 500, 0, 0, 0),
        rec(T_BASE + 1000, 2, 501, 0, 0, 0),
        rec(T_BASE + 2000, 3, 502, 0, 0, 0),
        rec(T_BASE + 3000, 4, 503, 0, 0, 0),
        rec(T_BASE + DAY_MS, 5, 504, 0, 0, 0),
    };

    env_init(&e);
    build(e.store, recs, sizeof recs / sizeof recs[0]);

    CHECK(export_store(&e, NULL, out, sizeof out, err, sizeof err) == 0);
    CHECK(count_lines(out) == 6);
    CHECK(strstr(out, "2023-11-15T22:13:20.000Z,1700086400000,5,504,") != NULL);
    CHECK(strstr(out, ",4,503,") < strstr(out, ",5,504,"));

    CHECK(export_store(&e, from_3, out, sizeof out, err, sizeof err) == 0);
    CHECK(count_lines(out) == 4);
    CHECK(strstr(out, ",2,501,") == NULL && strstr(out, ",3,502,") != NULL);

    CHECK(export_store(&e, to_3, out, sizeof out, err, sizeof err) == 0);
    CHECK(count_lines(out) == 3);
    CHECK(strstr(out, ",3,502,") == NULL && strstr(out, ",2,501,") != NULL);

    CHECK(export_store(&e, window, out, sizeof out, err, sizeof err) == 0);
    CHECK(count_lines(out) == 3);
    CHECK(strstr(out, ",2,501,") != NULL && strstr(out, ",3,502,") != NULL);

    CHECK(export_store(&e, none, out, sizeof out, err, sizeof err) == 0);
    CHECK(strcmp(out, HEADER) == 0);
    CHECK(strstr(err, "rows=0") != NULL);

    CHECK(export_store(&e, same, out, sizeof out, err, sizeof err) == 0);
    CHECK(strcmp(out, HEADER) == 0);
    rm_tree(e.root);
}

/* A record that fails its CRC is skipped and reported; the rest is exported and the exit
 * status is still 0. */
static void test_corrupt_record(void)
{
    env       e;
    char      out[4096];
    char      err[1024];
    gt_record recs[5];

    for (uint32_t i = 0; i < 5; i++) {
        recs[i] = rec(T_BASE + i * 1000u, i + 1, (uint16_t)(500 + i), 0, 0, 0);
    }
    env_init(&e);
    build(e.store, recs, 5);
    flip(e.store, SEG1, 2 * REC + 5); /* record 3, in the middle */
    CHECK(export_store(&e, NULL, out, sizeof out, err, sizeof err) == 0);
    CHECK(count_lines(out) == 5); /* header + 4 rows */
    CHECK(strstr(out, ",3,502,") == NULL);
    CHECK(strstr(out, ",2,501,") != NULL && strstr(out, ",4,503,") != NULL);
    CHECK(strstr(err, "corrupt record skipped after seq=2") != NULL);
    CHECK(strstr(err, "rows=4 corrupt_skipped=1") != NULL);
    rm_tree(e.root);
}

static void test_empty_and_missing(void)
{
    env  e;
    char out[1024];
    char err[1024];
    char gone[300];

    env_init(&e);
    CHECK(mkdir(e.store, 0755) == 0);
    CHECK(export_store(&e, NULL, out, sizeof out, err, sizeof err) == 0);
    CHECK(strcmp(out, HEADER) == 0);
    CHECK(strstr(err, "rows=0 corrupt_skipped=0") != NULL);

    path_of(gone, sizeof gone, e.root, "nope");
    {
        char* args[] = {"--dir", gone, NULL};

        CHECK(unlink(e.out) == 0);
        CHECK(unlink(e.err) == 0);
        CHECK(run_to(e.out, e.err, args) == 1);
        slurp(e.err, err, sizeof err);
        CHECK(strstr(err, "cannot read the store") != NULL);
    }
    rm_tree(e.root);
}

/* A stdout that cannot take the data is an I/O error, not a silent success. */
static void test_full_stdout(void)
{
    env       e;
    char      err[1024];
    gt_record recs[3];
    char*     args[4];

    for (uint32_t i = 0; i < 3; i++) {
        recs[i] = rec(T_BASE + i * 1000u, i + 1, 500, 0, 0, 0);
    }
    env_init(&e);
    build(e.store, recs, 3);
    args[0] = "--dir";
    args[1] = e.store;
    args[2] = NULL;
    CHECK(run_to("/dev/full", e.err, args) == 1);
    slurp(e.err, err, sizeof err);
    CHECK(strstr(err, "write to stdout failed") != NULL);
    rm_tree(e.root);
}

static void test_bad_arguments(void)
{
    env   e;
    char  out[1024];
    char  err[1024];
    char* none[] = {NULL};
    char* bogus[] = {"--bogus", NULL};
    char* no_dir_value[] = {"--dir", NULL};
    char* from_text[] = {"--dir", "x", "--from", "abc", NULL};
    char* from_neg[] = {"--dir", "x", "--from", "-1", NULL};
    char* to_text[] = {"--dir", "x", "--to", "1.5", NULL};
    char* from_no_value[] = {"--dir", "x", "--from", NULL};
    char* reversed[] = {"--dir", "x", "--from", "20", "--to", "10", NULL};
    char* no_dir[] = {"--from", "10", NULL};
    char* help[] = {"--help", NULL};

    env_init(&e);
    CHECK(unlink(e.out) == 0 || errno == ENOENT);
    CHECK(run_to(e.out, e.err, none) == 2);
    CHECK(run_to(e.out, e.err, bogus) == 2);
    CHECK(run_to(e.out, e.err, no_dir_value) == 2);
    CHECK(run_to(e.out, e.err, from_text) == 2);
    CHECK(run_to(e.out, e.err, from_neg) == 2);
    CHECK(run_to(e.out, e.err, to_text) == 2);
    CHECK(run_to(e.out, e.err, from_no_value) == 2);
    CHECK(run_to(e.out, e.err, reversed) == 2);
    CHECK(run_to(e.out, e.err, no_dir) == 2);
    slurp(e.out, out, sizeof out);
    CHECK(out[0] == '\0'); /* a bad command line prints no CSV, not even the header */
    slurp(e.err, err, sizeof err);
    CHECK(strstr(err, "usage: gt-csv") != NULL);

    CHECK(run_to(e.out, e.err, help) == 0);
    slurp(e.out, out, sizeof out);
    CHECK(strstr(out, "usage: gt-csv") != NULL);
    rm_tree(e.root);
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    g_csv = argv[1];
    test_columns_and_flags();
    test_ranges_and_days();
    test_corrupt_record();
    test_empty_and_missing();
    test_full_stdout();
    test_bad_arguments();
    (void)puts("PASS");
    return 0;
}
