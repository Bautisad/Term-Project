/* light_proc.h: the light sensor process loop.  light_main.c and the tests both call light_run,
 * so the real binary and the tests run the same code. */
#ifndef GT_LIGHT_PROC_H
#define GT_LIGHT_PROC_H

#include "light_drv.h"
#include "light_logic.h"

typedef struct light_opts {
    const char* hub_path;     /* hub's listening socket */
    int         ctl_fd;       /* supervisor control socket for heartbeats; -1 for none */
    unsigned    window_ms;    /* time between messages to the hub; at most 65535 (record.h) */
    unsigned    heartbeat_ms; /* time between heartbeats */
    light_cfg   cfg;
} light_opts;

/* The production settings: a window of one minute, ipc.h's heartbeat interval. */
light_opts light_opts_default(void);

/* Heartbeat, then every window: close it, send it, until told to stop; between those, sleep
 * until the pin changes.  Returns a gt_exit_code:
 *   GT_EXIT_OK      SIGTERM or SIGINT
 *   GT_EXIT_FATAL   reading the pin failed, or a signal or socket setup call failed
 *   GT_EXIT_IPC     the supervisor's control socket closed
 *   GT_EXIT_CONFIG  opts out of range (restarting would not help)
 * A hub that is down is not an exit: that window's message is dropped and the connection is
 * retried on the next window. */
int light_run(const light_dev* dev, const light_opts* opts);

#endif /* GT_LIGHT_PROC_H */
