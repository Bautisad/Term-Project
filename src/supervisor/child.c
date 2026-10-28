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

int spawn(child* c, const sigset_t* mask)
{
    int   sv[2];
    pid_t pid;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        log_event("ERROR", "child=%s socketpair failed errno=%d", c->name, errno);
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
    log_event("INFO", "start child=%s pid=%d", c->name, (int)pid);
    return 0;
}

void schedule_restart(child* c, int64_t now)
{
    c->restarts++;
    c->restart_at_ms = now + (int64_t)c->backoff_s * 1000;
    log_event("INFO", "child=%s restart=%u delay=%us", c->name, c->restarts, c->backoff_s);
    c->backoff_s = c->backoff_s * 2 > GT_BACKOFF_MAX_S ? GT_BACKOFF_MAX_S : c->backoff_s * 2;
}

void child_exited(child* c, int status, bool stopping, int64_t now)
{
    bool restart = true;

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);

        log_event(code == GT_EXIT_OK ? "INFO" : "ERROR", "child=%s pid=%d exited code=%d",
                  c->name, (int)c->pid, code);
        restart = code != GT_EXIT_OK && code != GT_EXIT_CONFIG;
    } else if (WIFSIGNALED(status)) {
        log_event("ERROR", "child=%s pid=%d died signal=%d", c->name, (int)c->pid,
                  WTERMSIG(status));
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
        schedule_restart(c, now);
    }
}
