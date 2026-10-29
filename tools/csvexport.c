/* gt-csv: exports a store as CSV for the analysis work and for people.
 *
 *   gt-csv --dir DIR [--from MS] [--to MS]
 *
 * One row per record, oldest first, on stdout.  --from is inclusive and --to exclusive, both
 * in Unix milliseconds.  The reading goes through the store's own reader, so it shares the
 * store's CRC32 and record decoder.  Readings the sensors flagged are exported, not dropped:
 * soil_valid and light_valid say whether to trust the value (an invalid value is stored as 0).
 * A record that fails its CRC cannot be decoded; it is reported on stderr and skipped.
 *
 * Exit 0: done (skipped records are reported on stderr).  1: the store or stdout could not be
 * read or written.  2: bad arguments. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "record.h"
#include "store.h"

#define EXIT_DONE  0
#define EXIT_IO    1
#define EXIT_USAGE 2

#define MS_PER_SEC 1000u

#define HEADER                                                                             \
    "time_utc,unix_ms,seq,soil_raw,soil_valid,soil_stuck,light,light_valid,"               \
    "light_bright_ms,light_changes,clock_unset,undervoltage\n"

typedef struct export_state {
    uint64_t rows;
    uint64_t corrupt;
    uint32_t last_seq;
    bool     have_seq;
    bool     write_failed;
} export_state;

static void usage(FILE* out)
{
    (void)fprintf(out,
                  "usage: gt-csv --dir DIR [--from MS] [--to MS]\n"
                  "  --dir DIR  store directory to export (read-only; safe while a writer runs)\n"
                  "  --from MS  first time to export, Unix milliseconds, inclusive, default 0\n"
                  "  --to MS    end of the range, Unix milliseconds, exclusive, default no limit\n"
                  "CSV goes to stdout. Flagged readings are exported with soil_valid, light_valid\n"
                  "and the other flag columns set; a record that fails its CRC is skipped and\n"
                  "reported on stderr.\n"
                  "Exit 0 done, 1 read or write error, 2 bad arguments.\n");
}

static bool parse_u64(const char* s, uint64_t max, uint64_t* out)
{
    char*              end;
    unsigned long long v;

    if (s == NULL || *s == '\0' || *s == '-') {
        return false;
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* "2023-11-14T22:13:20.000Z"; false (and an empty string) for a time gmtime_r cannot show. */
static bool format_time(uint64_t ms, char* out, size_t cap)
{
    char      date[32];
    struct tm tm;
    time_t    t = (time_t)(ms / MS_PER_SEC);
    int       n;

    out[0] = '\0';
    if (gmtime_r(&t, &tm) == NULL || strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm) == 0) {
        return false;
    }
    n = snprintf(out, cap, "%s.%03uZ", date, (unsigned)(ms % MS_PER_SEC));
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static const char* yes_no(uint16_t flags, uint16_t bit)
{
    return (flags & bit) != 0u ? "yes" : "no";
}

static int visit(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    export_state* st = ctx;
    char          when[48];

    if (status == GT_STORE_REC_CORRUPT) {
        st->corrupt++;
        if (st->have_seq) {
            (void)fprintf(stderr, "csv: corrupt record skipped after seq=%u\n",
                          (unsigned)st->last_seq);
        } else {
            (void)fprintf(stderr, "csv: corrupt record skipped before the first good record\n");
        }
        return 0;
    }
    if (!format_time(rec->ts_unix_ms, when, sizeof when)) {
        (void)fprintf(stderr, "csv: seq=%u time_utc not representable ts_unix_ms=%llu\n",
                      (unsigned)rec->seq, (unsigned long long)rec->ts_unix_ms);
    }
    if (printf("%s,%llu,%u,%u,%s,%s,%s,%s,%u,%u,%s,%s\n", when,
               (unsigned long long)rec->ts_unix_ms, (unsigned)rec->seq, (unsigned)rec->soil_raw,
               (rec->flags & GT_REC_SOIL_INVALID) != 0u ? "no" : "yes",
               yes_no(rec->flags, GT_REC_SOIL_STUCK),
               (rec->flags & GT_REC_LIGHT_BRIGHT) != 0u ? "bright" : "dark",
               (rec->flags & GT_REC_LIGHT_INVALID) != 0u ? "no" : "yes",
               (unsigned)rec->light_bright_ms, (unsigned)rec->light_edges,
               yes_no(rec->flags, GT_REC_TIME_UNSET), yes_no(rec->flags, GT_REC_UNDERVOLT)) < 0) {
        (void)fprintf(stderr, "csv: write to stdout failed errno=%d (%s)\n", errno,
                      strerror(errno));
        st->write_failed = true;
        return 1;
    }
    st->rows++;
    st->last_seq = rec->seq;
    st->have_seq = true;
    return 0;
}

int main(int argc, char** argv)
{
    const char*  dir = NULL;
    uint64_t     from = 0;
    uint64_t     to = UINT64_MAX;
    export_state st;
    int          status = EXIT_DONE;

    memset(&st, 0, sizeof st);
    for (int i = 1; i < argc; i++) {
        bool has_val = i + 1 < argc;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return EXIT_DONE;
        }
        if (strcmp(argv[i], "--dir") == 0 && has_val) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--from") == 0 && has_val &&
                   parse_u64(argv[i + 1], UINT64_MAX, &from)) {
            i++;
        } else if (strcmp(argv[i], "--to") == 0 && has_val &&
                   parse_u64(argv[i + 1], UINT64_MAX, &to)) {
            i++;
        } else {
            (void)fprintf(stderr, "csv: bad argument: %s\n", argv[i]);
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (dir == NULL) {
        (void)fprintf(stderr, "csv: --dir is required\n");
        usage(stderr);
        return EXIT_USAGE;
    }
    if (from > to) {
        (void)fprintf(stderr, "csv: --from %llu is after --to %llu\n", (unsigned long long)from,
                      (unsigned long long)to);
        return EXIT_USAGE;
    }

    if (fputs(HEADER, stdout) == EOF) {
        (void)fprintf(stderr, "csv: write to stdout failed errno=%d (%s)\n", errno,
                      strerror(errno));
        return EXIT_IO;
    }
    if (store_read_range(dir, from, to, visit, &st) != 0) {
        (void)fprintf(stderr, "csv: cannot read the store dir=%s errno=%d (%s)\n", dir, errno,
                      strerror(errno));
        status = EXIT_IO;
    }
    if (st.write_failed) {
        status = EXIT_IO;
    }
    if (fflush(stdout) != 0) {
        (void)fprintf(stderr, "csv: write to stdout failed errno=%d (%s)\n", errno,
                      strerror(errno));
        status = EXIT_IO;
    }
    (void)fprintf(stderr, "csv: rows=%llu corrupt_skipped=%llu\n", (unsigned long long)st.rows,
                  (unsigned long long)st.corrupt);
    return status;
}
