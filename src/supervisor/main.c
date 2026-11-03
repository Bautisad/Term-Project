/* gt-supervisor: starts the hub and sensor daemons, reaps them, restarts the ones that
 * die, and watches their heartbeats.  A child silent for --soft seconds is logged, silent for
 * --hard seconds gets SIGTERM, and --grace seconds later SIGKILL (the same grace applies at
 * shutdown).  A child killed for silence is restarted even if it exits 0.  More than --max
 * restarts inside --window seconds is a crash loop: the child is marked DEGRADED and its restart
 * delay held at the cap.  An events.log file is not written yet.
 *
 * usage: gt-supervisor [--soft S] [--hard S] [--grace S] [--max N] [--window S] [CHILD_PATH...]
 *        defaults: the ipc.h limits and ./gt-hub ./gt-sensor-soil ./gt-sensor-light ./gt-interface
 *        gt-supervisor --version
 * Exit: 0 clean shutdown, 1 no child left to supervise, 2 bad arguments. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "child.h"
#include "ipc.h"
#include "ipc_io.h"
#include "sup_log.h"

#define MAX_CHILDREN 4

static int64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        log_event("ERROR", "clock_gettime failed errno=%d", errno);
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void reap_children(child* ch, size_t n, bool stopping, int64_t now, const hb_limits* lim)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (ch[i].pid == pid) {
                child_exited(&ch[i], status, stopping, now, lim);
            }
        }
    }
    if (pid < 0 && errno != ECHILD) {
        log_event("ERROR", "waitpid failed errno=%d", errno);
    }
}

static void stop_all(child* ch, size_t n, int64_t now, const hb_limits* lim)
{
    log_event("INFO", "stopping children=%zu", n);
    for (size_t i = 0; i < n; i++) {
        if (ch[i].pid == 0) {
            ch[i].stopped = true;
        } else {
            child_term(&ch[i], now, lim);
        }
    }
}

#define DRAIN_MAX 8 /* messages per child per wake-up, so a flood cannot starve the rest */

/* Read what a child has sent: a heartbeat restarts its silence clock, anything else is logged.
 * On EOF or an error our end is closed; the exit is logged when the child is reaped, and a
 * child that stays silent is dealt with by the heartbeat deadline. */
static void drain_ctl(child* c, int64_t now)
{
    for (unsigned k = 0; k < DRAIN_MAX; k++) {
        gt_msg_hdr h;
        uint8_t    buf[GT_MSG_MAX];
        int        n = ipc_recv(c->ctl_fd, &h, buf, sizeof buf);

        if (n >= 0 && h.type == GT_MSG_HEARTBEAT) {
            gt_msg_heartbeat hb;

            memcpy(&hb, buf, sizeof hb);
            child_heartbeat(c, now, hb.state);
        } else if (n >= 0 || n == IPC_BAD) {
            log_event("WARN", "child=%s bad_message", c->name);
        } else {
            if (n != -1 || errno != EAGAIN) {
                if (n != IPC_EOF) {
                    log_event("ERROR", "child=%s recv failed errno=%d", c->name, errno);
                }
                close_fd(c->ctl_fd);
                c->ctl_fd = -1;
            }
            return;
        }
    }
}

static int run(child* ch, size_t n, const hb_limits* lim)
{
    sigset_t      want;
    sigset_t      orig;
    struct pollfd pfd[1 + MAX_CHILDREN]; /* [0] signals, [1 + i] child i's control socket */
    bool          stopping = false;
    int           rc = GT_EXIT_FATAL;

    if (sigemptyset(&want) != 0 || sigaddset(&want, SIGCHLD) != 0 ||
        sigaddset(&want, SIGTERM) != 0 || sigaddset(&want, SIGINT) != 0 ||
        sigprocmask(SIG_BLOCK, &want, &orig) != 0) {
        log_event("ERROR", "signal mask setup failed errno=%d", errno);
        return rc;
    }
    pfd[0] = (struct pollfd){.fd = signalfd(-1, &want, SFD_CLOEXEC | SFD_NONBLOCK), .events = POLLIN};
    if (pfd[0].fd < 0) {
        log_event("ERROR", "signalfd failed errno=%d", errno);
        return rc;
    }

    for (;;) {
        struct signalfd_siginfo si;
        int64_t                 now = now_ms();
        int64_t                 next = -1;
        size_t                  live = 0;
        bool                    reap = false;

        for (size_t i = 0; i < n; i++) {
            child*  c = &ch[i];
            int64_t ev;

            if (c->pid == 0 && !c->stopped && c->restart_at_ms <= now && spawn(c, &orig, now) != 0) {
                schedule_restart(c, now, lim);
            }
            ev = child_next_event(c, lim);
            if (ev >= 0 && (next < 0 || ev < next)) {
                next = ev;
            }
            pfd[1 + i] = (struct pollfd){.fd = c->ctl_fd, .events = POLLIN};
            live += c->stopped ? 0 : 1;
        }
        if (live == 0) {
            rc = stopping ? 0 : GT_EXIT_FATAL;
            log_event(stopping ? "INFO" : "ERROR", stopping ? "stopped" : "no_children_left");
            break;
        }
        if (poll(pfd, 1 + n, next < 0 ? -1 : (int)(next > now ? next - now : 0)) < 0 &&
            errno != EINTR) {
            log_event("ERROR", "poll failed errno=%d", errno);
            break;
        }
        now = now_ms();
        while (read(pfd[0].fd, &si, sizeof si) == (ssize_t)sizeof si) {
            if (si.ssi_signo == SIGCHLD) {
                reap = true;
            } else if (!stopping) {
                stopping = true;
                stop_all(ch, n, now, lim);
            }
        }
        if (reap) {
            reap_children(ch, n, stopping, now, lim);
        }
        for (size_t i = 0; i < n; i++) { /* a reaped child has no socket left to read */
            if (pfd[1 + i].revents != 0 && ch[i].ctl_fd >= 0) {
                drain_ctl(&ch[i], now);
            }
            child_watch(&ch[i], now, lim);
        }
    }

    for (size_t i = 0; i < n; i++) {
        close_fd(ch[i].ctl_fd);
    }
    close_fd(pfd[0].fd);
    return rc;
}

/* Sets the limit named by `flag` (--soft, --hard, --grace, --max or --window) to the whole number
 * `val`, 1..3600; --max is 1..GT_RESTART_MAX. */
static bool set_limit(hb_limits* lim, const char* flag, const char* val)
{
    unsigned*     f = strcmp(flag, "--soft") == 0     ? &lim->soft_s
                      : strcmp(flag, "--hard") == 0   ? &lim->hard_s
                      : strcmp(flag, "--grace") == 0  ? &lim->grace_s
                      : strcmp(flag, "--max") == 0    ? &lim->restart_max
                      : strcmp(flag, "--window") == 0 ? &lim->window_s
                                                      : NULL;
    char*         end;
    unsigned long v = strtoul(val, &end, 10);

    if (f == NULL || end == val || *end != '\0' || v < 1 ||
        v > (f == &lim->restart_max ? GT_RESTART_MAX : 3600)) {
        return false;
    }
    *f = (unsigned)v;
    return true;
}

int main(int argc, char** argv)
{
    static const char* const defaults[] = {"./gt-hub", "./gt-sensor-soil", "./gt-sensor-light",
                                           "./gt-interface"};
    const char* const*       paths = defaults;
    size_t                   n = sizeof defaults / sizeof defaults[0];
    hb_limits                lim = {GT_HEARTBEAT_SOFT_S, GT_HEARTBEAT_HARD_S, GT_KILL_GRACE_S,
                                    GT_RESTART_MAX, GT_RESTART_WINDOW_S};
    child                    ch[MAX_CHILDREN];
    int                      a = 1;
    bool                     ok = true;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        return puts("gt-supervisor 0.1.0") == EOF ? 1 : 0;
    }
    for (; ok && a + 1 < argc && strncmp(argv[a], "--", 2) == 0; a += 2) {
        ok = set_limit(&lim, argv[a], argv[a + 1]);
    }
    if (argc > a) {
        paths = (const char* const*)(argv + a);
        n = (size_t)(argc - a);
    }
    if (!ok || n > MAX_CHILDREN || (argc > a && argv[a][0] == '-') || lim.soft_s >= lim.hard_s) {
        (void)fprintf(stderr, "usage: %s [--soft S] [--hard S] [--grace S] [--max N] [--window S] "
                      "[CHILD_PATH...] (at most %d)\n", argv[0], MAX_CHILDREN);
        return GT_EXIT_CONFIG;
    }
    for (size_t i = 0; i < n; i++) {
        child_init(&ch[i], paths[i]);
    }
    return run(ch, n, &lim);
}
