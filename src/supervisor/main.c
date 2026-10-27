/* gt-supervisor: starts the hub and sensor daemons, reaps them, restarts the ones that
 * die.  Slice one.  Slice two adds heartbeat deadlines, SIGKILL after a SIGTERM grace
 * period, crash-loop DEGRADED and an events.log file; until then the control sockets are
 * not read and a child that ignores SIGTERM stalls shutdown.
 *
 * usage: gt-supervisor [CHILD_PATH...]   default: ./gt-hub ./gt-sensor-soil ./gt-sensor-light
 *        gt-supervisor --version
 * Exit: 0 clean shutdown, 1 no child left to supervise, 2 bad arguments. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "child.h"
#include "ipc.h"
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

static void reap_children(child* ch, size_t n, bool stopping, int64_t now)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (ch[i].pid == pid) {
                child_exited(&ch[i], status, stopping, now);
            }
        }
    }
    if (pid < 0 && errno != ECHILD) {
        log_event("ERROR", "waitpid failed errno=%d", errno);
    }
}

static void stop_all(child* ch, size_t n)
{
    log_event("INFO", "stopping children=%zu", n);
    for (size_t i = 0; i < n; i++) {
        if (ch[i].pid == 0) {
            ch[i].stopped = true;
        } else if (kill(ch[i].pid, SIGTERM) != 0 && errno != ESRCH) {
            log_event("ERROR", "child=%s kill failed errno=%d", ch[i].name, errno);
        }
    }
}

static int run(child* ch, size_t n)
{
    sigset_t      want;
    sigset_t      orig;
    struct pollfd pfd;
    bool          stopping = false;
    int           rc = GT_EXIT_FATAL;

    if (sigemptyset(&want) != 0 || sigaddset(&want, SIGCHLD) != 0 ||
        sigaddset(&want, SIGTERM) != 0 || sigaddset(&want, SIGINT) != 0 ||
        sigprocmask(SIG_BLOCK, &want, &orig) != 0) {
        log_event("ERROR", "signal mask setup failed errno=%d", errno);
        return rc;
    }
    pfd.fd = signalfd(-1, &want, SFD_CLOEXEC | SFD_NONBLOCK);
    pfd.events = POLLIN;
    if (pfd.fd < 0) {
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
            child* c = &ch[i];

            if (c->pid == 0 && !c->stopped && c->restart_at_ms <= now && spawn(c, &orig) != 0) {
                schedule_restart(c, now);
            }
            if (c->pid == 0 && !c->stopped && (next < 0 || c->restart_at_ms < next)) {
                next = c->restart_at_ms;
            }
            live += c->stopped ? 0 : 1;
        }
        if (live == 0) {
            rc = stopping ? 0 : GT_EXIT_FATAL;
            log_event(stopping ? "INFO" : "ERROR", stopping ? "stopped" : "no_children_left");
            break;
        }
        if (poll(&pfd, 1, next < 0 ? -1 : (int)(next > now ? next - now : 0)) < 0 &&
            errno != EINTR) {
            log_event("ERROR", "poll failed errno=%d", errno);
            break;
        }
        now = now_ms();
        while (read(pfd.fd, &si, sizeof si) == (ssize_t)sizeof si) {
            if (si.ssi_signo == SIGCHLD) {
                reap = true;
            } else if (!stopping) {
                stopping = true;
                stop_all(ch, n);
            }
        }
        if (reap) {
            reap_children(ch, n, stopping, now);
        }
    }

    for (size_t i = 0; i < n; i++) {
        close_fd(ch[i].ctl_fd);
    }
    close_fd(pfd.fd);
    return rc;
}

int main(int argc, char** argv)
{
    static const char* const defaults[] = {"./gt-hub", "./gt-sensor-soil", "./gt-sensor-light"};
    const char* const*       paths = defaults;
    size_t                   n = sizeof defaults / sizeof defaults[0];
    child                    ch[MAX_CHILDREN];

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        return puts("gt-supervisor 0.1.0") == EOF ? 1 : 0;
    }
    if (argc > 1) {
        paths = (const char* const*)(argv + 1);
        n = (size_t)(argc - 1);
    }
    if (n > MAX_CHILDREN || (argc > 1 && argv[1][0] == '-')) {
        (void)fprintf(stderr, "usage: %s [CHILD_PATH...] (at most %d)\n", argv[0], MAX_CHILDREN);
        return GT_EXIT_CONFIG;
    }
    for (size_t i = 0; i < n; i++) {
        child_init(&ch[i], paths[i]);
    }
    return run(ch, n);
}
