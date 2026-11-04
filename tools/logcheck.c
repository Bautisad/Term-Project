/* gt-logcheck: decides whether a store survived a crash (DESIGN test P1).
 *
 *   gt-logcheck --dir DIR [--writer-log FILE] [--restarts N] [--progress FILE]
 *
 * The log itself (always): every record passes its CRC, and sequence numbers rise by one,
 * except that a jump skipping exactly one number is tolerated and counted as a lost record.
 * The writer's captured stderr (--writer-log): a "store recovered" entry for every start,
 * none that dropped more than one record, and no ERROR lines.  The progress file written
 * by gt-feeder (--progress): the log still reaches the last seq the store acknowledged.
 * That last check is only meaningful while the writer is stopped, before it is restarted.
 *
 * Prints one summary line, then PASS or one FAIL line per failed check.
 * Exit 0: pass.  1: fail.  2: bad arguments. */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "record.h"
#include "store.h"

#define EXIT_PASS  0
#define EXIT_FAIL  1
#define EXIT_USAGE 2

#define MAX_REASONS 16
#define WRITER_LINE_MAX   8192u

/* Recovery may cut one torn record and nothing more. */
#define MAX_DROPPED_BYTES GT_RECORD_SIZE

typedef struct seqcheck {
    uint64_t records;
    uint64_t corrupt;
    uint64_t lost;      /* jumps that skipped exactly one number */
    uint64_t big_gaps;  /* jumps that skipped more */
    uint64_t backwards; /* repeated or smaller seq */
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t bad_at;    /* seq before the first big gap or backwards step */
    bool     have;
} seqcheck;

typedef struct writerlog {
    uint64_t recovered;
    uint64_t max_dropped;
    uint64_t errors;
    uint64_t malformed; /* "recovered" lines with no readable dropped_bytes */
} writerlog;

typedef struct verdict {
    char reasons[MAX_REASONS][160];
    int  n;
} verdict;

__attribute__((format(printf, 2, 3)))
static void fail(verdict* v, const char* fmt, ...)
{
    va_list ap;

    if (v->n >= MAX_REASONS) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(v->reasons[v->n], sizeof v->reasons[v->n], fmt, ap);
    va_end(ap);
    v->n++;
}

static void usage(FILE* out)
{
    (void)fprintf(out,
                  "usage: gt-logcheck --dir DIR [--writer-log FILE] [--restarts N] [--progress FILE]\n"
                  "  --dir DIR          store directory to check (read-only; safe while a writer runs)\n"
                  "  --writer-log FILE  the writer's stderr: needs a 'store recovered' entry per start,\n"
                  "                     none that dropped more than %u bytes, and no ERROR lines\n"
                  "  --restarts N       with --writer-log: expect exactly N+1 recovered entries\n"
                  "  --progress FILE    gt-feeder --progress file; the log must reach that seq.\n"
                  "                     Run it with the writer stopped, before it is restarted.\n"
                  "Exit 0 pass, 1 fail, 2 bad arguments.\n",
                  (unsigned)MAX_DROPPED_BYTES);
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

static int visit(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    seqcheck* c = ctx;
    uint32_t  seq = rec->seq;

    if (status == GT_STORE_REC_CORRUPT) {
        c->corrupt++;
        return 0;
    }
    if (!c->have) {
        c->have = true;
        c->first_seq = seq;
    } else if (seq <= c->last_seq) {
        if (c->backwards++ == 0 && c->big_gaps == 0) {
            c->bad_at = c->last_seq;
        }
    } else if (seq - c->last_seq == 2) {
        c->lost++;
    } else if (seq - c->last_seq > 2) {
        if (c->big_gaps++ == 0 && c->backwards == 0) {
            c->bad_at = c->last_seq;
        }
    }
    c->last_seq = seq;
    c->records++;
    return 0;
}

/* Reads the writer's stderr line by line.  A line too long for the buffer is cut short. */
static int scan_writer_log(const char* path, writerlog* w)
{
    char  line[WRITER_LINE_MAX];
    FILE* f = fopen(path, "r");

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        const char* rec;

        if (strchr(line, '\n') == NULL && !feof(f)) {
            int ch;

            while ((ch = getc(f)) != EOF && ch != '\n') {
            }
        }
        if (strstr(line, " ERROR ") != NULL) {
            w->errors++;
        }
        rec = strstr(line, " store recovered ");
        if (rec != NULL) {
            const char* d = strstr(rec, " dropped_bytes=");
            uint64_t    n = 0;
            char        digits[24];
            size_t      len;

            w->recovered++;
            if (d == NULL) {
                w->malformed++;
                continue;
            }
            d += strlen(" dropped_bytes=");
            len = strspn(d, "0123456789");
            if (len == 0 || len >= sizeof digits) {
                w->malformed++;
                continue;
            }
            memcpy(digits, d, len);
            digits[len] = '\0';
            if (!parse_u64(digits, UINT64_MAX, &n)) {
                w->malformed++;
                continue;
            }
            if (n > w->max_dropped) {
                w->max_dropped = n;
            }
        }
    }
    if (ferror(f)) {
        int err = errno;

        (void)fclose(f);
        errno = err;
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* The progress file is exactly four bytes, little-endian. */
static int read_progress(const char* path, uint32_t* out)
{
    uint8_t b[5];
    size_t  n;
    FILE*   f = fopen(path, "rb");

    if (f == NULL) {
        return -1;
    }
    n = fread(b, 1, sizeof b, f);
    if (ferror(f) || fclose(f) != 0) {
        return -1;
    }
    if (n != 4) {
        errno = EINVAL;
        return -1;
    }
    *out = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}

int main(int argc, char** argv)
{
    const char* dir = NULL;
    const char* wlog_path = NULL;
    const char* progress_path = NULL;
    uint64_t    restarts = 0;
    bool        have_restarts = false;
    seqcheck    c;
    writerlog   w;
    verdict     v;
    uint32_t    acked = 0;
    bool        have_acked = false;

    memset(&c, 0, sizeof c);
    memset(&w, 0, sizeof w);
    memset(&v, 0, sizeof v);
    for (int i = 1; i < argc; i++) {
        bool has_val = i + 1 < argc;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return EXIT_PASS;
        }
        if (strcmp(argv[i], "--dir") == 0 && has_val) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--writer-log") == 0 && has_val) {
            wlog_path = argv[++i];
        } else if (strcmp(argv[i], "--progress") == 0 && has_val) {
            progress_path = argv[++i];
        } else if (strcmp(argv[i], "--restarts") == 0 && has_val &&
                   parse_u64(argv[i + 1], 1000000000u, &restarts)) {
            have_restarts = true;
            i++;
        } else {
            (void)fprintf(stderr, "logcheck: bad argument: %s\n", argv[i]);
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (dir == NULL || (have_restarts && wlog_path == NULL)) {
        (void)fprintf(stderr, "logcheck: --dir is required, and --restarts needs --writer-log\n");
        usage(stderr);
        return EXIT_USAGE;
    }

    if (store_read_range(dir, 0, UINT64_MAX, visit, &c) != 0) {
        fail(&v, "cannot read the store dir=%s errno=%d (%s)", dir, errno, strerror(errno));
    }
    if (c.records == 0 && v.n == 0) {
        fail(&v, "no records in the store");
    }
    if (c.corrupt > 0) {
        fail(&v, "%llu record(s) failed their CRC in the middle of the log",
             (unsigned long long)c.corrupt);
    }
    if (c.backwards > 0) {
        fail(&v, "seq repeated or went backwards %llu time(s), first after seq=%u",
             (unsigned long long)c.backwards, (unsigned)c.bad_at);
    }
    if (c.big_gaps > 0) {
        fail(&v, "seq skipped more than one number %llu time(s), first after seq=%u",
             (unsigned long long)c.big_gaps, (unsigned)c.bad_at);
    }

    if (wlog_path != NULL) {
        if (scan_writer_log(wlog_path, &w) != 0) {
            fail(&v, "cannot read the writer log %s errno=%d (%s)", wlog_path, errno,
                 strerror(errno));
        } else {
            if (w.recovered == 0) {
                fail(&v, "no 'store recovered' entry in the writer log");
            }
            if (w.malformed > 0) {
                fail(&v, "%llu 'store recovered' entry(ies) without a readable dropped_bytes",
                     (unsigned long long)w.malformed);
            }
            if (w.max_dropped > MAX_DROPPED_BYTES) {
                fail(&v, "recovery dropped %llu bytes, more than one record (%u)",
                     (unsigned long long)w.max_dropped, (unsigned)MAX_DROPPED_BYTES);
            }
            if (have_restarts && w.recovered != restarts + 1) {
                fail(&v, "expected %llu 'store recovered' entries for %llu restart(s), found %llu",
                     (unsigned long long)(restarts + 1), (unsigned long long)restarts,
                     (unsigned long long)w.recovered);
            }
            if (w.errors > 0) {
                fail(&v, "%llu ERROR line(s) in the writer log", (unsigned long long)w.errors);
            }
        }
    }

    if (progress_path != NULL) {
        if (read_progress(progress_path, &acked) != 0) {
            fail(&v, "cannot read the progress file %s errno=%d (%s)", progress_path, errno,
                 strerror(errno));
        } else {
            have_acked = true;
            if (c.records == 0 || acked > c.last_seq) {
                fail(&v, "the log ends at seq=%u but seq=%u was acknowledged",
                     (unsigned)(c.records == 0 ? 0u : c.last_seq), (unsigned)acked);
            }
        }
    }

    (void)printf("logcheck records=%llu first_seq=%u last_seq=%u lost=%llu corrupt=%llu",
                 (unsigned long long)c.records, (unsigned)c.first_seq, (unsigned)c.last_seq,
                 (unsigned long long)c.lost, (unsigned long long)c.corrupt);
    if (wlog_path != NULL) {
        (void)printf(" recovered=%llu max_dropped=%llu", (unsigned long long)w.recovered,
                     (unsigned long long)w.max_dropped);
    }
    if (have_acked) {
        (void)printf(" acked=%u", (unsigned)acked);
    }
    (void)printf("\n");
    for (int i = 0; i < v.n; i++) {
        (void)printf("FAIL %s\n", v.reasons[i]);
    }
    if (v.n == 0) {
        (void)printf("PASS\n");
    }
    if (fflush(stdout) != 0) {
        return EXIT_FAIL;
    }
    return v.n == 0 ? EXIT_PASS : EXIT_FAIL;
}
