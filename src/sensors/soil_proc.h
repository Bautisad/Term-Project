/* soil_proc.h: the soil sensor process loop.  soil_main.c and the tests both call soil_run,
 * so the real binary and the tests run the same code. */
#ifndef GT_SOIL_PROC_H
#define GT_SOIL_PROC_H

#include "soil_drv.h"
#include "soil_logic.h"

typedef struct soil_opts {
    const char* hub_path;     /* hub's listening socket */
    int         ctl_fd;       /* supervisor control socket for heartbeats; -1 for none */
    unsigned    samples;      /* quick samples per cycle, 1..SOIL_MAX_SAMPLES */
    unsigned    attempts;     /* tries per sample before the cycle counts as a failed read */
    unsigned    gap_ms;       /* pause between samples and between tries */
    unsigned    cycle_ms;     /* time between messages to the hub */
    unsigned    heartbeat_ms; /* time between heartbeats */
    unsigned    fail_limit;   /* failed cycles in a row before the process gives up */
    soil_cfg    cfg;
} soil_opts;

/* The production settings: DESIGN.md's once-a-minute cycle, ipc.h's heartbeat interval. */
soil_opts soil_opts_default(void);

/* Heartbeat, read, judge, send, once per cycle, until told to stop.  Returns a gt_exit_code:
 *   GT_EXIT_OK      SIGTERM or SIGINT
 *   GT_EXIT_FATAL   fail_limit cycles in a row could not read the sensor, or a signal or
 *                   socket setup call failed
 *   GT_EXIT_IPC     the supervisor's control socket closed
 *   GT_EXIT_CONFIG  opts out of range (restarting would not help)
 * A hub that is down is not an exit: that cycle's message is dropped and the connection
 * is retried on the next cycle. */
int soil_run(const soil_dev* dev, const soil_opts* opts);

#endif /* GT_SOIL_PROC_H */
