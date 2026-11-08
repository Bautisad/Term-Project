#include "sup_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int events_fd = -1; /* the events.log file, -1 when there is none */

void log_event(const char* level, const char* fmt, ...)
{
    char      line[512];
    char      ts[32];
    time_t    t = time(NULL);
    struct tm tm;
    va_list   ap;
    size_t    n;

    if (gmtime_r(&t, &tm) == NULL || strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)strcpy(ts, "-");
    }
    n = (size_t)snprintf(line, sizeof line, "%s %s supervisor ", ts, level);
    va_start(ap, fmt);
    (void)vsnprintf(line + n, sizeof line - n - 1, fmt, ap); /* room left for the newline */
    va_end(ap);
    n = strlen(line);
    line[n++] = '\n';
    line[n] = '\0';
    (void)fputs(line, stderr);
    if (events_fd >= 0 && write(events_fd, line, n) != (ssize_t)n) {
        int err = errno;

        (void)close(events_fd); /* give up on the file; stderr still has every line */
        events_fd = -1;
        log_event("ERROR", "events write failed errno=%d", err);
    }
}

int events_open(const char* path)
{
    events_fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (events_fd < 0) {
        log_event("ERROR", "events open failed path=%s errno=%d", path, errno);
        return -1;
    }
    return 0;
}

void close_fd(int fd)
{
    if (fd >= 0 && close(fd) != 0) {
        log_event("ERROR", "close failed errno=%d", errno);
    }
}
