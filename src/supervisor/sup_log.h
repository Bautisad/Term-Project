/* sup_log.h: supervisor events, one line each, in the DESIGN.md log format. */
#ifndef GT_SUP_LOG_H
#define GT_SUP_LOG_H

/* Writes "<ISO time> <LEVEL> supervisor <event> key=value ..." to stderr, and to the events
 * file once events_open succeeded.  A line is cut at 511 bytes. */
__attribute__((format(printf, 2, 3)))
void log_event(const char* level, const char* fmt, ...);

/* Also append every log_event line to `path`.  Returns 0, or -1 with the reason logged; the
 * supervisor carries on with stderr alone. */
int events_open(const char* path);

/* close(fd) when fd >= 0; a failure is logged, not ignored. */
void close_fd(int fd);

#endif /* GT_SUP_LOG_H */
