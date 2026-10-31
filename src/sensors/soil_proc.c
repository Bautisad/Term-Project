#include "soil_proc.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>

#include "sensor_log.h"
#include "sensor_util.h"

typedef struct proc {
    const soil_dev*  dev;
    const soil_opts* o;
    soil_state       st;
    uint16_t         prev;   /* status of the last good read, to log only changes */
    sensor_hub       hub;
    uint32_t         seq;    /* cycles so far: gaps show dropped messages */
    uint32_t         hb_seq;
    unsigned         fails;  /* failed cycles in a row */
    bool             hb_down;
} proc;

soil_opts soil_opts_default(void)
{
    soil_opts o = {.hub_path = GT_HUB_SOCK_DEFAULT,
                   .ctl_fd = GT_CHILD_CTL_FD,
                   .samples = 5,
                   .attempts = 3,
                   .gap_ms = 20,
                   .cycle_ms = 60000,
                   .heartbeat_ms = GT_HEARTBEAT_INTERVAL_S * 1000u,
                   .fail_limit = 5,
                   .cfg = {.lo = SOIL_LO_DEFAULT, .hi = SOIL_HI_DEFAULT, .stuck_n = SOIL_STUCK_DEFAULT}};
    return o;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* One median of `samples` quick reads, each tried up to `attempts` times. */
static bool read_cycle(const proc* p, uint16_t* median)
{
    const soil_opts* o = p->o;
    uint16_t         v[SOIL_MAX_SAMPLES];

    for (unsigned i = 0; i < o->samples; i++) {
        for (unsigned a = 1; p->dev->read(p->dev->ctx, &v[i]) != 0; a++) {
            if (a >= o->attempts) {
                slog("ERROR", "read_failed errno=%d attempt=%u/%u seq=%u", errno, a, o->attempts, p->seq);
                return false;
            }
            sleep_ms(o->gap_ms);
        }
        if (i + 1 < o->samples) {
            sleep_ms(o->gap_ms);
        }
    }
    *median = soil_median(v, o->samples);
    return true;
}

/* Returns true when the sensor has failed fail_limit cycles in a row. */
static bool cycle(proc* p)
{
    gt_msg_soil m;
    uint16_t    median = 0;
    bool        ok;

    memset(&m, 0, sizeof m);
    m.ts_unix_ms = (uint64_t)sensor_clock_ms(CLOCK_REALTIME);
    ok = read_cycle(p, &median);
    m.raw = median;
    m.status = soil_judge(&p->st, &p->o->cfg, ok, median);
    if (ok && m.status != p->prev) {
        slog(m.status != GT_ST_OK ? "WARN" : "INFO", "status=0x%02x raw=%u seq=%u", m.status, m.raw, p->seq);
        p->prev = m.status;
    }
    p->fails = ok ? 0 : p->fails + 1;
    sensor_hub_send(&p->hub, GT_MSG_SOIL, GT_SENSOR_SOIL, p->seq, &m, sizeof m);
    p->seq++;
    if (p->fails >= p->o->fail_limit) {
        slog("ERROR", "giving_up failed_cycles=%u", p->fails);
        return true;
    }
    return false;
}

int soil_run(const soil_dev* dev, const soil_opts* o)
{
    proc          p = {.dev = dev, .o = o, .hub = {.path = o->hub_path, .fd = -1}};
    struct pollfd pfd[2] = {{.fd = -1, .events = POLLIN}, {.fd = o->ctl_fd, .events = POLLIN}};
    int64_t       next_cycle;
    int64_t       next_hb;
    int           rc = GT_EXIT_FATAL;

    slog_init("soil");
    if (o->samples < 1 || o->samples > SOIL_MAX_SAMPLES || o->attempts < 1 || o->cycle_ms < 1 ||
        o->heartbeat_ms < 1 || o->fail_limit < 1) {
        slog("ERROR", "bad_options");
        return GT_EXIT_CONFIG;
    }
    pfd[0].fd = sensor_setup(o->ctl_fd);
    if (pfd[0].fd < 0) {
        return rc;
    }

    next_cycle = next_hb = sensor_clock_ms(CLOCK_MONOTONIC); /* heartbeat first, then the first reading */
    for (;;) {
        int64_t now = sensor_clock_ms(CLOCK_MONOTONIC);
        int64_t wake = next_hb < next_cycle ? next_hb : next_cycle;

        if (poll(pfd, 2, wake > now ? (int)(wake - now) : 0) < 0 && errno != EINTR) {
            slog("ERROR", "poll failed errno=%d", errno);
            goto out;
        }
        int e = sensor_stop_code(pfd);

        if (e >= 0) {
            rc = e;
            goto out;
        }
        now = sensor_clock_ms(CLOCK_MONOTONIC);
        if (now >= next_hb) {
            gt_child_state state = p.fails > 0 ? GT_CHILD_DEGRADED
                                               : (p.seq > 0 ? GT_CHILD_RUNNING : GT_CHILD_STARTING);

            e = sensor_heartbeat(o->ctl_fd, GT_SENSOR_SOIL, &p.hb_seq, state, &p.hb_down);
            if (e >= 0) {
                rc = e;
                goto out;
            }
            next_hb = now + o->heartbeat_ms;
        }
        if (now >= next_cycle) {
            if (cycle(&p)) {
                goto out;
            }
            next_cycle = sensor_next_due(next_cycle, o->cycle_ms, sensor_clock_ms(CLOCK_MONOTONIC));
        }
    }

out:
    sensor_close_fd(p.hub.fd);
    sensor_close_fd(pfd[0].fd);
    return rc;
}
