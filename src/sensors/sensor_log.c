#include "sensor_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* component = "sensor";

void slog_init(const char* name)
{
    component = name;
}

void slog(const char* level, const char* fmt, ...)
{
    char      ts[32];
    time_t    t = time(NULL);
    struct tm tm;
    va_list   ap;

    if (gmtime_r(&t, &tm) == NULL || strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)strcpy(ts, "-");
    }
    (void)fprintf(stderr, "%s %s %s ", ts, level, component);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
}
