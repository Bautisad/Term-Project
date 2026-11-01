/* light_proc_test: runs the real light_run loop in a child process with a fake pin, against a
 * stand-in hub and supervisor (see proc_rig.h).  The pin is a pipe: each byte we write is one
 * change of the flag, '1' bright and '0' dark. */
#include <fcntl.h>

#include "light_proc.h"
#include "proc_rig.h"

static int pin[2];

static int pin_next(void* ctx, bool* level)
{
    const light_dev* dev = ctx;
    char             c;
    ssize_t          n = read(dev->fd, &c, 1);

    if (n == 1) {
        *level = c == '1';
        return 1;
    }
    if (n < 0 && errno == EAGAIN) {
        return 0;
    }
    errno = EIO; /* the pin went away */
    return -1;
}

typedef struct job {
    bool       level;
    light_opts o;
} job;

static int light_child(int ctl_fd, const void* arg)
{
    job       j = *(const job*)arg;
    light_dev dev = {.fd = pin[0], .level = j.level, .next = pin_next};

    dev.ctx = &dev;
    (void)close(pin[1]);
    j.o.ctl_fd = ctl_fd;
    return light_run(&dev, &j.o);
}

/* Short windows so the tests take a second, not minutes. */
static light_opts fast(void)
{
    light_opts o = light_opts_default();

    o.hub_path = hub_path;
    o.window_ms = 200;
    o.heartbeat_ms = 25;
    return o;
}

/* Start the loop with the flag at `level`. */
static void start(rig* r, bool level, light_opts o)
{
    job j = {level, o};

    CHECK(pipe(pin) == 0 && fcntl(pin[0], F_SETFL, O_NONBLOCK) == 0);
    rig_start(r, GT_SENSOR_LIGHT, light_child, &j);
    shut(&pin[0]);
}

static void finish(rig* r, int code)
{
    rig_end(r, code);
    shut(&pin[1]);
}

static void change(bool bright)
{
    CHECK(write(pin[1], bright ? "1" : "0", 1) == 1);
}

static gt_msg_light next_window(const rig* r, gt_msg_hdr* h)
{
    gt_msg_light m;

    next_hub_msg(r, h, GT_MSG_LIGHT, &m, sizeof m);
    CHECK(m.ts_unix_ms != 0 && m.reserved == 0);
    return m;
}

/* The first heartbeat comes first; later ones report RUNNING once a window has gone out. */
static void heartbeats(void)
{
    rig r;

    start(&r, false, fast());
    CHECK(next_hb(&r).state == GT_CHILD_STARTING);
    expect_state(&r, GT_CHILD_RUNNING);
    finish(&r, GT_EXIT_OK);
}

/* Windows carry the level, the number of changes and the time spent bright. */
static void windows(void)
{
    rig          r;
    gt_msg_hdr   h;
    gt_msg_light m;

    start(&r, false, fast());
    rig_accept(&r);
    m = next_window(&r, &h); /* quiet and dark */
    CHECK(h.seq == 0 && m.level == 0 && m.edges == 0 && m.bright_ms == 0 && m.status == GT_ST_OK);
    change(true);
    m = next_window(&r, &h); /* bright for nearly the whole window */
    CHECK(h.seq == 1 && m.level == 1 && m.edges == 1 && m.status == GT_ST_OK);
    CHECK(m.bright_ms > 100 && m.bright_ms < 400);
    change(false);
    change(true);
    m = next_window(&r, &h);
    CHECK(h.seq == 2 && m.level == 1 && m.edges == 2);
    finish(&r, GT_EXIT_OK);
}

/* More changes than chatter_edges in one window is INVALID and DEGRADED; the next quiet window
 * is fine again. */
static void chatter(void)
{
    rig          r;
    gt_msg_hdr   h;
    gt_msg_light m;

    start(&r, false, fast());
    rig_accept(&r);
    (void)next_window(&r, &h);
    for (unsigned i = 0; i <= LIGHT_CHATTER_EDGES_DEFAULT; i++) {
        change(i % 2 == 0);
    }
    m = next_window(&r, &h);
    CHECK(h.seq == 1 && m.edges == LIGHT_CHATTER_EDGES_DEFAULT + 1 && m.status == GT_ST_INVALID);
    expect_state(&r, GT_CHILD_DEGRADED);
    m = next_window(&r, &h);
    CHECK(h.seq == 2 && m.edges == 0 && m.status == GT_ST_OK);
    finish(&r, GT_EXIT_OK);
}

/* The hub goes away and comes back: windows in between are dropped, and the sequence gap
 * shows it. */
static void hub_restart(void)
{
    rig        r;
    gt_msg_hdr h;
    uint32_t   first;

    start(&r, false, fast());
    rig_accept(&r);
    (void)next_window(&r, &h);
    first = h.seq;
    shut(&r.hub);
    shut(&r.listener);
    nap(500);
    r.listener = ipc_listen(hub_path);
    CHECK(r.listener >= 0);
    rig_accept(&r);
    (void)next_window(&r, &h);
    CHECK(h.seq > first + 1);
    finish(&r, GT_EXIT_OK);
}

/* Start the loop and expect it to end by itself with `code`: after we close the supervisor's
 * socket or the pin, if asked. */
static void ends_alone(light_opts o, bool drop_ctl, bool drop_pin, int code)
{
    rig r;

    start(&r, false, o);
    if (drop_ctl) {
        shut(&r.ctl);
    }
    if (drop_pin) {
        shut(&pin[1]);
    }
    finish(&r, code);
}

int main(void)
{
    light_opts bad = fast();

    bad.window_ms = UINT16_MAX + 1u; /* bright_ms is 16 bits: a longer window is a format change */
    CHECK(snprintf(hub_path, sizeof hub_path, "/tmp/light_proc_%d.sock", (int)getpid()) > 0);

    heartbeats();
    windows();
    chatter();
    hub_restart();
    ends_alone(fast(), true, false, GT_EXIT_IPC);
    ends_alone(fast(), false, true, GT_EXIT_FATAL);
    ends_alone(bad, false, false, GT_EXIT_CONFIG);

    (void)unlink(hub_path);
    (void)puts("light_proc_test: ok");
    return 0;
}
