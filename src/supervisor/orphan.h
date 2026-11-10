/* orphan.h: children a dead supervisor left behind.  The supervisor records "pid start_time" for
 * each child it starts; the next supervisor kills the ones that are still the same process. */
#ifndef GT_SUP_ORPHAN_H
#define GT_SUP_ORPHAN_H

#include <stddef.h>
#include <sys/types.h>

#include "child.h"

/* Start time (field 22 of <proc_root>/<pid>/stat, in clock ticks) of a running process, or 0
 * when it is gone, a zombie or unreadable.  The pair (pid, start time) names one process even
 * after the kernel reuses the pid. */
unsigned long long proc_start(const char* proc_root, pid_t pid);

/* Rewrites `path` (by rename, so never half-written) with the children that are running.
 * A failure is logged. */
void pids_save(const char* path, const child* ch, size_t n);

/* SIGTERM every process listed in `path` that is still the same process, SIGKILL those left
 * after `grace_s` seconds, and log "start orphans_killed=N".  A missing or garbled file is N=0. */
void orphans_kill(const char* path, unsigned grace_s);

#endif /* GT_SUP_ORPHAN_H */
