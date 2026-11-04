#include "status.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sup_log.h"

/* The process may exit between our poll() and this read; that is not worth a log line. */
static bool gone(int err)
{
    return err == ENOENT || err == ESRCH;
}

/* VmRSS (kB) from a /proc/<pid>/status file.  0, or -1 with the reason logged unless the
 * process is gone. */
static int read_rss(const char* path, pid_t pid, long* rss_kb)
{
    char  line[128];
    FILE* f = fopen(path, "r");
    int   rc = -1;

    if (f == NULL) {
        if (!gone(errno)) {
            log_event("WARN", "status pid=%d open %s failed errno=%d", (int)pid, path, errno);
        }
        return -1;
    }
    while (rc != 0 && fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            char* end;
            long  v = strtol(line + 6, &end, 10);

            if (end != line + 6 && v >= 0) {
                *rss_kb = v;
                rc = 0;
            }
        }
    }
    if (rc != 0 && ferror(f)) {
        if (!gone(errno)) {
            log_event("WARN", "status pid=%d read %s failed errno=%d", (int)pid, path, errno);
        }
    } else if (rc != 0) {
        log_event("WARN", "status pid=%d no VmRSS in %s", (int)pid, path);
    }
    if (fclose(f) != 0) {
        log_event("WARN", "status pid=%d close %s failed errno=%d", (int)pid, path, errno);
    }
    return rc;
}

/* Number of entries in a /proc/<pid>/fd directory.  0, or -1 as read_rss.  The descriptor
 * opendir() uses here is ours, not the child's, so it is not in the count. */
static int count_fds(const char* path, pid_t pid, long* fds)
{
    DIR*           d = opendir(path);
    struct dirent* e;
    long           n = 0;
    int            err;

    if (d == NULL) {
        if (!gone(errno)) {
            log_event("WARN", "status pid=%d open %s failed errno=%d", (int)pid, path, errno);
        }
        return -1;
    }
    errno = 0;
    while ((e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.'; /* skips "." and ".."; descriptor names are all digits */
    }
    err = errno;
    if (closedir(d) != 0) {
        log_event("WARN", "status pid=%d close %s failed errno=%d", (int)pid, path, errno);
    }
    if (err != 0) {
        if (!gone(err)) {
            log_event("WARN", "status pid=%d read %s failed errno=%d", (int)pid, path, err);
        }
        return -1;
    }
    *fds = n;
    return 0;
}

int proc_stats(const char* proc_root, pid_t pid, long* rss_kb, long* fds)
{
    char path[512];
    long rss = -1;
    long nfd = -1;
    int  n = snprintf(path, sizeof path, "%s/%d/status", proc_root, (int)pid);

    *rss_kb = -1;
    *fds = -1;
    if (n < 0 || (size_t)n >= sizeof path) {
        log_event("WARN", "status pid=%d proc path too long", (int)pid);
        return -1;
    }
    if (read_rss(path, pid, &rss) != 0) {
        return -1;
    }
    n = snprintf(path, sizeof path, "%s/%d/fd", proc_root, (int)pid);
    if (n < 0 || (size_t)n >= sizeof path) {
        log_event("WARN", "status pid=%d proc path too long", (int)pid);
        return -1;
    }
    if (count_fds(path, pid, &nfd) != 0) {
        return -1;
    }
    *rss_kb = rss;
    *fds = nfd;
    return 0;
}

int status_format(char* buf, size_t n, const child* c, long rss_kb, long fds)
{
    static const char* const names[] = {"starting", "running", "degraded"};
    const char*              state = c->state < sizeof names / sizeof names[0] ? names[c->state] : "unknown";

    return snprintf(buf, n,
                    "status child=%s alive=%d state=%s degraded=%d rss_kb=%ld fds=%ld restarts=%u faults=%u",
                    c->name, c->pid != 0, state, c->degraded, rss_kb, fds, c->restarts, c->faults);
}

void status_log(const child* c, const char* proc_root)
{
    char line[256];
    long rss_kb = -1;
    long fds = -1;

    if (c->pid != 0) {
        (void)proc_stats(proc_root, c->pid, &rss_kb, &fds);
    }
    if (status_format(line, sizeof line, c, rss_kb, fds) >= (int)sizeof line) {
        log_event("WARN", "status child=%s line truncated", c->name);
    }
    log_event("INFO", "%s", line);
}
