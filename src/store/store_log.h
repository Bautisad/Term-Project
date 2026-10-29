/* store_log.h: store events, one line each, in the DESIGN.md log format. */
#ifndef GT_STORE_LOG_H
#define GT_STORE_LOG_H

/* Writes "<ISO time> <LEVEL> store <event> key=value ..." to stderr.  errno is left as
 * it was, so a failure path can log and then return the original error. */
__attribute__((format(printf, 2, 3)))
void store_log(const char* level, const char* fmt, ...);

/* close(fd) when fd >= 0; a failure is logged, not ignored.  errno is left as it was. */
void store_close_fd(int fd);

#endif /* GT_STORE_LOG_H */
