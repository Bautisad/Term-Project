/* analysis_store_test: the dry-down analysis on a real store on disk (#27 step 3).  Builds about
 * three weeks of synthetic history with store_append (one record per 10 minutes, so about 23
 * daily segments), then checks analysis_run and the real gt-analyze (argv[1]) against it: the
 * cycles, the prediction, skipped records, a corrupt record, a window, an empty and a missing
 * store, and bad arguments.  Exits 1 at the first failed check. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "analyze.h"
#include "predict.h"
#include "store.h"

#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) {                                                                      \
            (void)fprintf(stderr, "analysis_store_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

#define T0       1699920000000ull /* 2023-11-14T00:00:00Z, so segments are whole days */
#define STEP_MS  600000ull        /* one record per 10 minutes */
#define REC      24
#define COMPLETE 7                /* finished dry-downs, 72 hours each, -3 counts per hour */
#define OPEN_H   40               /* and a current one this old */
#define SEG_CORRUPT "2023-11-20.log"

static char g_tool[512];
static char g_dir[256];
static uint64_t g_last_ts;
static double   g_last_y;

static void make_tmp(char* dir, size_t cap, const char* tag)
{
    const char* base = getenv("TMPDIR");
    int         n = snprintf(dir, cap, "%s/gt-an-%s-XXXXXX", base != NULL ? base : "/tmp", tag);

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
        int  n;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        n = snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        CHECK(n > 0 && (size_t)n < sizeof path);
        CHECK(unlink(path) == 0);
    }
    CHECK(closedir(dp) == 0);
    CHECK(rmdir(dir) == 0);
}

static uint64_t g_seed = 424242u;

static double noise(double amp)
{
    double u;

    g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
    u = (double)(g_seed >> 33) / 2147483648.0;
    return (u * 2.0 - 1.0) * amp;
}

static gt_store* g_store;
static uint64_t  g_t;
static uint32_t  g_seq;

static void put(double y, uint16_t flags, uint16_t raw_override, bool override)
{
    gt_record r;

    (void)memset(&r, 0, sizeof r);
    r.ts_unix_ms = g_t;
    r.seq = g_seq++;
    r.flags = flags;
    r.soil_raw = override ? raw_override : (uint16_t)lround(y + noise(6.0));
    CHECK(store_append(g_store, &r) == 0);
    g_last_ts = g_t;
    g_last_y = y;
    g_t += STEP_MS;
}

/* Three weeks of watering and drying.  Cycle 3 has a 2 hour INVALID run and a 1 hour STUCK run. */
static void build_store(const char* dir)
{
    gt_store_recovery info;
    int               k;

    CHECK(store_recover(dir, &g_store, &info) == 0);
    g_t = T0;
    g_seq = 1;
    for (k = 0; k <= COMPLETE; k++) {
        int steps = (k < COMPLETE) ? 432 : OPEN_H * 6; /* 72 h or 40 h at 6 per hour */

        if (k > 0) { /* watering: three steps back up to 900 */
            double from = 900.0 - 0.5 * 431.0;

            for (int j = 1; j <= 3; j++) {
                put(from + (900.0 - from) * (double)j / 3.0, 0, 0, false);
            }
        }
        for (int s = 0; s < steps; s++) {
            double y = 900.0 - 0.5 * (double)s;

            if (k == 2 && s >= 100 && s < 106) {
                put(y, GT_REC_SOIL_STUCK, 999, true);
            } else if (k == 2 && s >= 200 && s < 212) {
                put(y, GT_REC_SOIL_INVALID, 0, true);
            } else {
                put(y, 0, 0, false);
            }
        }
    }
    store_close(g_store);
    g_store = NULL;
}

static void flip_byte(const char* dir, const char* name, off_t at)
{
    char    path[512];
    uint8_t b;
    int     fd;

    CHECK(snprintf(path, sizeof path, "%s/%s", dir, name) < (int)sizeof path);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    CHECK(pread(fd, &b, 1, at) == 1);
    b = (uint8_t)(b ^ 0xFFu);
    CHECK(pwrite(fd, &b, 1, at) == 1 && close(fd) == 0);
}

/* ---- the library ---------------------------------------------------------------------- */

static void test_library(void)
{
    dry_result     r;
    predict_params pp;
    predict_result o;
    double         truth = (900.0 - 0.5 * (OPEN_H * 6 - 1) - 565.0) / 3.0;

    CHECK(analysis_run(g_dir, 0, UINT64_MAX, NULL, &r) == 0);
    CHECK(r.cycles_total == COMPLETE + 1u && r.n_cycles == COMPLETE + 1u);
    for (size_t i = 0; i < r.n_cycles; i++) {
        CHECK(r.cycles[i].reject == DRY_OK);
        CHECK(r.cycles[i].complete == (i < COMPLETE));
        CHECK(fabs(r.cycles[i].slope_per_h - (-3.0)) < 0.1);
    }
    CHECK(r.skipped_invalid == 12u && r.skipped_stuck == 6u && r.skipped_corrupt == 0u);
    CHECK(r.skipped_time_unset == 0u && r.out_of_order == 0u);
    CHECK(r.have_last && r.last_ts_ms == g_last_ts);

    predict_params_default(&pp, 565.0);
    CHECK(predict_time_until_dry(&r, &pp, g_last_ts + STEP_MS, &o) == 0);
    CHECK(o.status == PREDICT_OK && o.have_cycles == COMPLETE);
    CHECK(fabs(o.hours - truth) < 0.05 * truth);
    CHECK(o.hours_fast <= o.hours && o.hours <= o.hours_slow);

    /* Only the last 10 days: fewer cycles, so the same plant is not ready to predict. */
    CHECK(analysis_run(g_dir, g_last_ts - 10ull * 86400000ull, UINT64_MAX, NULL, &r) == 0);
    CHECK(r.cycles_total >= 3u && r.cycles_total < COMPLETE + 1u);
    CHECK(predict_time_until_dry(&r, &pp, g_last_ts + STEP_MS, &o) == 0);
    CHECK(o.have_cycles <= 4u);

    /* A range that ends early ignores the later records. */
    CHECK(analysis_run(g_dir, 0, T0 + 5ull * 86400000ull, NULL, &r) == 0);
    CHECK(r.have_last && r.last_ts_ms < T0 + 5ull * 86400000ull);
    CHECK(r.cycles_total == 2u); /* one finished 72 h cycle, then the open one: 5 days is 120 hours */
}

static void test_library_errors(void)
{
    dry_result r;
    dry_params p;
    char       empty[256];
    char       missing[300];

    errno = 0;
    CHECK(analysis_run(NULL, 0, UINT64_MAX, NULL, &r) == -1 && errno == EINVAL);
    CHECK(analysis_run(g_dir, 0, UINT64_MAX, NULL, NULL) == -1 && errno == EINVAL);
    CHECK(analysis_run(g_dir, 5, 5, NULL, &r) == -1 && errno == EINVAL);
    dry_params_default(&p);
    p.bucket_ms = 1;
    CHECK(analysis_run(g_dir, 0, UINT64_MAX, &p, &r) == -1 && errno == EINVAL);

    /* More buckets than the limit allows: an error, never a partial answer. */
    dry_params_default(&p);
    p.max_buckets = 100;
    CHECK(analysis_run(g_dir, 0, UINT64_MAX, &p, &r) == -1 && errno == EOVERFLOW);
    /* 3285 records, 18 of them skipped (INVALID or STUCK), one bucket each: 3267 buckets. */
    p.max_buckets = 3267;
    CHECK(analysis_run(g_dir, 0, UINT64_MAX, &p, &r) == 0 && r.buckets == 3267u);
    p.max_buckets = 3266;
    CHECK(analysis_run(g_dir, 0, UINT64_MAX, &p, &r) == -1 && errno == EOVERFLOW);

    (void)snprintf(missing, sizeof missing, "%s/nope", g_dir);
    CHECK(analysis_run(missing, 0, UINT64_MAX, NULL, &r) == -1 && errno == ENOENT);

    make_tmp(empty, sizeof empty, "empty");
    (void)memset(&r, 0x55, sizeof r);
    CHECK(analysis_run(empty, 0, UINT64_MAX, NULL, &r) == 0);
    CHECK(r.n_cycles == 0u && r.buckets == 0u && !r.have_last && r.records == 0u);
    rm_tree(empty);
}

/* ---- the tool ------------------------------------------------------------------------- */

/* Runs `gt-analyze args` with stdout and stderr together; returns the exit code. */
static int tool(const char* args, char* out, size_t cap)
{
    char   cmd[1024];
    FILE*  f;
    size_t n;
    int    st;

    CHECK(snprintf(cmd, sizeof cmd, "%s %s 2>&1", g_tool, args) < (int)sizeof cmd);
    f = popen(cmd, "r");
    CHECK(f != NULL);
    n = fread(out, 1, cap - 1u, f);
    out[n] = '\0';
    st = pclose(f);
    CHECK(st != -1 && WIFEXITED(st));
    return WEXITSTATUS(st);
}

static void test_tool(void)
{
    char   out[8192];
    char   args[512];
    double hours = 0.0;
    char*  p;
    double truth = (900.0 - 0.5 * (OPEN_H * 6 - 1) - 565.0) / 3.0;
    double truth700 = (900.0 - 0.5 * (OPEN_H * 6 - 1) - 700.0) / 3.0;

    /* The whole history, with the clock just after the last record. */
    (void)snprintf(args, sizeof args, "--dir %s --days 60 --now %llu", g_dir,
                   (unsigned long long)(g_last_ts + STEP_MS));
    CHECK(tool(args, out, sizeof out) == 0);
    CHECK(strstr(out, "cycles: 8 found, 8 shown") != NULL);
    CHECK(strstr(out, "skipped_invalid=12 skipped_stuck=6 skipped_time_unset=0 skipped_corrupt=0") != NULL);
    CHECK(strstr(out, "complete=yes verdict=ok slope_per_h=-3.0") != NULL ||
          strstr(out, "complete=yes verdict=ok slope_per_h=-2.9") != NULL);
    CHECK(strstr(out, "complete=no verdict=ok") != NULL);
    CHECK(strstr(out, "dry threshold: 565 (draft, not calibrated)") != NULL);
    p = strstr(out, "\nprediction hours=");
    CHECK(p != NULL && sscanf(p, "\nprediction hours=%lf", &hours) == 1);
    CHECK(fabs(hours - truth) < 0.05 * truth);
    CHECK(strstr(out, " cycles=7 ") != NULL);

    /* A different threshold changes the answer. */
    (void)snprintf(args, sizeof args, "--dir %s --dry 700 --days 60 --now %llu", g_dir,
                   (unsigned long long)(g_last_ts + STEP_MS));
    CHECK(tool(args, out, sizeof out) == 0);
    CHECK(strstr(out, "dry threshold: 700 (draft, not calibrated)") != NULL);
    p = strstr(out, "\nprediction hours=");
    CHECK(p != NULL && sscanf(p, "\nprediction hours=%lf", &hours) == 1);
    CHECK(fabs(hours - truth700) < 0.12 * truth700); /* the last sample carries +-6 counts */

    /* Five days of history hold one usable finished cycle: withheld, with the reason. */
    (void)snprintf(args, sizeof args, "--dir %s --days 5 --now %llu", g_dir,
                   (unsigned long long)(g_last_ts + STEP_MS));
    CHECK(tool(args, out, sizeof out) == 0);
    CHECK(strstr(out, "prediction_withheld reason=insufficient_cycles have=1 need=3") != NULL);

    /* The clock two hours past the last record: the data is too old to count down from. */
    (void)snprintf(args, sizeof args, "--dir %s --days 60 --now %llu", g_dir,
                   (unsigned long long)(g_last_ts + 2ull * 3600000ull));
    CHECK(tool(args, out, sizeof out) == 0);
    CHECK(strstr(out, "prediction_withheld reason=no_recent_data have=7 need=3") != NULL);

    /* A missing store is an error with the errno in the message. */
    (void)snprintf(args, sizeof args, "--dir %s/nope --now 1700000000000", g_dir);
    CHECK(tool(args, out, sizeof out) == 1);
    CHECK(strstr(out, "ERROR analysis read_failed") != NULL && strstr(out, "errno=2") != NULL);

    /* Bad arguments: exit 2 and the usage text. */
    CHECK(tool("", out, sizeof out) == 2 && strstr(out, "usage: gt-analyze") != NULL);
    (void)snprintf(args, sizeof args, "--dir %s --dry 70000", g_dir);
    CHECK(tool(args, out, sizeof out) == 2);
    (void)snprintf(args, sizeof args, "--dir %s --dry -1", g_dir);
    CHECK(tool(args, out, sizeof out) == 2);
    (void)snprintf(args, sizeof args, "--dir %s --days 0", g_dir);
    CHECK(tool(args, out, sizeof out) == 2);
    (void)snprintf(args, sizeof args, "--dir %s --days 3651", g_dir);
    CHECK(tool(args, out, sizeof out) == 2);
    (void)snprintf(args, sizeof args, "--dir %s --now 0", g_dir);
    CHECK(tool(args, out, sizeof out) == 2);
    (void)snprintf(args, sizeof args, "--dir %s --bogus 1", g_dir);
    CHECK(tool(args, out, sizeof out) == 2);
    (void)snprintf(args, sizeof args, "--dir %s --dry", g_dir);
    CHECK(tool(args, out, sizeof out) == 2);
    CHECK(tool("--help", out, sizeof out) == 0 && strstr(out, "usage: gt-analyze") != NULL);
}

static void test_tool_empty_store(void)
{
    char out[4096];
    char args[512];
    char empty[256];

    make_tmp(empty, sizeof empty, "empty2");
    (void)snprintf(args, sizeof args, "--dir %s --now 1700000000000", empty);
    CHECK(tool(args, out, sizeof out) == 0);
    CHECK(strstr(out, "records: 0 used=0") != NULL && strstr(out, "latest: none") != NULL);
    CHECK(strstr(out, "cycles: 0 found, 0 shown") != NULL);
    CHECK(strstr(out, "prediction_withheld reason=no_recent_data have=0 need=3") != NULL);
    rm_tree(empty);
}

/* Last, because it damages the store: one record fails its CRC. */
static void test_corrupt(void)
{
    dry_result r;
    char       out[8192];
    char       args[512];

    flip_byte(g_dir, SEG_CORRUPT, 40 * REC + 5);
    CHECK(analysis_run(g_dir, 0, UINT64_MAX, NULL, &r) == 0);
    CHECK(r.skipped_corrupt == 1u);
    CHECK(r.cycles_total == COMPLETE + 1u && r.n_cycles == COMPLETE + 1u);
    for (size_t i = 0; i < r.n_cycles; i++) {
        CHECK(r.cycles[i].reject == DRY_OK);
    }
    (void)snprintf(args, sizeof args, "--dir %s --days 60 --now %llu", g_dir,
                   (unsigned long long)(g_last_ts + STEP_MS));
    CHECK(tool(args, out, sizeof out) == 0);
    CHECK(strstr(out, "skipped_corrupt=1") != NULL);
    CHECK(strstr(out, "\nprediction hours=") != NULL);
}

int main(int argc, char** argv)
{
    CHECK(argc == 2 && strlen(argv[1]) < sizeof g_tool);
    CHECK(access(argv[1], X_OK) == 0);
    (void)strcpy(g_tool, argv[1]);

    make_tmp(g_dir, sizeof g_dir, "store");
    build_store(g_dir);
    test_library();
    test_library_errors();
    test_tool();
    test_tool_empty_store();
    test_corrupt();
    rm_tree(g_dir);
    (void)puts("analysis_store_test: ok");
    return 0;
}
