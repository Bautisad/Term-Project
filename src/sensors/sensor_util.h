/* sensor_util.h: what the soil and light processes both need, so neither repeats it. */
#ifndef GT_SENSOR_UTIL_H
#define GT_SENSOR_UTIL_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "ipc.h"

/* Milliseconds on `clk` (CLOCK_MONOTONIC for intervals, CLOCK_REALTIME for timestamps).
 * A clock failure is logged and gives 0. */
int64_t sensor_clock_ms(clockid_t clk);

/* close(fd) when fd >= 0; a failure is logged, not ignored. */
void sensor_close_fd(int fd);

/* Returns false with errno set if fd could not be made nonblocking. */
bool sensor_set_nonblock(int fd);

/* Log `what` once when it starts failing (ok false, err is the errno) and once when it
 * recovers, tracked in *down. */
void sensor_track(bool* down, bool ok, const char* what, int err);

/* A plain decimal number in 0..max, for command-line flags. */
bool sensor_parse_uint(const char* s, unsigned long max, unsigned long* out);

/* The supervisor's socket is fd; if nothing is open there (run by hand, not by the supervisor),
 * say so and return -1 so the process runs without heartbeats. */
int sensor_ctl_fd(int fd);

/* Start-up: block SIGTERM and SIGINT, and make the supervisor's socket nonblocking if
 * ctl_fd >= 0.  Returns a descriptor that becomes readable when a signal arrives, or -1 with
 * the reason logged. */
int sensor_setup(int ctl_fd);

/* After poll() on pfd[0] (that descriptor) and pfd[1] (the supervisor's socket): GT_EXIT_OK if
 * told to stop, GT_EXIT_IPC if the supervisor's end is closed, else -1.  Logs either. */
int sensor_stop_code(const struct pollfd* pfd);

/* When a periodic job is next due: due + period_ms, or now + period_ms if that has already
 * passed, so a late wake-up does not make it catch up. */
int64_t sensor_next_due(int64_t due, unsigned period_ms, int64_t now);

/* Send one heartbeat on the control socket and advance *seq.  Returns -1 to carry on (a full
 * socket is logged once through *down; ctl_fd < 0 sends nothing), or GT_EXIT_IPC when the
 * supervisor is gone. */
int sensor_heartbeat(int ctl_fd, gt_sensor_id id, uint32_t* seq, gt_child_state state, bool* down);

/* The connection from a sensor to the hub. */
typedef struct sensor_hub {
    const char* path;
    int         fd;   /* -1 when not connected */
    bool        down; /* the last send failed */
} sensor_hub;

/* Connect if needed and send one message, or drop it: nothing is queued.  A full hub (EAGAIN)
 * keeps its connection; any other failure closes it so the next call reconnects.  The first
 * failure and the recovery are logged. */
void sensor_hub_send(sensor_hub* hub, gt_msg_type type, gt_sensor_id id, uint32_t seq,
                     const void* payload, size_t len);

#endif /* GT_SENSOR_UTIL_H */
