/* interface_daemon_test: the real iface_run in a forked child, with this process playing the
 * supervisor (heartbeats, closing the socket) and the browser (real TCP over loopback).
 * Routes and refusals, the store states on the page, the refresh interval, the connection limit
 * and deadline, heartbeats, exit codes, and that aborted connections leak no descriptor.
 * Linux only (prctl, /proc).  Exits 1 at the first failed check, printing file:line. */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "daemon.h"
#include "ipc.h"
#include "ipc_io.h"
#include "record.h"
#include "store.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);         \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#define WAIT_MS 4000

static char resp[IFACE_RESPONSE_CAP + 256];

typedef struct dproc {
    pid_t    pid;
    unsigned port;
    int      ctl; /* our end of the supervisor socketpair */
} dproc;

static int64_t mono_ms(void)
{
    struct timespec ts;

    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t real_ms(void)
{
    struct timespec ts;

    CHECK(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void nap(unsigned ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};

    CHECK(nanosleep(&ts, NULL) == 0);
}

/* --- a store directory ----------------------------------------------------------------- */

static void make_dir(char* dir, size_t cap)
{
    const char* base = getenv("TMPDIR");

    CHECK(snprintf(dir, cap, "%s/gt-iface-XXXXXX", base != NULL && base[0] != '\0' ? base : "/tmp") < (int)cap);
    CHECK(mkdtemp(dir) != NULL);
}

static void remove_dir(const char* dir)
{
    DIR*           d = opendir(dir);
    struct dirent* e;

    CHECK(d != NULL);
    while ((e = readdir(d)) != NULL) {
        char path[512];

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        CHECK(snprintf(path, sizeof path, "%s/%s", dir, e->d_name) < (int)sizeof path);
        CHECK(unlink(path) == 0);
    }
    CHECK(closedir(d) == 0);
    CHECK(rmdir(dir) == 0);
}

/* One record stamped now, appended and closed. */
static void add_record(const char* dir, uint32_t seq, uint16_t soil)
{
    gt_store* s = NULL;
    gt_record r;

    (void)memset(&r, 0, sizeof r);
    r.ts_unix_ms = real_ms();
    r.seq = seq;
    r.soil_raw = soil;
    CHECK(store_recover(dir, &s, NULL) == 0);
    CHECK(store_append(s, &r) == 0);
    store_close(s);
}

/* --- the daemon under test -------------------------------------------------------------- */

static iface_opts base_opts(const char* dir)
{
    iface_opts o = iface_opts_default();

    o.dir = dir;
    o.deadline_ms = 3000;
    o.refresh_ms = 1000;
    o.heartbeat_ms = 60000; /* one heartbeat at start, none to fill the socket */
    return o;
}

static void start(dproc* p, const iface_opts* base)
{
    int      sv[2];
    unsigned port = 0;
    int      lfd = iface_listen("127.0.0.1", 0, &port);

    CHECK(lfd >= 0 && port != 0);
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    p->pid = fork();
    CHECK(p->pid >= 0);
    if (p->pid == 0) {
        iface_opts o = *base;

        (void)prctl(PR_SET_PDEATHSIG, SIGKILL); /* never outlive a failed test */
        (void)close(sv[0]);
        o.ctl_fd = sv[1];
        _exit(iface_run(&o, lfd));
    }
    CHECK(close(sv[1]) == 0);
    CHECK(close(lfd) == 0); /* the child keeps the listening socket alive */
    p->ctl = sv[0];
    p->port = port;
}

/* The daemon must exit with `code` within WAIT_MS.  GT_EXIT_OK is asked for with SIGTERM and
 * GT_EXIT_IPC by closing the supervisor's socket. */
static void finish(dproc* p, int code)
{
    pid_t w = 0;
    int   st = 0;

    if (code == GT_EXIT_OK) {
        CHECK(kill(p->pid, SIGTERM) == 0);
    } else if (code == GT_EXIT_IPC) {
        CHECK(close(p->ctl) == 0);
        p->ctl = -1;
    }
    for (int i = 0; i < WAIT_MS / 10 && (w = waitpid(p->pid, &st, WNOHANG)) == 0; i++) {
        nap(10);
    }
    CHECK(w == p->pid && WIFEXITED(st) && WEXITSTATUS(st) == code);
    if (p->ctl >= 0) {
        CHECK(close(p->ctl) == 0);
    }
}

static gt_msg_heartbeat next_hb(const dproc* p)
{
    struct pollfd    pf = {.fd = p->ctl, .events = POLLIN};
    gt_msg_hdr       h;
    uint8_t          buf[GT_MSG_MAX];
    gt_msg_heartbeat hb;

    CHECK(poll(&pf, 1, WAIT_MS) == 1);
    CHECK(ipc_recv(p->ctl, &h, buf, sizeof buf) == (int)sizeof hb);
    CHECK(h.type == GT_MSG_HEARTBEAT && h.sensor_id == GT_SENSOR_NONE);
    (void)memcpy(&hb, buf, sizeof hb);
    CHECK(hb.pid == p->pid);
    return hb;
}

static int fd_count(pid_t pid)
{
    char           path[64];
    DIR*           d;
    struct dirent* e;
    int            n = 0;

    CHECK(snprintf(path, sizeof path, "/proc/%d/fd", (int)pid) > 0);
    d = opendir(path);
    CHECK(d != NULL);
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    CHECK(closedir(d) == 0);
    return n;
}

/* --- the browser ------------------------------------------------------------------------ */

static int dial(unsigned port)
{
    struct sockaddr_in sin;
    int                fd = socket(AF_INET, SOCK_STREAM, 0);

    CHECK(fd >= 0);
    (void)memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) == 1);
    CHECK(connect(fd, (const struct sockaddr*)&sin, sizeof sin) == 0);
    return fd;
}

static void send_all(int fd, const char* s)
{
    size_t n = strlen(s);

    CHECK(send(fd, s, n, MSG_NOSIGNAL) == (ssize_t)n);
}

/* Everything the server sends until it closes, into resp (NUL-terminated); its length. */
static size_t slurp(int fd)
{
    size_t  len = 0;
    int64_t end = mono_ms() + WAIT_MS;

    for (;;) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        int64_t       left = end - mono_ms();
        ssize_t       n;

        CHECK(left > 0 && poll(&pf, 1, (int)left) == 1);
        CHECK(len < sizeof resp - 1);
        n = recv(fd, resp + len, sizeof resp - 1 - len, 0);
        if (n == 0 || (n < 0 && errno == ECONNRESET)) {
            break;
        }
        CHECK(n > 0);
        len += (size_t)n;
    }
    resp[len] = '\0';
    return len;
}

static size_t get(unsigned port, const char* request)
{
    int    fd = dial(port);
    size_t n;

    send_all(fd, request);
    n = slurp(fd);
    CHECK(close(fd) == 0);
    return n;
}

static int status_of(void)
{
    int code = 0;

    CHECK(sscanf(resp, "HTTP/1.1 %d", &code) == 1);
    return code;
}

static bool has(const char* needle)
{
    return strstr(resp, needle) != NULL;
}

static const char* body(void)
{
    const char* b = strstr(resp, "\r\n\r\n");

    CHECK(b != NULL);
    return b + 4;
}

/* Content-Length says exactly how much body there is. */
static void check_length(void)
{
    const char* h = strstr(resp, "Content-Length: ");
    unsigned    n = 0;

    CHECK(h != NULL && sscanf(h, "Content-Length: %u", &n) == 1);
    CHECK(strlen(body()) == n);
}

/* --- tests ------------------------------------------------------------------------------ */

static void routes_and_refusals(void)
{
    char       dir[256];
    iface_opts o;
    dproc      p;

    make_dir(dir, sizeof dir);
    add_record(dir, 1, 600);
    o = base_opts(dir);
    start(&p, &o);

    (void)get(p.port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(status_of() == 200 && has("Content-Type: text/html") && has("Connection: close"));
    CHECK(has("Cache-Control: no-store") && strstr(body(), "<h1>Green Thumb</h1>") != NULL);
    check_length();

    (void)get(p.port, "GET /index.html HTTP/1.0\r\n\r\n");
    CHECK(status_of() == 200 && strstr(body(), "<h1>Green Thumb</h1>") != NULL);

    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200 && has("Content-Type: application/json"));
    CHECK(strstr(body(), "\"records\":1") != NULL && strstr(body(), "\"raw\":600") != NULL);
    CHECK(strstr(body(), "\"needs_water\":\"no\",\"dry_raw\":565,") != NULL); /* 600 > 565 */
    check_length();

    (void)get(p.port, "GET /nope HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 404);
    (void)get(p.port, "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    CHECK(status_of() == 405 && has("Allow: GET"));
    (void)get(p.port, "GET / HTTP/2.0\r\n\r\n");
    CHECK(status_of() == 505);
    (void)get(p.port, "GET / HTTP/1.1\n\n"); /* bare LF */
    CHECK(status_of() == 400);

    {   /* too many headers */
        char big[1536] = "GET / HTTP/1.1\r\n";

        for (int i = 0; i < 40; i++) {
            char h[32];

            CHECK(snprintf(h, sizeof h, "X-%d: y\r\n", i) < (int)sizeof h);
            CHECK(strlen(big) + strlen(h) + 3 < sizeof big);
            (void)strcat(big, h);
        }
        (void)strcat(big, "\r\n");
        (void)get(p.port, big);
        CHECK(status_of() == 431);
    }

    {   /* a request that arrives in pieces */
        int fd = dial(p.port);

        send_all(fd, "GET /stat");
        nap(50);
        send_all(fd, "us.json HTTP/1.1\r\nHo");
        nap(50);
        send_all(fd, "st: x\r\n\r\n");
        (void)slurp(fd);
        CHECK(close(fd) == 0);
        CHECK(status_of() == 200 && strstr(body(), "\"records\":1") != NULL);
    }

    finish(&p, GT_EXIT_OK);
    remove_dir(dir);
}

static void store_states(void)
{
    char       dir[256];
    char       missing[300];
    iface_opts o;
    dproc      p;

    make_dir(dir, sizeof dir);
    CHECK(snprintf(missing, sizeof missing, "%s/not-there", dir) < (int)sizeof missing);

    /* A store that is not there: the page says so and the daemon carries on. */
    o = base_opts(missing);
    start(&p, &o);
    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200 && strstr(body(), "\"state\":\"degraded\"") != NULL);
    CHECK(strstr(body(), "store_unreadable") != NULL);
    (void)get(p.port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200 && strstr(body(), "<h1>Green Thumb</h1>") != NULL);
    finish(&p, GT_EXIT_OK);

    /* An empty store, then one that grows: the page changes only after the refresh interval. */
    o = base_opts(dir);
    o.refresh_ms = 3000;
    start(&p, &o);
    {
        int64_t t0 = mono_ms();
        int64_t elapsed;

        (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
        CHECK(status_of() == 200 && strstr(body(), "\"records\":0") != NULL);
        CHECK(strstr(body(), "store_unreadable") == NULL && strstr(body(), "\"soil\":null") != NULL);

        add_record(dir, 1, 600);
        (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
        elapsed = mono_ms() - t0;
        if (elapsed < 2500) {
            CHECK(strstr(body(), "\"records\":0") != NULL); /* served from the last look */
        } else {
            (void)fprintf(stderr, "interface_daemon_test: slow machine, cache check skipped\n");
        }

        while (mono_ms() - t0 < 3100) {
            nap(50);
        }
        (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
        CHECK(status_of() == 200 && strstr(body(), "\"records\":1") != NULL);
        CHECK(strstr(body(), "\"raw\":600") != NULL);
    }
    finish(&p, GT_EXIT_OK);
    remove_dir(dir);
}

/* The dry line the daemon is given decides what the same store means. */
static void needs_water_threshold(void)
{
    char       dir[256];
    iface_opts o;
    dproc      p;

    make_dir(dir, sizeof dir);
    add_record(dir, 1, 500);

    o = base_opts(dir); /* the default line, 565: 500 is dry */
    start(&p, &o);
    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200);
    CHECK(strstr(body(), "\"needs_water\":\"yes\",\"dry_raw\":565,") != NULL);
    (void)get(p.port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200 && strstr(body(), "Yes, water this plant") != NULL);
    finish(&p, GT_EXIT_OK);

    o = base_opts(dir);
    o.dry_raw = 499; /* 500 is above this line */
    start(&p, &o);
    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
    CHECK(strstr(body(), "\"needs_water\":\"no\",\"dry_raw\":499,") != NULL);
    (void)get(p.port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(strstr(body(), "No. Soil reading 500, dry line 499") != NULL);
    CHECK(strstr(body(), "Yes, water this plant") == NULL);
    finish(&p, GT_EXIT_OK);

    o = base_opts(dir);
    o.dry_raw = 500; /* at the line is dry */
    start(&p, &o);
    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
    CHECK(strstr(body(), "\"needs_water\":\"yes\",\"dry_raw\":500,") != NULL);
    finish(&p, GT_EXIT_OK);

    /* A store that is not there: unknown, never "no". */
    {
        char missing[300];

        CHECK(snprintf(missing, sizeof missing, "%s/not-there", dir) < (int)sizeof missing);
        o = base_opts(missing);
        start(&p, &o);
        (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
        CHECK(strstr(body(), "\"needs_water\":\"unknown\"") != NULL);
        finish(&p, GT_EXIT_OK);
    }
    remove_dir(dir);
}

static void connection_limits(void)
{
    char       dir[256];
    iface_opts o;
    dproc      p;
    int        idle[IFACE_MAX_CONNS];

    make_dir(dir, sizeof dir);
    o = base_opts(dir);
    o.deadline_ms = 1500;
    start(&p, &o);

    /* Fill every slot: half say nothing, half start a request and stall. */
    for (unsigned i = 0; i < IFACE_MAX_CONNS; i++) {
        idle[i] = dial(p.port);
        if (i % 2 == 1) {
            send_all(idle[i], "GET /status.js");
        }
    }
    {   /* the next one is turned away at once */
        int64_t t0 = mono_ms();
        int     fd = dial(p.port);

        (void)slurp(fd);
        CHECK(close(fd) == 0);
        CHECK(status_of() == 503 && mono_ms() - t0 < 1000);
    }
    /* The stalled ones get a 408 when their time is up, and the slots come back. */
    for (unsigned i = 0; i < IFACE_MAX_CONNS; i++) {
        (void)slurp(idle[i]);
        CHECK(status_of() == 408);
        CHECK(close(idle[i]) == 0);
    }
    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200);

    finish(&p, GT_EXIT_OK);
    remove_dir(dir);
}

static void heartbeats(void)
{
    char             dir[256];
    char             missing[300];
    iface_opts       o;
    dproc            p;
    gt_msg_heartbeat hb;
    int              n;

    make_dir(dir, sizeof dir);
    CHECK(snprintf(missing, sizeof missing, "%s/not-there", dir) < (int)sizeof missing);
    o = base_opts(missing);
    o.heartbeat_ms = 100;
    start(&p, &o);

    hb = next_hb(&p); /* right after start, long before the 60 s the supervisor allows */
    CHECK(hb.state == GT_CHILD_STARTING);
    hb = next_hb(&p);
    CHECK(hb.state == GT_CHILD_RUNNING);

    /* A request finds the store unreadable: later heartbeats say degraded. */
    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200);
    for (n = 0; n < 20; n++) {
        hb = next_hb(&p);
        if (hb.state == GT_CHILD_DEGRADED) {
            break;
        }
    }
    CHECK(hb.state == GT_CHILD_DEGRADED);

    finish(&p, GT_EXIT_OK);
    remove_dir(dir);
}

static void exit_codes(void)
{
    char       dir[256];
    iface_opts o;
    dproc      p;
    unsigned   port = 0;
    int        lfd;

    make_dir(dir, sizeof dir);
    o = base_opts(dir);

    /* The supervisor's end closing is GT_EXIT_IPC; so is the first heartbeat after it. */
    start(&p, &o);
    (void)get(p.port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200);
    finish(&p, GT_EXIT_IPC);

    /* Options that make no sense are GT_EXIT_CONFIG, and are refused before anything starts. */
    o = base_opts(dir);
    o.dir = NULL;
    CHECK(iface_run(&o, 0) == GT_EXIT_CONFIG);
    o = base_opts(dir);
    o.deadline_ms = 0;
    CHECK(iface_run(&o, 0) == GT_EXIT_CONFIG);
    o = base_opts(dir);
    o.refresh_ms = 0;
    CHECK(iface_run(&o, 0) == GT_EXIT_CONFIG);
    o = base_opts(dir);
    o.heartbeat_ms = 0;
    CHECK(iface_run(&o, 0) == GT_EXIT_CONFIG);
    o = base_opts(dir);
    CHECK(iface_run(&o, -1) == GT_EXIT_CONFIG);
    CHECK(iface_run(NULL, 0) == GT_EXIT_CONFIG);

    /* The listening socket: a port in use, a bad address and a bad port. */
    lfd = iface_listen("127.0.0.1", 0, &port);
    CHECK(lfd >= 0);
    CHECK(iface_listen("127.0.0.1", port, NULL) == -1 && errno == EADDRINUSE);
    CHECK(iface_listen("not-an-address", 0, NULL) == -1 && errno == EINVAL);
    CHECK(iface_listen("127.0.0.1", 70000, NULL) == -1 && errno == EINVAL);
    CHECK(iface_listen(NULL, 0, NULL) == -1 && errno == EINVAL);
    CHECK(close(lfd) == 0);

    remove_dir(dir);
}

/* Browsers that leave in every way they can: no descriptor stays behind and the daemon still
 * answers. */
static void aborted_connections(void)
{
    char       dir[256];
    iface_opts o;
    dproc      p;
    int        before;
    int        after = 0;

    make_dir(dir, sizeof dir);
    add_record(dir, 1, 600);
    o = base_opts(dir);
    start(&p, &o);
    (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n"); /* anything lazily opened is open now */
    before = fd_count(p.pid);

    for (int i = 0; i < 60; i++) { /* close without a word */
        CHECK(close(dial(p.port)) == 0);
    }
    for (int i = 0; i < 60; i++) { /* leave in the middle of a request */
        int fd = dial(p.port);

        send_all(fd, "GET /stat");
        CHECK(close(fd) == 0);
    }
    for (int i = 0; i < 40; i++) { /* ask, then reset before reading the answer */
        struct linger lg = {.l_onoff = 1, .l_linger = 0};
        int           fd = dial(p.port);

        send_all(fd, i % 2 == 0 ? "GET / HTTP/1.1\r\n\r\n" : "GET /status.json HTTP/1.1\r\n\r\n");
        CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) == 0);
        CHECK(close(fd) == 0);
    }
    for (int i = 0; i < 40; i++) { /* ask, then shut the sending side and walk away */
        int fd = dial(p.port);

        send_all(fd, "GET / HTTP/1.1\r\n\r\n");
        CHECK(shutdown(fd, SHUT_RDWR) == 0);
        CHECK(close(fd) == 0);
    }

    /* Mid-flood a 503 is the connection limit working, so it is no failure; but once the daemon has
     * worked through the pile it must answer properly, and soon. */
    {
        int64_t end = mono_ms() + 3000;

        do {
            (void)get(p.port, "GET /status.json HTTP/1.1\r\n\r\n");
            if (status_of() == 503) {
                nap(50);
            }
        } while (status_of() == 503 && mono_ms() < end);
        CHECK(status_of() == 200 && strstr(body(), "\"records\":1") != NULL);
    }
    for (int i = 0; i < 100; i++) { /* the daemon may still be clearing the last of them */
        after = fd_count(p.pid);
        if (after == before) {
            break;
        }
        nap(20);
    }
    CHECK(after == before);

    finish(&p, GT_EXIT_OK);
    remove_dir(dir);
}

int main(void)
{
    routes_and_refusals();
    store_states();
    needs_water_threshold();
    connection_limits();
    heartbeats();
    exit_codes();
    aborted_connections();
    (void)puts("PASS");
    return 0;
}
