#include "store_log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void store_log(const char* level, const char* fmt, ...)
{
    int       saved = errno;
    char      ts[32];
    time_t    t = time(NULL);
    struct tm tm;
    va_list   ap;

    if (gmtime_r(&t, &tm) == NULL || strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)strcpy(ts, "-");
    }
    (void)fprintf(stderr, "%s %s store ", ts, level);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
    errno = saved;
}

void store_close_fd(int fd)
{
    int saved = errno;

    if (fd >= 0 && close(fd) != 0) {
        store_log("ERROR", "close failed errno=%d", errno);
    }
    errno = saved;
}
