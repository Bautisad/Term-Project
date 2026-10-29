/* gt-feeder: a fake sensor hub.  Appends synthetic records to a store at a fixed rate so the
 * store can be tested (and killed with SIGKILL) without the real sensor drivers.
 *
 * Every field of record N is a pure function of N, so a checker can recompute and compare
 * it.  Timestamps are the real wall clock, so they do not go backwards across restarts. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "record.h"
#include "store.h"

#define RATE_DEFAULT 1000u
#define RATE_MAX     1000000u
#define NS_PER_SEC   1000000000L
#define MAX_LAG_NS   NS_PER_SEC /* behind by more than this: drop the backlog, don't burst */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

__attribute__((format(printf, 2, 3)))
static void feeder_log(const char* level, const char* fmt, ...)
{
    char      ts[32];
    time_t    t = time(NULL);
    struct tm tm;
    va_list   ap;

    if (gmtime_r(&t, &tm) == NULL || strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)strcpy(ts, "-");
    }
    (void)fprintf(stderr, "%s %s feeder ", ts, level);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
}

static void usage(FILE* out)
{
    (void)fprintf(out,
                  "usage: gt-feeder --dir DIR [--rate N] [--count N] [--progress FILE]\n"
                  "  --dir DIR    store directory (created if missing; resumes after the last record)\n"
                  "  --rate N     records per second, default %u, 0 = as fast as the disk allows\n"
                  "               (every record is fsynced, so the disk sets the ceiling)\n"
                  "  --count N    stop after N records, default 0 = run until SIGTERM or SIGINT\n"
                  "  --progress FILE  after each append, record its seq there (4 bytes, little-endian,\n"
                  "               no fsync, so it survives kill -9): the log checker uses it to prove no\n"
                  "               acknowledged record was lost\n"
                  "At 1000 records/s the log grows by about 86 MB per hour.\n",
                  RATE_DEFAULT);
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

static void make_record(uint32_t seq, uint64_t now_ms, gt_record* rec)
{
    memset(rec, 0, sizeof *rec);
    rec->ts_unix_ms = now_ms;
    rec->seq = seq;
    rec->soil_raw = (uint16_t)(300u + (seq * 37u) % 700u);
    rec->flags = (seq % 2u != 0u) ? GT_REC_LIGHT_BRIGHT : 0u;
    rec->light_bright_ms = (uint16_t)((seq * 13u) % 60001u);
    rec->light_edges = (uint16_t)(seq % 7u);
}

/* Records the last acknowledged seq at offset 0 of the progress file. */
static int write_progress(int fd, uint32_t seq)
{
    uint8_t buf[4] = {(uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24)};
    ssize_t n;

    do {
        n = pwrite(fd, buf, sizeof buf, 0);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof buf) {
        if (n >= 0) {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

static uint64_t wall_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int64_t ns_between(const struct timespec* a, const struct timespec* b)
{
    return ((int64_t)b->tv_sec - (int64_t)a->tv_sec) * NS_PER_SEC +
           ((int64_t)b->tv_nsec - (int64_t)a->tv_nsec);
}

static void add_ns(struct timespec* t, int64_t ns)
{
    int64_t total = (int64_t)t->tv_nsec + ns;

    t->tv_sec += (time_t)(total / NS_PER_SEC);
    t->tv_nsec = (long)(total % NS_PER_SEC);
}

/* Sleeps until the deadline; a signal ends the sleep early (g_stop is then set). */
static void pace(struct timespec* deadline, int64_t period_ns)
{
    struct timespec now;

    add_ns(deadline, period_ns);
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    if (ns_between(deadline, &now) > MAX_LAG_NS) {
        *deadline = now; /* fell far behind (disk stall): carry on from here */
        return;
    }
    (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
}

int main(int argc, char** argv)
{
    const char*       dir = NULL;
    const char*       progress = NULL;
    int               progress_fd = -1;
    uint64_t          rate = RATE_DEFAULT;
    uint64_t          count = 0;
    gt_store*         store = NULL;
    gt_store_recovery info;
    struct sigaction  sa;
    struct timespec   t0 = {0, 0};
    struct timespec   t1 = {0, 0};
    struct timespec   deadline;
    uint64_t          done = 0;
    uint32_t          seq;
    int               status = GT_EXIT_OK;

    for (int i = 1; i < argc; i++) {
        bool has_val = i + 1 < argc;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return GT_EXIT_OK;
        }
        if (strcmp(argv[i], "--dir") == 0 && has_val) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--progress") == 0 && has_val) {
            progress = argv[++i];
        } else if (strcmp(argv[i], "--rate") == 0 && has_val && parse_u64(argv[i + 1], RATE_MAX, &rate)) {
            i++;
        } else if (strcmp(argv[i], "--count") == 0 && has_val && parse_u64(argv[i + 1], UINT32_MAX, &count)) {
            i++;
        } else {
            feeder_log("ERROR", "bad_argument arg=%s", argv[i]);
            usage(stderr);
            return GT_EXIT_CONFIG;
        }
    }
    if (dir == NULL) {
        feeder_log("ERROR", "missing_argument arg=--dir");
        usage(stderr);
        return GT_EXIT_CONFIG;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    (void)sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0) {
        feeder_log("ERROR", "sigaction failed errno=%d", errno);
        return GT_EXIT_FATAL;
    }

    if (store_recover(dir, &store, &info) != 0) {
        feeder_log("ERROR", "recover failed dir=%s errno=%d", dir, errno);
        return GT_EXIT_FATAL;
    }
    seq = info.has_records ? info.last_seq + 1u : 1u;
    feeder_log("INFO", "start dir=%s rate=%llu resumed_seq=%u dropped_bytes=%llu", dir,
               (unsigned long long)rate, (unsigned)seq, (unsigned long long)info.dropped_bytes);

    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        feeder_log("ERROR", "clock_gettime failed errno=%d", errno);
        status = GT_EXIT_FATAL;
        goto out;
    }
    if (progress != NULL) {
        progress_fd = open(progress, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (progress_fd < 0) {
            feeder_log("ERROR", "progress open failed path=%s errno=%d", progress, errno);
            status = GT_EXIT_FATAL;
            goto out;
        }
    }
    deadline = t0;
    while (!g_stop && (count == 0 || done < count)) {
        gt_record rec;

        if (seq == 0) { /* the 32-bit sequence number wrapped */
            feeder_log("ERROR", "seq_exhausted records=%llu", (unsigned long long)done);
            status = GT_EXIT_FATAL;
            break;
        }
        make_record(seq, wall_ms(), &rec);
        if (store_append(store, &rec) != 0) {
            feeder_log("ERROR", "append failed seq=%u errno=%d", (unsigned)seq, errno);
            status = GT_EXIT_FATAL;
            break;
        }
        if (progress_fd >= 0 && write_progress(progress_fd, seq) != 0) {
            feeder_log("ERROR", "progress write failed seq=%u errno=%d", (unsigned)seq, errno);
            status = GT_EXIT_FATAL;
            break;
        }
        seq++;
        done++;
        if (rate != 0 && !g_stop && (count == 0 || done < count)) {
            pace(&deadline, NS_PER_SEC / (int64_t)rate);
        }
    }

out:
    if (progress_fd >= 0 && close(progress_fd) != 0) {
        feeder_log("ERROR", "progress close failed errno=%d", errno);
        status = GT_EXIT_FATAL;
    }
    store_close(store);
    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
        t1 = t0;
    }
    {
        int64_t elapsed_ms = ns_between(&t0, &t1) / 1000000;
        double  achieved = elapsed_ms > 0 ? (double)done * 1000.0 / (double)elapsed_ms : 0.0;

        feeder_log("INFO", "done records=%llu last_seq=%u elapsed_ms=%lld rate=%.1f",
                   (unsigned long long)done, (unsigned)(seq - 1u), (long long)elapsed_ms, achieved);
    }
    return status;
}
