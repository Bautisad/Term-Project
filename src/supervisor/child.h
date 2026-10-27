/* child.h: one supervised daemon: how it is started, and what its exit means. */
#ifndef GT_SUP_CHILD_H
#define GT_SUP_CHILD_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct child {
    const char* path;
    const char* name;    /* last path component, for log lines */
    pid_t       pid;     /* 0 when not running */
    int         ctl_fd;  /* supervisor end of the control socketpair, -1 when none */
    bool        stopped; /* will not be started again */
    int64_t     restart_at_ms;
    unsigned    backoff_s;
    unsigned    restarts;
} child;

/* Ready to be started on the first pass of the loop. */
void child_init(child* c, const char* path);

/* fork/exec c->path with a control socket on fd GT_CHILD_CTL_FD and `mask` as the
 * child's signal mask.  Returns 0, or -1 with the reason logged. */
int spawn(child* c, const sigset_t* mask);

/* Restart after the current backoff (1s, 2s, 4s ... capped), then double it. */
void schedule_restart(child* c, int64_t now);

/* Handle a reaped child's wait status.  GT_EXIT_OK and GT_EXIT_CONFIG are final; any
 * other exit, or a signal, schedules a restart.  While `stopping`, nothing restarts. */
void child_exited(child* c, int status, bool stopping, int64_t now);

#endif /* GT_SUP_CHILD_H */
