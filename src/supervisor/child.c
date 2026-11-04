#define _GNU_SOURCE /* close_range */
#include "child.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc.h"
#include "sup_log.h"

void child_init(child* c, const char* path)
{
    const char* slash = strrchr(path, '/');

    memset(c, 0, sizeof *c);
    c->path = path;
    c->name = slash != NULL ? slash + 1 : path;
    c->ctl_fd = -1;
    c->backoff_s = GT_BACKOFF_MIN_S;
}

/* Runs in the forked child: put the control socket on fd 3, restore the signal mask
 * (a blocked mask survives exec), then exec.  Never returns. */
static void child_exec(const char* path, int ctl, const sigset_t* mask)
{
    char* const argv[] = {(char*)path, NULL};

    if (ctl != GT_CHILD_CTL_FD) {
        if (dup2(ctl, GT_CHILD_CTL_FD) < 0) {
            _exit(GT_EXIT_FATAL);
        }
    } else if (fcntl(ctl, F_SETFD, 0) != 0) { /* already fd 3: clear CLOEXEC */
        _exit(GT_EXIT_FATAL);
    }
    if (sigprocmask(SIG_SETMASK, mask, NULL) != 0) {
        _exit(GT_EXIT_FATAL);
    }
    if (close_range(GT_CHILD_CTL_FD + 1, ~0U, 0) != 0) { /* drop fds the launcher leaked */
        _exit(GT_EXIT_FATAL);
    }
    execv(path, argv);
    (void)fprintf(stderr, "gt-supervisor: exec %s failed errno=%d\n", path, errno);
    _exit(GT_EXIT_CONFIG);
}

int spawn(child* c, const sigset_t* mask, int64_t now)
{
    int   sv[2];
    pid_t pid;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        log_event("ERROR", "child=%s socketpair failed errno=%d", c->name, errno);
        return -1;
    }
    if (fcntl(sv[0], F_SETFL, O_NONBLOCK) != 0) { /* the supervisor never blocks on a child */
        log_event("ERROR", "child=%s control socket setup failed errno=%d", c->name, errno);
        close_fd(sv[0]);
        close_fd(sv[1]);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        log_event("ERROR", "child=%s fork failed errno=%d", c->name, errno);
        close_fd(sv[0]);
        close_fd(sv[1]);
        return -1;
    }
    if (pid == 0) {
        child_exec(c->path, sv[1], mask);
    }
    close_fd(sv[1]);
    c->pid = pid;
    c->ctl_fd = sv[0];
    c->started_ms = now;
    c->last_hb_ms = now;
    c->kill_at_ms = 0;
    c->state = GT_CHILD_STARTING;
    c->warned = false;
    c->hung = false;
    log_event("INFO", "start child=%s pid=%d", c->name, (int)pid);
    return 0;
}

void schedule_restart(child* c, int64_t now, const hb_limits* l)
{
    size_t   ring = sizeof c->recent / sizeof c->recent[0];
    unsigned k = ++c->restarts;

    c->recent[k % ring] = now;
    /* restart k and the l->restart_max before it all fall inside the window: a crash loop */
    if (k > l->restart_max && now - c->recent[(k - l->restart_max) % ring] <= (int64_t)l->window_s * 1000) {
        if (!c->degraded) {
            log_event("ERROR", "child=%s crash_loop restarts=%u window=%us state=DEGRADED", c->name,
                      l->restart_max + 1, l->window_s);
        }
        c->degraded = true;
        c->backoff_s = GT_BACKOFF_MAX_S;
    }
    c->restart_at_ms = now + (int64_t)c->backoff_s * 1000;
    log_event("INFO", "child=%s restart=%u delay=%us", c->name, c->restarts, c->backoff_s);
    c->backoff_s = c->backoff_s * 2 > GT_BACKOFF_MAX_S ? GT_BACKOFF_MAX_S : c->backoff_s * 2;
}

static void send_sig(child* c, int sig)
{
    if (kill(c->pid, sig) != 0 && errno != ESRCH) {
        log_event("ERROR", "child=%s kill failed signal=%d errno=%d", c->name, sig, errno);
    }
}

void child_heartbeat(child* c, int64_t now, uint32_t state)
{
    static const char* const names[] = {"starting", "running", "degraded"};

    if (state != c->state && state < sizeof names / sizeof names[0]) {
        log_event(state == GT_CHILD_DEGRADED ? "WARN" : "INFO", "child=%s state=%s", c->name,
                  names[state]);
        c->state = state;
    }
    c->last_hb_ms = now;
    c->warned = false;
}

void child_term(child* c, int64_t now, const hb_limits* l)
{
    send_sig(c, SIGTERM);
    c->kill_at_ms = now + (int64_t)l->grace_s * 1000;
}

void child_watch(child* c, int64_t now, const hb_limits* l)
{
    long long age_s = (now - c->last_hb_ms) / 1000;

    if (c->pid == 0 || c->kill_at_ms < 0) {
        return;
    }
    if (now - c->started_ms >= (int64_t)l->window_s * 1000) { /* up a full window: stable again */
        if (c->degraded) {
            log_event("INFO", "child=%s recovered state=running", c->name);
        }
        c->degraded = false;
        c->backoff_s = GT_BACKOFF_MIN_S;
    }
    if (c->kill_at_ms > 0) { /* SIGTERM was sent: has the grace run out? */
        if (now >= c->kill_at_ms) {
            log_event("ERROR", "child=%s killed signal=SIGKILL", c->name);
            send_sig(c, SIGKILL);
            c->kill_at_ms = -1;
        }
    } else if (age_s >= l->hard_s) {
        log_event("ERROR", "child=%s heartbeat_timeout age=%llds signal=SIGTERM", c->name, age_s);
        c->hung = true;
        child_term(c, now, l);
    } else if (age_s >= l->soft_s && !c->warned) {
        log_event("WARN", "child=%s heartbeat_missed age=%llds", c->name, age_s);
        c->warned = true;
    }
}

int64_t child_next_event(const child* c, const hb_limits* l)
{
    if (c->pid == 0) {
        return c->stopped ? -1 : c->restart_at_ms;
    }
    if (c->kill_at_ms != 0) {
        return c->kill_at_ms > 0 ? c->kill_at_ms : -1;
    }
    return c->last_hb_ms + (int64_t)(c->warned ? l->hard_s : l->soft_s) * 1000;
}

void child_exited(child* c, int status, bool stopping, int64_t now, const hb_limits* l)
{
    bool restart = true;
    bool fault = c->hung;

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);

        log_event(code == GT_EXIT_OK ? "INFO" : "ERROR", "child=%s pid=%d exited code=%d",
                  c->name, (int)c->pid, code);
        restart = c->hung || (code != GT_EXIT_OK && code != GT_EXIT_CONFIG);
        fault = fault || code != GT_EXIT_OK;
    } else if (WIFSIGNALED(status)) {
        log_event("ERROR", "child=%s pid=%d died signal=%d", c->name, (int)c->pid,
                  WTERMSIG(status));
        fault = true;
    }
    if (fault && !stopping) {
        c->faults++;
    }
    c->pid = 0;
    close_fd(c->ctl_fd);
    c->ctl_fd = -1;
    if (stopping || !restart) {
        c->stopped = true;
        if (!stopping) {
            log_event("INFO", "child=%s no_restart", c->name);
        }
    } else {
        schedule_restart(c, now, l);
    }
}
