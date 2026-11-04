/* status.h: the supervisor's periodic health line for each child. */
#ifndef GT_SUP_STATUS_H
#define GT_SUP_STATUS_H

#include <stddef.h>
#include <sys/types.h>

#include "child.h"

#define GT_STATUS_INTERVAL_S 3600 /* one status line per child per hour */

/* Resident memory (kB) and open descriptors of process `pid`, read from <proc_root>/<pid>/status
 * (VmRSS) and <proc_root>/<pid>/fd; proc_root is "/proc" outside tests.  Returns 0 with both
 * values set.  Returns -1 with both set to -1 when they cannot be had: quietly if the process is
 * already gone, with a WARN logged for anything else. */
int proc_stats(const char* proc_root, pid_t pid, long* rss_kb, long* fds);

/* Writes "status child=NAME alive=0|1 state=S degraded=0|1 rss_kb=N fds=N restarts=N faults=N"
 * into buf.  Returns the snprintf result, so a value >= n means the line was cut. */
int status_format(char* buf, size_t n, const child* c, long rss_kb, long fds);

/* Reads the child's stats (if it is running) and logs its status line at INFO. */
void status_log(const child* c, const char* proc_root);

#endif /* GT_SUP_STATUS_H */
