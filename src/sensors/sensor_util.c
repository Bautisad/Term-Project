#include "sensor_util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ipc_io.h"
#include "sensor_log.h"

int64_t sensor_clock_ms(clockid_t clk)
{
    struct timespec ts;

    if (clock_gettime(clk, &ts) != 0) {
        slog("ERROR", "clock_gettime failed errno=%d", errno);
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sensor_close_fd(int fd)
{
    if (fd >= 0 && close(fd) != 0) {
        slog("ERROR", "close failed errno=%d", errno);
    }
}

bool sensor_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void sensor_track(bool* down, bool ok, const char* what, int err)
{
    if (ok == !*down) {
        return;
    }
    *down = !ok;
    if (ok) {
        slog("INFO", "%s recovered", what);
    } else {
        slog("WARN", "%s failed errno=%d", what, err);
    }
}

bool sensor_parse_uint(const char* s, unsigned long max, unsigned long* out)
{
    char* end;

    errno = 0;
    *out = strtoul(s, &end, 10);
    return *s >= '0' && *s <= '9' && *end == '\0' && errno == 0 && *out <= max;
}

int sensor_ctl_fd(int fd)
{
    if (fcntl(fd, F_GETFD) >= 0) {
        return fd;
    }
    slog("WARN", "no control socket on fd %d: running without heartbeats", fd);
    return -1;
}

int sensor_setup(int ctl_fd)
{
    sigset_t sigs;
    int      fd = -1;

    if (sigemptyset(&sigs) != 0 || sigaddset(&sigs, SIGTERM) != 0 || sigaddset(&sigs, SIGINT) != 0 ||
        sigprocmask(SIG_BLOCK, &sigs, NULL) != 0 ||
        (fd = signalfd(-1, &sigs, SFD_CLOEXEC | SFD_NONBLOCK)) < 0) {
        slog("ERROR", "signal setup failed errno=%d", errno);
        return -1;
    }
    if (ctl_fd >= 0 && !sensor_set_nonblock(ctl_fd)) {
        slog("ERROR", "control socket setup failed errno=%d", errno);
        sensor_close_fd(fd);
        return -1;
    }
    return fd;
}

/* Discard whatever the supervisor sent; true if its end is closed. */
static bool ctl_closed(int fd)
{
    char    buf[GT_MSG_MAX];
    ssize_t n;

    while ((n = recv(fd, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
    }
    return n == 0 || (errno != EAGAIN && errno != EINTR);
}

int sensor_stop_code(const struct pollfd* pfd)
{
    if (pfd[0].revents != 0) {
        slog("INFO", "stopping");
        return GT_EXIT_OK;
    }
    if (pfd[1].revents != 0 && ctl_closed(pfd[1].fd)) {
        slog("ERROR", "supervisor_lost");
        return GT_EXIT_IPC;
    }
    return -1;
}

int64_t sensor_next_due(int64_t due, unsigned period_ms, int64_t now)
{
    due += period_ms;
    return due > now ? due : now + period_ms;
}

int sensor_heartbeat(int ctl_fd, gt_sensor_id id, uint32_t* seq, gt_child_state state, bool* down)
{
    gt_msg_heartbeat hb = {.pid = (int32_t)getpid(), .state = (uint32_t)state};
    bool             ok;
    int              e;

    if (ctl_fd < 0) {
        return -1;
    }
    ok = ipc_send(ctl_fd, GT_MSG_HEARTBEAT, id, (*seq)++, &hb, sizeof hb) == 0;
    e = errno;
    if (!ok && (e == EPIPE || e == ECONNRESET)) {
        slog("ERROR", "supervisor_lost errno=%d", e);
        return GT_EXIT_IPC;
    }
    sensor_track(down, ok, "heartbeat", e); /* EAGAIN: the supervisor is not draining its end */
    return -1;
}

void sensor_hub_send(sensor_hub* hub, gt_msg_type type, gt_sensor_id id, uint32_t seq,
                     const void* payload, size_t len)
{
    bool ok = false;
    int  e;

    if (hub->fd >= 0 || ((hub->fd = ipc_connect(hub->path)) >= 0 && sensor_set_nonblock(hub->fd))) {
        ok = ipc_send(hub->fd, type, id, seq, payload, len) == 0;
    }
    e = errno;
    if (!ok && e != EAGAIN) { /* includes a connection we could not make nonblocking */
        sensor_close_fd(hub->fd);
        hub->fd = -1;
    }
    sensor_track(&hub->down, ok, "hub_send", e);
}
