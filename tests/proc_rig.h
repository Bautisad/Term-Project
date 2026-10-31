/* proc_rig.h: shared by the sensor process tests.  Runs a sensor loop in a forked child and
 * plays the hub and the supervisor from this side.  Header-only; a test includes it once.
 * A failed CHECK prints file:line and exits 1. */
#ifndef GT_PROC_RIG_H
#define GT_PROC_RIG_H

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ipc_io.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);         \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#define WAIT_MS 2000

static char hub_path[128];

typedef struct rig {
    pid_t        pid;
    gt_sensor_id id;       /* who the child should say it is */
    int          ctl;      /* our end of the supervisor socketpair */
    int          hub;      /* the accepted sensor connection, -1 until rig_accept */
    int          listener; /* the hub's listening socket */
} rig;

/* Runs in the child: the sensor loop on the given supervisor socket; returns its exit code. */
typedef int (*rig_child)(int ctl_fd, const void* arg);

static inline void nap(unsigned ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};

    CHECK(nanosleep(&ts, NULL) == 0);
}

static inline void shut(int* fd)
{
    if (*fd >= 0) {
        CHECK(close(*fd) == 0);
        *fd = -1;
    }
}

static inline void rig_start(rig* r, gt_sensor_id id, rig_child child, const void* arg)
{
    int sv[2];

    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    r->listener = ipc_listen(hub_path);
    CHECK(r->listener >= 0);
    r->id = id;
    r->hub = -1;
    r->pid = fork();
    CHECK(r->pid >= 0);
    if (r->pid == 0) {
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL); /* never outlive a failed test */
        (void)close(r->listener); /* else the hub socket would stay alive after we close ours */
        (void)close(sv[0]);
        _exit(child(sv[1], arg));
    }
    CHECK(close(sv[1]) == 0);
    r->ctl = sv[0];
}

static inline void rig_accept(rig* r)
{
    struct pollfd p = {.fd = r->listener, .events = POLLIN};

    CHECK(poll(&p, 1, WAIT_MS) == 1);
    r->hub = ipc_accept(r->listener);
    CHECK(r->hub >= 0);
}

/* Expect the child to exit with `code` within WAIT_MS (PR_SET_PDEATHSIG cleans up if it does
 * not); a clean stop (GT_EXIT_OK) is asked for with SIGTERM.  Closes our ends. */
static inline void rig_end(rig* r, int code)
{
    pid_t w = 0;
    int   st = 0;

    if (code == GT_EXIT_OK) {
        CHECK(kill(r->pid, SIGTERM) == 0);
    }
    for (int i = 0; i < WAIT_MS / 10 && (w = waitpid(r->pid, &st, WNOHANG)) == 0; i++) {
        nap(10);
    }
    CHECK(w == r->pid && WIFEXITED(st) && WEXITSTATUS(st) == code);
    shut(&r->ctl);
    shut(&r->hub);
    shut(&r->listener);
}

/* The next message on fd, within WAIT_MS. */
static inline void next_msg(int fd, gt_msg_hdr* h, void* payload)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};

    CHECK(poll(&p, 1, WAIT_MS) == 1);
    CHECK(ipc_recv(fd, h, payload, GT_MSG_MAX) > 0);
}

/* The next hub-socket message: it must be `type`, from the rig's sensor; n bytes go to out. */
static inline void next_hub_msg(const rig* r, gt_msg_hdr* h, gt_msg_type type, void* out, size_t n)
{
    uint8_t buf[GT_MSG_MAX];

    next_msg(r->hub, h, buf);
    CHECK(h->type == type && h->sensor_id == r->id);
    memcpy(out, buf, n);
}

static inline gt_msg_heartbeat next_hb(const rig* r)
{
    gt_msg_hdr       h;
    gt_msg_heartbeat hb;
    uint8_t          buf[GT_MSG_MAX];

    next_msg(r->ctl, &h, buf);
    CHECK(h.type == GT_MSG_HEARTBEAT && h.sensor_id == r->id);
    memcpy(&hb, buf, sizeof hb);
    CHECK(hb.pid == r->pid);
    return hb;
}

/* Read heartbeats until one reports `state`. */
static inline void expect_state(const rig* r, gt_child_state state)
{
    for (int i = 0; i < 60; i++) {
        if (next_hb(r).state == (uint32_t)state) {
            return;
        }
    }
    CHECK(false);
}

#endif /* GT_PROC_RIG_H */
