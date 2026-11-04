/* interface_lifecycle_test: runs the real gt-interface binary the way the supervisor does (the
 * control socket on fd 3, no arguments but the test's own) next to a real gt-feeder, and checks
 * what the supervisor and a browser can see: exit codes, heartbeats, the page following a store
 * that appears, no stray descriptors, and a clean stop.  argv[1] is gt-interface and argv[2] is
 * gt-feeder.  Linux only (prctl, /proc, pidfd_getfd).  Exits 1 at the first failed check. */
#define _GNU_SOURCE /* syscall, SO_DOMAIN */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "daemon.h"
#include "ipc.h"
#include "ipc_io.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);         \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#define WAIT_MS  5000
#define MAX_LIVE 8

static const char* g_iface;
static const char* g_feeder;
static char        resp[IFACE_RESPONSE_CAP + 256];

/* Children started and not yet reaped: a failing CHECK exits with some still running, so the
 * exit handler kills what is left. */
static pid_t g_live[MAX_LIVE];

static void live_add(pid_t pid)
{
    for (size_t i = 0; i < MAX_LIVE; i++) {
        if (g_live[i] == 0) {
            g_live[i] = pid;
            return;
        }
    }
    (void)kill(pid, SIGKILL);
    CHECK(!"too many live children");
}

static void live_remove(pid_t pid)
{
    for (size_t i = 0; i < MAX_LIVE; i++) {
        if (g_live[i] == pid) {
            g_live[i] = 0;
        }
    }
}

static void kill_live(void)
{
    for (size_t i = 0; i < MAX_LIVE; i++) {
        if (g_live[i] != 0) {
            (void)kill(g_live[i], SIGKILL);
            (void)waitpid(g_live[i], NULL, 0);
            g_live[i] = 0;
        }
    }
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap(unsigned ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};

    CHECK(nanosleep(&ts, NULL) == 0);
}

/* --- a temporary directory -------------------------------------------------------------- */

static void make_tmp(char* dir, size_t cap)
{
    const char* base = getenv("TMPDIR");

    CHECK(snprintf(dir, cap, "%s/gt-life-XXXXXX", base != NULL && base[0] != '\0' ? base : "/tmp") < (int)cap);
    CHECK(mkdtemp(dir) != NULL);
}

static void join(char* out, size_t cap, const char* dir, const char* name)
{
    CHECK(snprintf(out, cap, "%s/%s", dir, name) < (int)cap);
}

/* Removes dir and what is in it (files, and directories to any depth the test makes). */
static void remove_tree(const char* dir)
{
    DIR*           d = opendir(dir);
    struct dirent* e;

    CHECK(d != NULL);
    while ((e = readdir(d)) != NULL) {
        char        path[512];
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        join(path, sizeof path, dir, e->d_name);
        CHECK(lstat(path, &st) == 0);
        if (S_ISDIR(st.st_mode)) {
            remove_tree(path);
        } else {
            CHECK(unlink(path) == 0);
        }
    }
    CHECK(closedir(d) == 0);
    CHECK(rmdir(dir) == 0);
}

/* --- starting and stopping programs ----------------------------------------------------- */

/* Run path with args (NULL-terminated, argv[0] is added).  stderr goes to `log`.  The child's
 * fd 3 is ctl_child (the supervisor's socket) or closed; ctl_parent is our end, closed there. */
static pid_t spawn(const char* path, const char* const* args, const char* log, int ctl_child, int ctl_parent)
{
    char* argv[16];
    int   n = 0;
    pid_t pid;

    argv[n++] = (char*)path;
    for (size_t i = 0; args != NULL && args[i] != NULL; i++) {
        CHECK(n < 15);
        argv[n++] = (char*)args[i];
    }
    argv[n] = NULL;

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int lf;

        (void)prctl(PR_SET_PDEATHSIG, SIGKILL); /* never outlive a failed test */
        if (ctl_parent >= 0) {
            (void)close(ctl_parent);
        }
        lf = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (lf < 0 || dup2(lf, 2) < 0) {
            _exit(126);
        }
        if (ctl_child == 3) {
            if (fcntl(3, F_SETFD, 0) != 0) { /* keep it across exec */
                _exit(126);
            }
        } else if (ctl_child >= 0) {
            if (dup2(ctl_child, 3) < 0) {
                _exit(126);
            }
        } else {
            (void)close(3); /* nothing may look like a supervisor socket */
        }
        execv(path, argv);
        _exit(127);
    }
    live_add(pid);
    return pid;
}

/* The exit code of pid, which must have exited normally within `ms`. */
static int wait_exit(pid_t pid, int ms)
{
    pid_t w = 0;
    int   st = 0;

    for (int i = 0; i < ms / 10 && (w = waitpid(pid, &st, WNOHANG)) == 0; i++) {
        nap(10);
    }
    CHECK(w == pid && WIFEXITED(st));
    live_remove(pid);
    return WEXITSTATUS(st);
}

/* The port from the "listening addr=... port=N" line gt-interface logs, once it is there. */
static unsigned wait_listening(const char* log)
{
    int64_t end = mono_ms() + WAIT_MS;

    for (;;) {
        char  buf[4096];
        FILE* f = fopen(log, "r");

        if (f != NULL) {
            size_t n = fread(buf, 1, sizeof buf - 1, f);

            CHECK(!ferror(f));
            CHECK(fclose(f) == 0);
            buf[n] = '\0';
            {
                const char* l = strstr(buf, "listening addr=");
                const char* p = l != NULL ? strstr(l, "port=") : NULL;

                if (p != NULL && strchr(p, '\n') != NULL) { /* the whole line, not half of it */
                    return (unsigned)strtoul(p + 5, NULL, 10);
                }
            }
        }
        CHECK(mono_ms() < end);
        nap(20);
    }
}

static bool log_has(const char* log, const char* needle)
{
    char  buf[8192];
    FILE* f = fopen(log, "r");
    size_t n;

    CHECK(f != NULL);
    n = fread(buf, 1, sizeof buf - 1, f);
    CHECK(!ferror(f));
    CHECK(fclose(f) == 0);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* --- the supervisor's side -------------------------------------------------------------- */

static gt_msg_heartbeat next_hb(int ctl, pid_t pid)
{
    struct pollfd    pf = {.fd = ctl, .events = POLLIN};
    gt_msg_hdr       h;
    uint8_t          buf[GT_MSG_MAX];
    gt_msg_heartbeat hb;

    CHECK(poll(&pf, 1, WAIT_MS) == 1);
    CHECK(ipc_recv(ctl, &h, buf, sizeof buf) == (int)sizeof hb);
    CHECK(h.type == GT_MSG_HEARTBEAT && h.sensor_id == GT_SENSOR_NONE);
    (void)memcpy(&hb, buf, sizeof hb);
    CHECK(hb.pid == pid);
    return hb;
}

/* Heartbeats until one reports `state`, within `tries`. */
static void expect_hb_state(int ctl, pid_t pid, gt_child_state state, int tries)
{
    gt_msg_heartbeat hb = {0, 0};

    for (int i = 0; i < tries; i++) {
        hb = next_hb(ctl, pid);
        if (hb.state == (uint32_t)state) {
            return;
        }
    }
    CHECK(hb.state == (uint32_t)state);
}

/* --- descriptors ------------------------------------------------------------------------ */

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

/* Past stdin, stdout and stderr the daemon may hold sockets (the supervisor's, the listener, the
 * connections) and its signalfd, and nothing else: no store directory, no log file. */
static void check_fd_kinds(pid_t pid)
{
    char           path[64];
    DIR*           d;
    struct dirent* e;

    CHECK(snprintf(path, sizeof path, "/proc/%d/fd", (int)pid) > 0);
    d = opendir(path);
    CHECK(d != NULL);
    while ((e = readdir(d)) != NULL) {
        char    link[128];
        char    target[128];
        ssize_t n;

        if (e->d_name[0] == '.' || atoi(e->d_name) < 3) {
            continue;
        }
        CHECK(snprintf(link, sizeof link, "%s/%s", path, e->d_name) < (int)sizeof link);
        n = readlink(link, target, sizeof target - 1);
        CHECK(n > 0);
        target[n] = '\0';
        if (strncmp(target, "socket:", 7) != 0 && strncmp(target, "anon_inode:", 11) != 0) {
            (void)fprintf(stderr, "stray fd %s -> %s\n", e->d_name, target);
            CHECK(!"stray descriptor");
        }
    }
    CHECK(closedir(d) == 0);
}

/* --- sockets: nothing leaves the machine ------------------------------------------------ */

/* The other half of tools/nonetcheck.sh: a text scan cannot see a call made through a macro or a
 * function pointer, but the kernel can say what the running process holds.  Each of the daemon's
 * sockets is borrowed with pidfd_getfd (Linux 5.6) and asked its family, type and port.  /proc/net
 * is no use for this: an unbound UDP socket is not listed there at all.  Every socket must be the
 * supervisor's AF_UNIX link, the page's one listener, or a TCP connection accepted on the
 * listener's port; a TCP socket on any other local port is one the daemon opened itself. */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434 /* the same number on every architecture that has it */
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

static void check_sockets(pid_t pid, unsigned port, unsigned min_conns)
{
    char           path[64];
    DIR*           d;
    struct dirent* e;
    unsigned       listeners = 0;
    unsigned       conns = 0;
    int            pidfd = (int)syscall(SYS_pidfd_open, pid, 0);

    if (pidfd < 0) {
        (void)fprintf(stderr, "pidfd_open failed, errno %d\n", errno);
        CHECK(!"cannot inspect the daemon's sockets (needs Linux 5.6)");
    }
    CHECK(snprintf(path, sizeof path, "/proc/%d/fd", (int)pid) > 0);
    d = opendir(path);
    CHECK(d != NULL);
    while ((e = readdir(d)) != NULL) {
        char               link[128];
        char               target[128];
        ssize_t            n;
        int                theirs;
        int                mine;
        int                domain = 0;
        int                type = 0;
        int                accepting = 0;
        socklen_t          len;
        struct sockaddr_in local;

        if (e->d_name[0] == '.' || (theirs = atoi(e->d_name)) < 3) {
            continue;
        }
        CHECK(snprintf(link, sizeof link, "%s/%s", path, e->d_name) < (int)sizeof link);
        n = readlink(link, target, sizeof target - 1);
        if (n < 0 && errno == ENOENT) {
            continue; /* closed while we were looking */
        }
        CHECK(n > 0);
        target[n] = '\0';
        if (strncmp(target, "socket:[", 8) != 0) {
            continue; /* the signalfd; check_fd_kinds has judged what is not a socket */
        }
        mine = (int)syscall(SYS_pidfd_getfd, pidfd, theirs, 0);
        if (mine < 0 && errno == EBADF) {
            continue; /* closed since the readlink */
        }
        if (mine < 0) {
            (void)fprintf(stderr, "pidfd_getfd failed, errno %d\n", errno);
            CHECK(!"cannot borrow the daemon's socket (ptrace permission, or Linux before 5.6)");
        }
        len = sizeof domain;
        CHECK(getsockopt(mine, SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0);
        len = sizeof type;
        CHECK(getsockopt(mine, SOL_SOCKET, SO_TYPE, &type, &len) == 0);
        if (domain == AF_UNIX) {
            CHECK(type == SOCK_SEQPACKET); /* the supervisor's link, nothing else is local */
        } else if (domain == AF_INET) {
            len = sizeof accepting;
            CHECK(getsockopt(mine, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &len) == 0);
            len = sizeof local;
            CHECK(getsockname(mine, (struct sockaddr*)&local, &len) == 0);
            if (type != SOCK_STREAM || ntohs(local.sin_port) != port) {
                (void)fprintf(stderr, "fd %d: AF_INET type %d on local port %u, the page is TCP on %u\n",
                              theirs, type, (unsigned)ntohs(local.sin_port), port);
                CHECK(!"an internet socket that is not the page's listener or a connection to it");
            }
            if (accepting != 0) {
                listeners++;
            } else {
                conns++;
            }
        } else {
            (void)fprintf(stderr, "fd %d: socket family %d\n", theirs, domain);
            CHECK(!"a socket that is neither local nor the page's own");
        }
        CHECK(close(mine) == 0);
    }
    CHECK(closedir(d) == 0);
    CHECK(close(pidfd) == 0);
    CHECK(listeners == 1);           /* we did see the listener, so the inspection works */
    CHECK(conns >= min_conns);       /* and the connections we are holding */
}

/* --- the browser ------------------------------------------------------------------------ */

static int dial(unsigned port)
{
    struct sockaddr_in sin;
    int                fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

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

/* Everything the server sends until it closes, into resp (NUL-terminated). */
static void slurp(int fd)
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
}

static void get(unsigned port, const char* request)
{
    int fd = dial(port);

    send_all(fd, request);
    slurp(fd);
    CHECK(close(fd) == 0);
}

static int status_of(void)
{
    int code = 0;

    CHECK(sscanf(resp, "HTTP/1.1 %d", &code) == 1);
    return code;
}

static const char* body(void)
{
    const char* b = strstr(resp, "\r\n\r\n");

    CHECK(b != NULL);
    return b + 4;
}

/* The status page; true if it came back 200. */
static bool get_status(unsigned port)
{
    get(port, "GET /status.json HTTP/1.1\r\n\r\n");
    return status_of() == 200;
}

static uint64_t json_u64(const char* key)
{
    const char* p = strstr(body(), key);

    CHECK(p != NULL);
    return strtoull(p + strlen(key), NULL, 10);
}

/* The page's reading count once the store is readable and holds at least `min` records. */
static uint64_t wait_records(unsigned port, uint64_t min)
{
    int64_t end = mono_ms() + 2 * WAIT_MS;

    for (;;) {
        if (get_status(port) && strstr(body(), "store_unreadable") == NULL && json_u64("\"records\":") >= min) {
            return json_u64("\"records\":");
        }
        CHECK(mono_ms() < end);
        nap(100);
    }
}

/* --- tests ------------------------------------------------------------------------------ */

static void bad_arguments(const char* tmp)
{
    static const char* const BAD[][6] = {
        {"--bogus", "x", NULL},
        {"stray", NULL},
        {"--port", NULL},
        {"--port", "x", NULL},
        {"--port", "70000", NULL},
        {"--dir", NULL},
        {"--bind", "nonsense", "--port", "0", NULL},
        {"--heartbeat-ms", "0", NULL},
        {"--heartbeat-ms", "30001", NULL},
        {"--refresh-ms", "0", NULL},
        {"--refresh-ms", "3600001", NULL},
        {"--dry", NULL},
        {"--dry", "x", NULL},
        {"--dry", "-1", NULL},
        {"--dry", "65536", NULL},
    };
    char log[300];

    join(log, sizeof log, tmp, "bad.log");
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        pid_t pid = spawn(g_iface, BAD[i], log, -1, -1);

        if (wait_exit(pid, WAIT_MS) != GT_EXIT_CONFIG) {
            (void)fprintf(stderr, "case %zu (%s) did not exit with GT_EXIT_CONFIG\n", i, BAD[i][0]);
            CHECK(!"bad arguments");
        }
    }
}

static void port_in_use(const char* tmp)
{
    char        log[300];
    char        port[16];
    unsigned    p = 0;
    int         lfd = iface_listen("127.0.0.1", 0, &p);
    const char* args[8];
    pid_t       pid;

    CHECK(lfd >= 0);
    CHECK(snprintf(port, sizeof port, "%u", p) < (int)sizeof port);
    args[0] = "--bind";
    args[1] = "127.0.0.1";
    args[2] = "--port";
    args[3] = port;
    args[4] = NULL;
    join(log, sizeof log, tmp, "inuse.log");
    pid = spawn(g_iface, args, log, -1, -1);
    CHECK(wait_exit(pid, WAIT_MS) == GT_EXIT_FATAL);
    CHECK(log_has(log, "listen failed"));
    CHECK(close(lfd) == 0);
}

static void supervisor_lost(const char* tmp)
{
    char        log[300];
    char        store[300];
    int         sv[2];
    const char* args[10];
    pid_t       pid;

    join(log, sizeof log, tmp, "ipc.log");
    join(store, sizeof store, tmp, "ipc-store");
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    args[0] = "--dir";
    args[1] = store;
    args[2] = "--bind";
    args[3] = "127.0.0.1";
    args[4] = "--port";
    args[5] = "0";
    args[6] = NULL;
    pid = spawn(g_iface, args, log, sv[1], sv[0]);
    CHECK(close(sv[1]) == 0);
    (void)wait_listening(log);
    (void)next_hb(sv[0], pid); /* it is up and talking */
    CHECK(close(sv[0]) == 0);
    CHECK(wait_exit(pid, WAIT_MS) == GT_EXIT_IPC);
    CHECK(log_has(log, "supervisor_lost"));
}

/* Browsers that leave in every way they can. */
static void flood(unsigned port)
{
    for (int i = 0; i < 100; i++) { /* close without a word */
        CHECK(close(dial(port)) == 0);
    }
    for (int i = 0; i < 50; i++) { /* leave in the middle of a request */
        int fd = dial(port);

        send_all(fd, "GET /stat");
        CHECK(close(fd) == 0);
    }
    for (int i = 0; i < 50; i++) { /* ask, then reset before reading the answer */
        struct linger lg = {.l_onoff = 1, .l_linger = 0};
        int           fd = dial(port);

        send_all(fd, i % 2 == 0 ? "GET / HTTP/1.1\r\n\r\n" : "GET /status.json HTTP/1.1\r\n\r\n");
        CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) == 0);
        CHECK(close(fd) == 0);
    }
}

static void full_run(const char* tmp)
{
    char        log[300];
    char        flog[300];
    char        store[300];
    int         sv[2];
    const char* args[16];
    const char* fargs[8];
    char        rate[8] = "5";
    unsigned    port;
    pid_t       pid;
    pid_t       feeder;
    uint64_t    n1;
    uint64_t    n2;
    int         before;
    int         after = 0;
    unsigned    again = 0;
    int         lfd;

    join(log, sizeof log, tmp, "run.log");
    join(flog, sizeof flog, tmp, "feeder.log");
    join(store, sizeof store, tmp, "store"); /* not there yet */
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    args[0] = "--dir";
    args[1] = store;
    args[2] = "--bind";
    args[3] = "127.0.0.1";
    args[4] = "--port";
    args[5] = "0";
    args[6] = "--refresh-ms";
    args[7] = "200";
    args[8] = "--heartbeat-ms";
    args[9] = "200";
    args[10] = NULL;
    pid = spawn(g_iface, args, log, sv[1], sv[0]);
    CHECK(close(sv[1]) == 0);
    port = wait_listening(log);

    /* First sign of life: the first heartbeat, long before the 60 s the supervisor allows. */
    CHECK(next_hb(sv[0], pid).state == GT_CHILD_STARTING);

    /* No store yet: the daemon stays up, the page and the heartbeat both say so. */
    CHECK(get_status(port));
    CHECK(strstr(body(), "\"state\":\"degraded\"") != NULL && strstr(body(), "store_unreadable") != NULL);
    CHECK(strstr(body(), "\"source\":\"none\"") != NULL); /* no store, so no source to name */
    expect_hb_state(sv[0], pid, GT_CHILD_DEGRADED, 20);

    /* The store appears (a real feeder writing five records a second) and the page follows it,
     * with no restart. */
    fargs[0] = "--dir";
    fargs[1] = store;
    fargs[2] = "--rate";
    fargs[3] = rate;
    fargs[4] = NULL;
    feeder = spawn(g_feeder, fargs, flog, -1, -1);
    n1 = wait_records(port, 1);
    CHECK(strstr(body(), "\"soil\":{") != NULL);
    /* Everything the real feeder wrote is labeled synthetic: in its own log, in the JSON and on
     * the page. */
    CHECK(log_has(flog, "source=SYNTHETIC"));
    CHECK(strstr(body(), "\"source\":\"synthetic\"") != NULL);
    CHECK(json_u64("\"synthetic_records\":") == json_u64("\"records\":"));
    get(port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(status_of() == 200 && strstr(body(), "SYNTHETIC DATA:") != NULL);
    n2 = wait_records(port, n1 + 3);
    CHECK(n2 > n1);
    (void)get_status(port); /* keep the heartbeat state fresh: a request is what looks at the store */
    expect_hb_state(sv[0], pid, GT_CHILD_RUNNING, 20);

    /* Descriptors: only sockets and the signalfd, and the same number after a rough crowd. */
    check_fd_kinds(pid);
    before = fd_count(pid);
    {   /* three browsers mid-request: their sockets are the only TCP ones besides the listener */
        int     held[3];
        int64_t end = mono_ms() + 3000;

        for (size_t i = 0; i < 3; i++) {
            held[i] = dial(port);
            send_all(held[i], "GET /sta");
        }
        while (fd_count(pid) < before + 3 && mono_ms() < end) {
            nap(20);
        }
        CHECK(fd_count(pid) == before + 3);
        check_sockets(pid, port, 3);
        for (size_t i = 0; i < 3; i++) {
            CHECK(close(held[i]) == 0);
        }
        end = mono_ms() + 3000;
        while (fd_count(pid) != before && mono_ms() < end) {
            nap(20);
        }
        CHECK(fd_count(pid) == before);
    }
    flood(port);
    {
        int64_t end = mono_ms() + 3000;

        do {   /* mid-flood a 503 is the connection limit working; afterwards it must answer */
            (void)get_status(port);
            if (status_of() == 503) {
                nap(50);
            }
        } while (status_of() == 503 && mono_ms() < end);
        CHECK(status_of() == 200);
    }
    for (int i = 0; i < 150; i++) { /* it may still be clearing the last of them */
        after = fd_count(pid);
        if (after == before) {
            break;
        }
        nap(20);
    }
    CHECK(after == before);
    check_fd_kinds(pid);
    check_sockets(pid, port, 0);

    /* Told to stop: exit 0, said so, and the port is free again. */
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(wait_exit(pid, WAIT_MS) == GT_EXIT_OK);
    CHECK(log_has(log, "stopping"));
    lfd = iface_listen("127.0.0.1", port, &again);
    CHECK(lfd >= 0 && again == port);
    CHECK(close(lfd) == 0);
    CHECK(close(sv[0]) == 0);

    CHECK(kill(feeder, SIGTERM) == 0);
    CHECK(wait_exit(feeder, WAIT_MS) == GT_EXIT_OK);
}

/* The real binary with --dry: the feeder's soil values are 300..999, all valid, so a line of 0 is
 * never reached ("no") and a line of 65535 always is ("yes"). */
static void dry_flag(const char* tmp)
{
    static const struct {
        const char* dry;
        const char* want;
    } CASES[] = {{"0", "\"needs_water\":\"no\",\"dry_raw\":0,"},
                 {"65535", "\"needs_water\":\"yes\",\"dry_raw\":65535,"}};
    char        flog[300];
    char        store[300];
    const char* fargs[8];
    char        rate[8] = "5";
    pid_t       feeder;

    join(flog, sizeof flog, tmp, "dry-feeder.log");
    join(store, sizeof store, tmp, "dry-store");
    fargs[0] = "--dir";
    fargs[1] = store;
    fargs[2] = "--rate";
    fargs[3] = rate;
    fargs[4] = NULL;
    feeder = spawn(g_feeder, fargs, flog, -1, -1);

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char        log[300];
        int         sv[2];
        const char* args[12];
        unsigned    port;
        pid_t       pid;

        CHECK(snprintf(log, sizeof log, "%s/dry-%zu.log", tmp, i) < (int)sizeof log);
        CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
        args[0] = "--dir";
        args[1] = store;
        args[2] = "--bind";
        args[3] = "127.0.0.1";
        args[4] = "--port";
        args[5] = "0";
        args[6] = "--refresh-ms";
        args[7] = "200";
        args[8] = "--dry";
        args[9] = CASES[i].dry;
        args[10] = NULL;
        pid = spawn(g_iface, args, log, sv[1], sv[0]);
        CHECK(close(sv[1]) == 0);
        port = wait_listening(log);
        (void)wait_records(port, 3);
        if (strstr(body(), CASES[i].want) == NULL) {
            (void)fprintf(stderr, "--dry %s: wanted %s in %s\n", CASES[i].dry, CASES[i].want, body());
            CHECK(!"--dry changes the page");
        }
        get(port, "GET / HTTP/1.1\r\n\r\n");
        CHECK(status_of() == 200);
        CHECK(strstr(body(), i == 0 ? "No. Soil reading" : "Yes, water this plant") != NULL);
        CHECK(kill(pid, SIGTERM) == 0);
        CHECK(wait_exit(pid, WAIT_MS) == GT_EXIT_OK);
        CHECK(close(sv[0]) == 0);
    }
    CHECK(kill(feeder, SIGTERM) == 0);
    CHECK(wait_exit(feeder, WAIT_MS) == GT_EXIT_OK);
}

int main(int argc, char** argv)
{
    char tmp[256];

    if (argc != 3) {
        (void)fprintf(stderr, "usage: %s GT_INTERFACE GT_FEEDER\n", argv[0]);
        return 2;
    }
    g_iface = argv[1];
    g_feeder = argv[2];
    if (access(g_iface, X_OK) != 0 || access(g_feeder, X_OK) != 0) {
        (void)fprintf(stderr, "%s: cannot execute %s or %s\n", argv[0], g_iface, g_feeder);
        return 2;
    }
    CHECK(atexit(kill_live) == 0);
    make_tmp(tmp, sizeof tmp);

    bad_arguments(tmp);
    port_in_use(tmp);
    supervisor_lost(tmp);
    full_run(tmp);
    dry_flag(tmp);

    remove_tree(tmp);
    (void)puts("PASS");
    return 0;
}
