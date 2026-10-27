#include "sup_log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void log_event(const char* level, const char* fmt, ...)
{
    char      ts[32];
    time_t    t = time(NULL);
    struct tm tm;
    va_list   ap;

    if (gmtime_r(&t, &tm) == NULL || strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)strcpy(ts, "-");
    }
    (void)fprintf(stderr, "%s %s supervisor ", ts, level);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
}

void close_fd(int fd)
{
    if (fd >= 0 && close(fd) != 0) {
        log_event("ERROR", "close failed errno=%d", errno);
    }
}
