/* hub_proc_test: runs the real hub_run loop in a child process against stand-in sensors and a
 * stand-in supervisor (proc_rig.h), with a real store under /tmp read back by store_read_range. */
#define _GNU_SOURCE /* prlimit */
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "proc_rig.h"
#include "hub_proc.h"
#include "store.h"

/* The flags without TIME_UNSET, so the test does not depend on the machine's clock. */
#define FLAGS(r) ((uint16_t)((r).flags & ~GT_REC_TIME_UNSET))

static char   base_dir[96];
static char   store_dir[128];
static rlim_t file_cap = RLIM_INFINITY; /* the largest file the next hub may write */

/* A fresh store directory per test. */
static void new_store(void)
{
    static int n;

    CHECK(snprintf(store_dir, sizeof store_dir, "%s/s%d", base_dir, n++) > 0);
}

/* Short cycles so the tests take milliseconds; idle is long so only idle_records sees it. */
static hub_opts fast(void)
{
    return (hub_opts){.sock_path = hub_path, .store_dir = store_dir, .grace_ms = 100,
                      .idle_ms = 60000, .heartbeat_ms = 25};
}

/* The hub in a child process; our end of its supervisor socketpair is r->ctl. */
static void start(rig* r, hub_opts o)
{
    struct rlimit lim = {.rlim_cur = file_cap, .rlim_max = RLIM_INFINITY};
    int           sv[2];

    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    r->id = GT_SENSOR_NONE;
    r->hub = r->listener = -1;
    r->pid = fork();
    CHECK(r->pid >= 0);
    if (r->pid == 0) {
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL); /* never outlive a failed test */
        (void)close(sv[0]);
        o.ctl_fd = sv[1];
        if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE, &lim) != 0) {
            _exit(99);
        }
        _exit(hub_run(&o));
    }
    CHECK(close(sv[1]) == 0);
    r->ctl = sv[0];
}

/* The first heartbeat comes once the hub is listening, so after this a sensor can connect. */
static void start_ready(rig* r, hub_opts o)
{
    start(r, o);
    CHECK(next_hb(r).state == GT_CHILD_STARTING);
}

static int connect_hub(void)
{
    int fd = ipc_connect(hub_path);

    CHECK(fd >= 0);
    return fd;
}

#define NOW_MS ((uint64_t)time(NULL) * 1000)

static void send_soil(int fd, uint16_t raw, uint16_t status)
{
    gt_msg_soil m = {.ts_unix_ms = NOW_MS, .raw = raw, .status = status};

    CHECK(ipc_send(fd, GT_MSG_SOIL, GT_SENSOR_SOIL, 0, &m, sizeof m) == 0);
}

/* Soil 800 on s; on l, light with 30 s bright, 3 edges, bright at the end. */
static void send_pair(int s, int l)
{
    gt_msg_light m = {.ts_unix_ms = NOW_MS, .bright_ms = 30000, .edges = 3, .level = 1};

    send_soil(s, 800, GT_ST_OK);
    CHECK(ipc_send(l, GT_MSG_LIGHT, GT_SENSOR_LIGHT, 0, &m, sizeof m) == 0);
}

typedef struct got {
    gt_record rec[8];
    size_t    n;
} got;

static int collect(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    got* g = ctx;

    CHECK(status == GT_STORE_REC_OK);
    if (g->n < sizeof g->rec / sizeof g->rec[0]) {
        g->rec[g->n++] = *rec;
    }
    return 0;
}

/* Wait until the store holds at least n records, then leave them in *g. */
static void wait_records(got* g, size_t n)
{
    for (int i = 0; i < WAIT_MS / 10; i++) {
        g->n = 0;
        CHECK(store_read_range(store_dir, 0, UINT64_MAX, collect, g) == 0);
        if (g->n >= n) {
            return;
        }
        nap(10);
    }
    CHECK(false);
}

static int count_fds(pid_t pid)
{
    char           path[64];
    int            n = 0;
    DIR*           d;
    struct dirent* e;

    CHECK(snprintf(path, sizeof path, "/proc/%d/fd", (int)pid) > 0);
    CHECK((d = opendir(path)) != NULL);
    while ((e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    CHECK(closedir(d) == 0);
    return n;
}

/* Both sensors, then soil alone (the light never comes), then a restart on the same store. */
static void records_and_resume(void)
{
    rig r;
    got g;
    int s;
    int l;

    new_store();
    start_ready(&r, fast());
    s = connect_hub();
    l = connect_hub();
    send_pair(s, l);
    wait_records(&g, 1);
    CHECK(g.rec[0].seq == 0 && g.rec[0].soil_raw == 800 && g.rec[0].light_bright_ms == 30000);
    CHECK(g.rec[0].light_edges == 3 && FLAGS(g.rec[0]) == GT_REC_LIGHT_BRIGHT);
    expect_state(&r, GT_CHILD_RUNNING);

    send_soil(s, 801, GT_ST_OK);
    wait_records(&g, 2);
    CHECK(g.rec[1].seq == 1 && g.rec[1].soil_raw == 801 && FLAGS(g.rec[1]) == GT_REC_LIGHT_INVALID);
    shut(&s);
    shut(&l);
    rig_end(&r, GT_EXIT_OK);

    start_ready(&r, fast()); /* the numbering carries on after the last record in the store */
    s = connect_hub();
    send_soil(s, 802, GT_ST_OK);
    wait_records(&g, 3);
    CHECK(g.rec[2].seq == 2 && g.rec[2].soil_raw == 802 && g.rec[0].seq == 0);
    shut(&s);
    rig_end(&r, GT_EXIT_OK);
}

/* Garbage and a heartbeat on the sensor socket are dropped and the connection keeps working.
 * Then a sensor that connects, sends and goes away, again and again, leaves no descriptors. */
static void clients(void)
{
    rig              r;
    got              g;
    uint8_t          zeros[sizeof(gt_msg_hdr) + sizeof(gt_msg_soil)] = {0};
    gt_msg_heartbeat hb = {.pid = 1, .state = GT_CHILD_RUNNING};
    int              s;
    int              base = 0;

    new_store();
    start_ready(&r, fast());
    s = connect_hub();
    CHECK(send(s, "junk", 4, MSG_NOSIGNAL) == 4); /* shorter than a header */
    CHECK(send(s, zeros, sizeof zeros, MSG_NOSIGNAL) == (ssize_t)sizeof zeros); /* bad magic */
    CHECK(ipc_send(s, GT_MSG_HEARTBEAT, GT_SENSOR_SOIL, 0, &hb, sizeof hb) == 0);
    send_soil(s, 800, GT_ST_OK);
    wait_records(&g, 1);
    CHECK(g.n == 1 && g.rec[0].soil_raw == 800);
    shut(&s);

    for (size_t i = 2; i <= 6; i++) {
        s = connect_hub();
        send_soil(s, 800, GT_ST_OK);
        shut(&s);
        wait_records(&g, i);
        if (i == 3) { /* by now the segment file is open and the first client is gone */
            base = count_fds(r.pid);
        }
    }
    CHECK(count_fds(r.pid) == base);
    rig_end(&r, GT_EXIT_OK);
}

/* With no sensor at all, records with both readings invalid still appear. */
static void idle_records(void)
{
    rig      r;
    got      g;
    hub_opts o = fast();

    new_store();
    o.idle_ms = 200;
    start_ready(&r, o);
    wait_records(&g, 2);
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(g.rec[i].seq == i && FLAGS(g.rec[i]) == (GT_REC_SOIL_INVALID | GT_REC_LIGHT_INVALID));
    }
    rig_end(&r, GT_EXIT_OK);
}

/* The third record does not fit (a file size cap, so the write fails with EFBIG): the hub
 * reports DEGRADED and keeps running, and once there is room the next record follows the second
 * with no gap in the numbers. */
static void store_failure(void)
{
    rig           r;
    got           g;
    struct rlimit lim = {.rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY};
    int           s;
    int           l;

    new_store();
    file_cap = 2 * GT_RECORD_SIZE;
    start_ready(&r, fast());
    file_cap = RLIM_INFINITY;
    s = connect_hub();
    l = connect_hub();
    for (size_t i = 1; i <= 2; i++) {
        send_pair(s, l);
        wait_records(&g, i);
    }
    send_pair(s, l);
    expect_state(&r, GT_CHILD_DEGRADED);
    CHECK(prlimit(r.pid, RLIMIT_FSIZE, &lim, NULL) == 0);
    send_pair(s, l);
    wait_records(&g, 3);
    CHECK(g.rec[2].seq == 2);
    expect_state(&r, GT_CHILD_RUNNING);
    shut(&s);
    shut(&l);
    rig_end(&r, GT_EXIT_OK);
}

/* The exit codes the supervisor's restart policy keys off. */
static void exit_codes(void)
{
    rig      a;
    rig      b;
    hub_opts bad = fast();

    new_store();
    start_ready(&a, fast());
    start(&b, fast()); /* the store is in use */
    rig_end(&b, GT_EXIT_FATAL);
    shut(&a.ctl); /* the supervisor goes away */
    rig_end(&a, GT_EXIT_IPC);

    bad.grace_ms = 0; /* out of range: a config error, not a restartable failure */
    start(&b, bad);
    rig_end(&b, GT_EXIT_CONFIG);
}

int main(void)
{
    char cmd[128];

    CHECK(snprintf(hub_path, sizeof hub_path, "/tmp/hub_proc_%d.sock", (int)getpid()) > 0);
    CHECK(snprintf(base_dir, sizeof base_dir, "/tmp/hub_proc_%d", (int)getpid()) > 0);
    CHECK(mkdir(base_dir, 0700) == 0);

    records_and_resume();
    clients();
    idle_records();
    store_failure();
    exit_codes();

    (void)unlink(hub_path);
    CHECK(snprintf(cmd, sizeof cmd, "rm -rf %s", base_dir) > 0 && system(cmd) == 0);
    (void)puts("hub_proc_test: ok");
    return 0;
}
