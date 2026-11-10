#include "orphan.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sup_log.h"

#define MAX_ORPHANS 8

typedef struct orphan {
    pid_t              pid;
    unsigned long long start;
} orphan;

unsigned long long proc_start(const char* proc_root, pid_t pid)
{
    char               path[512];
    char               buf[512];
    char               state = 'Z';
    unsigned long long start = 0;
    char*              p = NULL;
    FILE*              f;

    if (snprintf(path, sizeof path, "%s/%d/stat", proc_root, (int)pid) >= (int)sizeof path ||
        (f = fopen(path, "r")) == NULL) {
        return 0;
    }
    if (fgets(buf, sizeof buf, f) != NULL) {
        p = strrchr(buf, ')'); /* the name inside "(...)" may hold spaces and ')' */
    }
    (void)fclose(f);
    /* After it: the state, 18 fields to skip, then the start time (fields 3 to 22 of proc(5)). */
    if (p == NULL || sscanf(p + 1, " %c%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s%*s %llu",
                            &state, &start) != 2) {
        return 0;
    }
    return state == 'Z' ? 0 : start;
}

void pids_save(const char* path, const child* ch, size_t n)
{
    char  tmp[512];
    FILE* f;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp ||
        (f = fopen(tmp, "w")) == NULL) {
        log_event("ERROR", "pids open failed path=%s errno=%d", path, errno);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned long long start = ch[i].pid != 0 ? proc_start("/proc", ch[i].pid) : 0;

        if (start != 0) {
            (void)fprintf(f, "%d %llu\n", (int)ch[i].pid, start);
        }
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        log_event("ERROR", "pids write failed path=%s errno=%d", path, errno);
    }
}

/* How many are still the same running process.  When sig is not 0, each of those gets it first. */
static size_t alive(const orphan* o, size_t n, int sig)
{
    size_t k = 0;

    for (size_t i = 0; i < n; i++) {
        if (proc_start("/proc", o[i].pid) == o[i].start) {
            k++;
            if (sig != 0 && kill(o[i].pid, sig) != 0 && errno != ESRCH) {
                log_event("ERROR", "orphan pid=%d kill failed signal=%d errno=%d", (int)o[i].pid,
                          sig, errno);
            }
        }
    }
    return k;
}

static void wait_gone(const orphan* o, size_t n, unsigned tenths)
{
    struct timespec tick = {0, 100000000L};

    for (; tenths > 0 && alive(o, n, 0) > 0; tenths--) {
        (void)nanosleep(&tick, NULL);
    }
}

void orphans_kill(const char* path, unsigned grace_s)
{
    orphan             o[MAX_ORPHANS];
    size_t             n = 0;
    FILE*              f = fopen(path, "r");
    int                pid;
    unsigned long long start;

    if (f == NULL && errno != ENOENT) {
        log_event("WARN", "pids open failed path=%s errno=%d", path, errno);
    }
    while (f != NULL && n < MAX_ORPHANS && fscanf(f, "%d %llu", &pid, &start) == 2) {
        if (pid > 1 && start != 0 && proc_start("/proc", pid) == start) {
            o[n++] = (orphan){pid, start};
            log_event("WARN", "orphan pid=%d", pid);
        }
    }
    if (f != NULL) {
        (void)fclose(f);
    }
    (void)alive(o, n, SIGTERM);
    wait_gone(o, n, grace_s * 10);
    (void)alive(o, n, SIGKILL);
    wait_gone(o, n, 10);
    log_event("INFO", "start orphans_killed=%zu", n);
}
