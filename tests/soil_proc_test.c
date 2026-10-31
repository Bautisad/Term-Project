/* soil_proc_test: runs the real soil_run loop in a child process with a fake sensor, against a
 * stand-in hub and supervisor (see proc_rig.h). */
#include "proc_rig.h"
#include "soil_proc.h"

static const int STEADY[] = {600};

/* The fake sensor replays a script: a value is a reading, -1 is a failed read. */
static const int* g_script;
static size_t     g_len;
static size_t     g_pos;

static int fake_read(void* ctx, uint16_t* raw)
{
    int v = g_script[g_pos++ % g_len];

    (void)ctx;
    if (v < 0) {
        errno = EIO;
        return -1;
    }
    *raw = (uint16_t)v;
    return 0;
}

typedef struct job {
    const int* script;
    size_t     len;
    soil_opts  o;
} job;

static int soil_child(int ctl_fd, const void* arg)
{
    job      j = *(const job*)arg;
    soil_dev dev = {.read = fake_read};

    g_script = j.script;
    g_len = j.len;
    j.o.ctl_fd = ctl_fd;
    return soil_run(&dev, &j.o);
}

/* Short cycles so the tests take milliseconds, not minutes. */
static soil_opts fast(void)
{
    soil_opts o = soil_opts_default();

    o.hub_path = hub_path;
    o.samples = 3;
    o.gap_ms = 1;
    o.cycle_ms = 40;
    o.heartbeat_ms = 25;
    o.fail_limit = 3;
    return o;
}

static void start(rig* r, const int* script, size_t len, soil_opts o)
{
    job j = {script, len, o};

    rig_start(r, GT_SENSOR_SOIL, soil_child, &j);
}

static gt_msg_soil next_soil(const rig* r, gt_msg_hdr* h)
{
    gt_msg_soil m;

    next_hub_msg(r, h, GT_MSG_SOIL, &m, sizeof m);
    CHECK(m.ts_unix_ms != 0 && m.reserved == 0);
    return m;
}

/* What the sensor reads, and the first n messages the hub should then see.  A zero field
 * keeps fast()'s setting; code is how the process ends (0, GT_EXIT_OK: we stop it). */
typedef struct msg_case {
    int      script[8];
    size_t   len;
    unsigned samples, attempts, stuck_n;
    size_t   n;
    uint16_t raw[3], status[3];
    int      code;
} msg_case;

static const msg_case CASES[] = {
    /* the median of three samples */
    {.script = {600, 602, 601}, .len = 3, .n = 3,
     .raw = {601, 601, 601}, .status = {GT_ST_OK, GT_ST_OK, GT_ST_OK}},
    /* out of range: what the sensor returns right after i2cdetect */
    {.script = {0xffff}, .len = 1, .n = 1, .raw = {0xffff}, .status = {GT_ST_INVALID}},
    /* the same value three cycles in a row */
    {.script = {700}, .len = 1, .stuck_n = 3, .n = 3,
     .raw = {700, 700, 700}, .status = {GT_ST_OK, GT_ST_OK, GT_ST_STUCK}},
    /* the first two tries of the first sample fail, the third succeeds */
    {.script = {-1, -1, 600, 600, 600, 600, 600, 600}, .len = 8, .samples = 1, .n = 3,
     .raw = {600, 600, 600}, .status = {GT_ST_OK, GT_ST_OK, GT_ST_OK}},
    /* fail_limit unreadable cycles: each is sent as INVALID, then the process gives up */
    {.script = {-1}, .len = 1, .samples = 1, .attempts = 1, .n = 3,
     .raw = {0, 0, 0}, .status = {GT_ST_INVALID, GT_ST_INVALID, GT_ST_INVALID},
     .code = GT_EXIT_FATAL},
};

static void expect_msgs(const msg_case* c)
{
    soil_opts  o = fast();
    rig        r;
    gt_msg_hdr h;

    o.samples = c->samples != 0 ? c->samples : o.samples;
    o.attempts = c->attempts != 0 ? c->attempts : o.attempts;
    o.cfg.stuck_n = c->stuck_n != 0 ? c->stuck_n : o.cfg.stuck_n;
    start(&r, c->script, c->len, o);
    rig_accept(&r);
    for (size_t i = 0; i < c->n; i++) {
        gt_msg_soil m = next_soil(&r, &h);

        CHECK(h.seq == i && m.raw == c->raw[i] && m.status == c->status[i]);
    }
    if (c->code != GT_EXIT_OK) { /* giving up shows as DEGRADED first */
        expect_state(&r, GT_CHILD_DEGRADED);
    }
    rig_end(&r, c->code);
}

/* The first heartbeat comes before the first reading; later ones report RUNNING. */
static void heartbeats(void)
{
    rig r;

    start(&r, STEADY, 1, fast());
    CHECK(next_hb(&r).state == GT_CHILD_STARTING);
    expect_state(&r, GT_CHILD_RUNNING);
    rig_end(&r, GT_EXIT_OK);
}

/* The hub goes away and comes back: messages in between are dropped, and the sequence gap
 * shows it. */
static void hub_restart(void)
{
    rig        r;
    gt_msg_hdr h;
    uint32_t   first;

    start(&r, STEADY, 1, fast());
    rig_accept(&r);
    (void)next_soil(&r, &h);
    first = h.seq;
    shut(&r.hub);
    shut(&r.listener);
    nap(150);
    r.listener = ipc_listen(hub_path);
    CHECK(r.listener >= 0);
    rig_accept(&r);
    (void)next_soil(&r, &h);
    CHECK(h.seq > first + 1);
    rig_end(&r, GT_EXIT_OK);
}

/* Start the loop and expect it to end by itself with `code`, after we close the supervisor's
 * socket if asked. */
static void ends_alone(soil_opts o, bool drop_ctl, int code)
{
    rig r;

    start(&r, STEADY, 1, o);
    if (drop_ctl) {
        shut(&r.ctl);
    }
    rig_end(&r, code);
}

int main(void)
{
    soil_opts bad = fast();

    bad.samples = 0; /* out of range: a config error, not a restartable failure */
    CHECK(snprintf(hub_path, sizeof hub_path, "/tmp/soil_proc_%d.sock", (int)getpid()) > 0);

    heartbeats();
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        expect_msgs(&CASES[i]);
    }
    hub_restart();
    ends_alone(fast(), true, GT_EXIT_IPC);
    ends_alone(bad, false, GT_EXIT_CONFIG);

    (void)unlink(hub_path);
    (void)puts("soil_proc_test: ok");
    return 0;
}
