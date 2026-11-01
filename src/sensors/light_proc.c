#include "light_proc.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

#include "sensor_log.h"
#include "sensor_util.h"

typedef struct proc {
    const light_dev*  dev;
    const light_opts* o;
    light_state       st;
    uint8_t           prev;   /* status of the last window, to log only changes */
    sensor_hub        hub;
    uint32_t          seq;    /* windows so far: gaps show dropped messages */
    uint32_t          hb_seq;
    bool              hb_down;
} proc;

light_opts light_opts_default(void)
{
    light_opts o = {.hub_path = GT_HUB_SOCK_DEFAULT,
                    .ctl_fd = GT_CHILD_CTL_FD,
                    .window_ms = 60000,
                    .heartbeat_ms = GT_HEARTBEAT_INTERVAL_S * 1000u,
                    .cfg = {.stuck_ms = LIGHT_STUCK_MS_DEFAULT, .chatter_edges = LIGHT_CHATTER_EDGES_DEFAULT}};
    return o;
}

/* Feed every waiting change to the window.  False if reading the pin failed. */
static bool drain(proc* p)
{
    bool level;
    int  r;

    while ((r = p->dev->next(p->dev->ctx, &level)) > 0) {
        light_edge(&p->st, level, sensor_clock_ms(CLOCK_MONOTONIC));
    }
    if (r < 0) {
        slog("ERROR", "read_failed errno=%d", errno);
    }
    return r == 0;
}

/* End the window at `now` and send it. */
static void send_window(proc* p, int64_t now)
{
    gt_msg_light m;

    memset(&m, 0, sizeof m);
    m.ts_unix_ms = (uint64_t)sensor_clock_ms(CLOCK_REALTIME);
    light_close(&p->st, &p->o->cfg, now, &m);
    if (m.status != p->prev) {
        slog(m.status != GT_ST_OK ? "WARN" : "INFO", "status=0x%02x level=%u edges=%u seq=%u", m.status,
             m.level, m.edges, p->seq);
        p->prev = m.status;
    }
    sensor_hub_send(&p->hub, GT_MSG_LIGHT, GT_SENSOR_LIGHT, p->seq++, &m, sizeof m);
}

int light_run(const light_dev* dev, const light_opts* o)
{
    proc          p = {.dev = dev, .o = o, .hub = {.path = o->hub_path, .fd = -1}};
    struct pollfd pfd[3] = {{.fd = -1, .events = POLLIN},
                            {.fd = o->ctl_fd, .events = POLLIN},
                            {.fd = dev->fd, .events = POLLIN}};
    int64_t       next_window;
    int64_t       next_hb;
    int           rc = GT_EXIT_FATAL;

    slog_init("light");
    if (o->window_ms < 1 || o->window_ms > UINT16_MAX || o->heartbeat_ms < 1) {
        slog("ERROR", "bad_options");
        return GT_EXIT_CONFIG;
    }
    pfd[0].fd = sensor_setup(o->ctl_fd);
    if (pfd[0].fd < 0) {
        return rc;
    }

    next_hb = sensor_clock_ms(CLOCK_MONOTONIC); /* heartbeat first */
    light_init(&p.st, dev->level, next_hb);
    next_window = next_hb + o->window_ms;
    for (;;) {
        int64_t now = sensor_clock_ms(CLOCK_MONOTONIC);
        int64_t wake = next_hb < next_window ? next_hb : next_window;

        if (poll(pfd, 3, wake > now ? (int)(wake - now) : 0) < 0 && errno != EINTR) {
            slog("ERROR", "poll failed errno=%d", errno);
            goto out;
        }
        int e = sensor_stop_code(pfd);

        if (e >= 0) {
            rc = e;
            goto out;
        }
        if (pfd[2].revents != 0 && !drain(&p)) {
            goto out;
        }
        now = sensor_clock_ms(CLOCK_MONOTONIC);
        if (now >= next_hb) {
            gt_child_state state = p.prev != GT_ST_OK ? GT_CHILD_DEGRADED
                                                      : (p.seq > 0 ? GT_CHILD_RUNNING : GT_CHILD_STARTING);

            e = sensor_heartbeat(o->ctl_fd, GT_SENSOR_LIGHT, &p.hb_seq, state, &p.hb_down);
            if (e >= 0) {
                rc = e;
                goto out;
            }
            next_hb = now + o->heartbeat_ms;
        }
        if (now >= next_window) {
            send_window(&p, now);
            next_window = sensor_next_due(next_window, o->window_ms, now);
        }
    }

out:
    sensor_close_fd(p.hub.fd);
    sensor_close_fd(pfd[0].fd);
    return rc;
}
