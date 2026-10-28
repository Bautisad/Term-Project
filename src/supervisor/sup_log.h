/* sup_log.h: supervisor events, one line each, in the DESIGN.md log format. */
#ifndef GT_SUP_LOG_H
#define GT_SUP_LOG_H

/* Writes "<ISO time> <LEVEL> supervisor <event> key=value ..." to stderr. */
__attribute__((format(printf, 2, 3)))
void log_event(const char* level, const char* fmt, ...);

/* close(fd) when fd >= 0; a failure is logged, not ignored. */
void close_fd(int fd);

#endif /* GT_SUP_LOG_H */
