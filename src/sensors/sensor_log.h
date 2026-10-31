/* sensor_log.h: sensor-process events, one line each, in the DESIGN.md log format. */
#ifndef GT_SENSOR_LOG_H
#define GT_SENSOR_LOG_H

/* Name used as the component in every line ("soil", "light"). */
void slog_init(const char* component);

/* Lets gcc check the format string; other parsers (Visual Studio's editor) skip it. */
#if defined(__GNUC__)
#define GT_PRINTF_FORMAT(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define GT_PRINTF_FORMAT(fmt_idx, args_idx)
#endif

/* Writes "<ISO time> <LEVEL> <component> <event> key=value ..." to stderr. */
GT_PRINTF_FORMAT(2, 3)
void slog(const char* level, const char* fmt, ...);

#endif /* GT_SENSOR_LOG_H */
