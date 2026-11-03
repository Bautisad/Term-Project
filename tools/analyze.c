/* gt-analyze: runs the dry-down analysis (#27) on a store and prints what it found.
 *
 *   gt-analyze --dir DIR [--dry N] [--days N] [--now MS]
 *
 * Reads the last --days days of the store (read-only, safe while a writer runs), finds the
 * dry-down cycles, and says how long until the soil reaches the dry threshold, or why it will
 * not say.  The threshold is NOT calibrated (S1 was descoped): --dry sets it and the report
 * always says so.  --now exists so a test or a replay of old data can pick the clock.
 *
 * Exit 0: done (a withheld prediction is a result, not a failure).  1: the store could not be
 * read.  2: bad arguments. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "analyze.h"
#include "predict.h"

#define EXIT_DONE  0
#define EXIT_IO    1
#define EXIT_USAGE 2

#define MS_PER_DAY  86400000ull
#define DRY_DEFAULT 565u /* README: "very dry soil" read about 565.  A draft, not calibrated. */
#define DAYS_DEFAULT 28u
#define DAYS_MAX     3650u

static void usage(FILE* out)
{
    (void)fprintf(out,
                  "usage: gt-analyze --dir DIR [--dry N] [--days N] [--now MS]\n"
                  "  --dir DIR   store directory (read-only; safe while a writer runs)\n"
                  "  --dry N     raw soil value at or below which the soil counts as dry, 0-65535,\n"
                  "              default %u. Not calibrated: S1 was descoped.\n"
                  "  --days N    how much history to read, 1-%u, default %u\n"
                  "  --now MS    the clock, Unix milliseconds, default the real time\n"
                  "Exit 0 done (a withheld prediction is a result), 1 store error, 2 bad arguments.\n",
                  DRY_DEFAULT, DAYS_MAX, DAYS_DEFAULT);
}

static bool parse_u64(const char* s, uint64_t min, uint64_t max, uint64_t* out)
{
    char*              end;
    unsigned long long v;

    if (s == NULL || *s == '\0' || *s == '-') {
        return false;
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* "2023-11-14T22:13:20Z"; "-" for a time gmtime_r cannot show. */
static void format_time(uint64_t ms, char* out, size_t cap)
{
    time_t    t = (time_t)(ms / 1000u);
    struct tm tm;

    if (ms / 1000u > (uint64_t)INT32_MAX * 100u || gmtime_r(&t, &tm) == NULL ||
        strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)snprintf(out, cap, "-");
    }
}

int main(int argc, char** argv)
{
    const char* dir = NULL;
    uint64_t    dry = DRY_DEFAULT;
    uint64_t    days = DAYS_DEFAULT;
    uint64_t    now = 0;
    bool        have_now = false;
    uint64_t    from;
    dry_result  r;
    predict_params pp;
    predict_result pr;
    char        a[40];
    char        b[40];
    char        line[256];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return EXIT_DONE;
        }
        if (i + 1 >= argc) {
            usage(stderr);
            return EXIT_USAGE;
        }
        if (strcmp(argv[i], "--dir") == 0) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--dry") == 0 && parse_u64(argv[i + 1], 0, 65535u, &dry)) {
            i++;
        } else if (strcmp(argv[i], "--days") == 0 && parse_u64(argv[i + 1], 1, DAYS_MAX, &days)) {
            i++;
        } else if (strcmp(argv[i], "--now") == 0 && parse_u64(argv[i + 1], 1, UINT64_MAX / 2u, &now)) {
            have_now = true;
            i++;
        } else {
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (dir == NULL) {
        usage(stderr);
        return EXIT_USAGE;
    }
    if (!have_now) {
        now = (uint64_t)time(NULL) * 1000u;
    }
    from = (now > days * MS_PER_DAY) ? now - days * MS_PER_DAY : 0u;

    if (analysis_run(dir, from, UINT64_MAX, NULL, &r) != 0) {
        (void)fprintf(stderr, "ERROR analysis read_failed dir=%s errno=%d\n", dir, errno);
        return EXIT_IO;
    }

    format_time(from, a, sizeof a);
    (void)printf("window: from %s, %llu days\n", a, (unsigned long long)days);
    (void)printf("records: %llu used=%llu skipped_invalid=%llu skipped_stuck=%llu skipped_time_unset=%llu "
                 "skipped_corrupt=%llu out_of_order=%llu\n",
                 (unsigned long long)r.records, (unsigned long long)r.used,
                 (unsigned long long)r.skipped_invalid, (unsigned long long)r.skipped_stuck,
                 (unsigned long long)r.skipped_time_unset, (unsigned long long)r.skipped_corrupt,
                 (unsigned long long)r.out_of_order);
    (void)printf("buckets: %zu\n", r.buckets);
    (void)printf("cycles: %zu found, %zu shown\n", r.cycles_total, r.n_cycles);
    for (size_t i = 0; i < r.n_cycles; i++) {
        const dry_cycle* c = &r.cycles[i];

        format_time(c->start_ms, a, sizeof a);
        format_time(c->end_ms, b, sizeof b);
        (void)printf("  cycle start=%s end=%s hours=%.1f complete=%s verdict=%s slope_per_h=%.2f r2=%.3f buckets=%zu\n",
                     a, b, (double)(c->end_ms - c->start_ms) / 3600000.0, c->complete ? "yes" : "no",
                     dry_reject_name(c->reject), c->slope_per_h, c->r2, c->buckets);
    }
    if (r.have_last) {
        format_time(r.last_ts_ms, a, sizeof a);
        (void)printf("latest: %s raw=%.0f\n", a, r.last_raw);
    } else {
        (void)printf("latest: none\n");
    }
    (void)printf("dry threshold: %llu (draft, not calibrated)\n", (unsigned long long)dry);

    predict_params_default(&pp, (double)dry);
    if (predict_time_until_dry(&r, &pp, now, &pr) != 0) {
        (void)fprintf(stderr, "ERROR analysis bad_parameters errno=%d\n", errno);
        return EXIT_USAGE;
    }
    (void)predict_describe(&pr, line, sizeof line);
    (void)printf("%s\n", line);
    return (fflush(stdout) == 0 && ferror(stdout) == 0) ? EXIT_DONE : EXIT_IO;
}
