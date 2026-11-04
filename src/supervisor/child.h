/* child.h: one supervised daemon: how it is started, and what its exit means. */
#ifndef GT_SUP_CHILD_H
#define GT_SUP_CHILD_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "ipc.h"

/* The supervisor's limits, in seconds.  Heartbeats: silent for soft_s logs a warning, for hard_s
 * gets SIGTERM, and grace_s after that SIGKILL.  Restarts: more than restart_max of them inside
 * window_s is a crash loop, and a child that stays up for window_s is stable again.  The
 * defaults are in ipc.h. */
typedef struct hb_limits {
    unsigned soft_s;
    unsigned hard_s;
    unsigned grace_s;
    unsigned restart_max; /* at most GT_RESTART_MAX: that is how many restart times a child keeps */
    unsigned window_s;
} hb_limits;

typedef struct child {
    const char* path;
    const char* name;    /* last path component, for log lines */
    pid_t       pid;     /* 0 when not running */
    int         ctl_fd;  /* supervisor end of the control socketpair, -1 when none */
    bool        stopped; /* will not be started again */
    int64_t     restart_at_ms;
    unsigned    backoff_s;
    unsigned    restarts;
    int64_t     recent[GT_RESTART_MAX + 1]; /* when the last restarts were scheduled */
    int64_t     started_ms; /* when it was last started */
    bool        degraded;   /* in a crash loop */
    int64_t     last_hb_ms; /* last heartbeat, or the start */
    int64_t     kill_at_ms; /* SIGKILL due; 0 before SIGTERM was sent, -1 once SIGKILL was */
    uint32_t    state;      /* gt_child_state of the last heartbeat */
    bool        warned;     /* the silence warning was logged */
    bool        hung;       /* killed for silence: restart even if it exits 0 */
} child;

/* Ready to be started on the first pass of the loop. */
void child_init(child* c, const char* path);

/* fork/exec c->path with a nonblocking control socket on fd GT_CHILD_CTL_FD and `mask` as
 * the child's signal mask; the silence clock starts at `now`.  Returns 0, or -1 with the
 * reason logged. */
int spawn(child* c, const sigset_t* mask, int64_t now);

/* A heartbeat arrived: restart the silence clock, and log a change of reported state. */
void child_heartbeat(child* c, int64_t now, uint32_t state);

/* SIGTERM now, SIGKILL due after the grace period (used for silence and for shutdown). */
void child_term(child* c, int64_t now, const hb_limits* l);

/* Apply the deadlines: warn once at soft, SIGTERM at hard (and mark the child hung), SIGKILL
 * once the grace after SIGTERM has run out. */
void child_watch(child* c, int64_t now, const hb_limits* l);

/* The time the loop must wake for this child next: its restart, or its next deadline.
 * -1 for none. */
int64_t child_next_event(const child* c, const hb_limits* l);

/* Restart after the current backoff (1s, 2s, 4s ... capped), then double it.  More than
 * restart_max restarts inside window_s is a crash loop: the child is marked DEGRADED (logged
 * once) and its backoff is held at the cap. */
void schedule_restart(child* c, int64_t now, const hb_limits* l);

/* Handle a reaped child's wait status.  GT_EXIT_OK and GT_EXIT_CONFIG are final; any
 * other exit, or a signal, schedules a restart.  While `stopping`, nothing restarts. */
void child_exited(child* c, int status, bool stopping, int64_t now, const hb_limits* l);

#endif /* GT_SUP_CHILD_H */
