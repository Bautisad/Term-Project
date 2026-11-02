#define _GNU_SOURCE /* accept4 */
#include "daemon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <time.h>

#include "http.h"
#include "ipc.h"
#include "page.h"
#include "sensor_log.h"
#include "sensor_util.h"
#include "store.h"

#define LISTEN_BACKLOG   16
#define ACCEPTS_PER_WAKE 16u    /* so a flood cannot keep the loop from its other work */
#define ACCEPT_PAUSE_MS  1000   /* after running out of descriptors */
#define LOG_EVERY_MS     60000  /* repeated events are logged once per minute, with a count */
#define TIMEOUT_GRACE_MS 1000   /* how long a 408 gets to be delivered */
#define LISTEN_SLOT      2      /* pfd[0] signals, pfd[1] supervisor, pfd[2] listener, then conns */

static const char TEXT[] = "text/plain; charset=utf-8";
static const char HTML[] = "text/html; charset=utf-8";
static const char JSON[] = "application/json";

typedef enum conn_state { C_FREE = 0, C_READING, C_WRITING } conn_state;

typedef struct conn {
    int        fd;
    conn_state st;
    int64_t    deadline; /* CLOCK_MONOTONIC ms */
    size_t     in_len;
    size_t     out_len;
    size_t     out_off;
    char       in[HTTP_MAX_REQUEST_BYTES];
    char       out[IFACE_RESPONSE_CAP];
} conn;

/* Logs an event at most once a minute and counts what it skipped. */
typedef struct limiter {
    int64_t  next_ms;
    unsigned skipped;
} limiter;

typedef struct iface {
    const iface_opts* o;
    conn              c[IFACE_MAX_CONNS];
    char              html[IFACE_BODY_CAP];
    char              json[IFACE_BODY_CAP];
    int               html_len; /* -1: the page did not fit */
    int               json_len;
    bool              have_snap;
    int64_t           snap_due;      /* CLOCK_MONOTONIC ms: when the store may be read again */
    bool              store_down;    /* the last read of the store failed */
    bool              disk_down;     /* the last statvfs failed */
    bool              render_down;
    bool              hb_down;
    uint32_t          hb_seq;
    int64_t           accept_resume; /* CLOCK_MONOTONIC ms: the listener is ignored until then */
    limiter           lim_refused;
    limiter           lim_full;
    limiter           lim_timeout;
    limiter           lim_accept;
} iface;

iface_opts iface_opts_default(void)
{
    iface_opts o = {.dir = IFACE_DIR_DEFAULT,
                    .ctl_fd = GT_CHILD_CTL_FD,
                    .deadline_ms = 5000,
                    .refresh_ms = 5000,
                    .heartbeat_ms = GT_HEARTBEAT_INTERVAL_S * 1000u};
    return o;
}

int iface_listen(const char* ipv4, unsigned port, unsigned* bound)
{
    struct sockaddr_in sin;
    socklen_t          len = sizeof sin;
    int                one = 1;
    int                fd;
    int                err;

    slog_init("interface");
    if (ipv4 == NULL || port > 65535u) {
        errno = EINVAL;
        return -1;
    }
    (void)memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ipv4, &sin.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        err = errno;
        slog("ERROR", "listen failed addr=%s port=%u errno=%d", ipv4, port, err);
        errno = err;
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
        bind(fd, (const struct sockaddr*)&sin, sizeof sin) != 0 || listen(fd, LISTEN_BACKLOG) != 0 ||
        getsockname(fd, (struct sockaddr*)&sin, &len) != 0) {
        goto fail;
    }
    if (bound != NULL) {
        *bound = ntohs(sin.sin_port);
    }
    slog("INFO", "listening addr=%s port=%u", ipv4, (unsigned)ntohs(sin.sin_port));
    return fd;

fail:
    err = errno;
    slog("ERROR", "listen failed addr=%s port=%u errno=%d", ipv4, port, err);
    sensor_close_fd(fd);
    errno = err;
    return -1;
}

/* True if the event should be logged now; *skipped is what was left out since the last time. */
static bool limit_ok(limiter* l, int64_t now, unsigned* skipped)
{
    if (now < l->next_ms) {
        l->skipped++;
        return false;
    }
    *skipped = l->skipped;
    l->skipped = 0;
    l->next_ms = now + LOG_EVERY_MS;
    return true;
}

static void conn_close(conn* c)
{
    sensor_close_fd(c->fd);
    c->fd = -1;
    c->st = C_FREE;
}

/* Send what is queued.  True when the connection is finished (all sent, or it failed); false
 * when the socket is full and we must wait for POLLOUT. */
static bool conn_write(conn* c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return false;
            }
            if (errno != EPIPE && errno != ECONNRESET) { /* the client going away is normal */
                slog("WARN", "send failed errno=%d", errno);
            }
            return true;
        }
        c->out_off += (size_t)n;
    }
    return true;
}

/* Queue a complete response and try to send it now. */
static void respond(conn* c, int status, const char* ctype, const char* body, size_t len)
{
    int n = http_build(c->out, sizeof c->out, status, ctype, body, len);

    if (n < 0) {
        const char* eb = http_error_body(500);

        slog("ERROR", "response_build_failed status=%d", status);
        n = http_build(c->out, sizeof c->out, 500, TEXT, eb, eb != NULL ? strlen(eb) : 0);
    }
    if (n < 0) {
        conn_close(c);
        return;
    }
    c->out_len = (size_t)n;
    c->out_off = 0;
    c->st = C_WRITING;
    if (conn_write(c)) {
        conn_close(c);
    }
}

static void respond_error(conn* c, int status)
{
    const char* body = http_error_body(status);

    respond(c, status, TEXT, body, body != NULL ? strlen(body) : 0);
}

/* Read the store and the disk again if the last look is older than refresh_ms, and render both
 * bodies.  Called when a request needs the page. */
static void refresh(iface* d, int64_t mono)
{
    page_acc      a;
    page_input    in;
    page_view     v;
    struct statvfs sv;
    uint64_t      now_ms;
    int           err = 0;
    int           derr = 0;
    bool          disk_ok;

    if (d->have_snap && mono < d->snap_due) {
        return;
    }
    page_acc_init(&a);
    now_ms = (uint64_t)sensor_clock_ms(CLOCK_REALTIME);
    /* No upper bound, so a record dated ahead of now is seen and reported as a clock fault. */
    if (store_read_range(d->o->dir, now_ms > PAGE_WINDOW_MS ? now_ms - PAGE_WINDOW_MS : 0, UINT64_MAX,
                         page_acc_visit, &a) < 0) {
        err = errno;
    }
    disk_ok = statvfs(d->o->dir, &sv) == 0;
    if (!disk_ok) {
        derr = errno;
    }
    sensor_track(&d->store_down, err == 0, "store_read", err);
    sensor_track(&d->disk_down, disk_ok, "statvfs", derr);

    (void)memset(&in, 0, sizeof in);
    in.acc = &a;
    in.now_ms = now_ms;
    in.store_errno = err;
    in.disk_known = disk_ok;
    in.disk_free_bytes = disk_ok ? (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize : 0;
    page_judge(&in, &v);
    d->html_len = page_render_html(d->html, sizeof d->html, &v);
    d->json_len = page_render_json(d->json, sizeof d->json, &v);
    sensor_track(&d->render_down, d->html_len >= 0 && d->json_len >= 0, "render", ENOSPC);
    d->snap_due = mono + d->o->refresh_ms;
    d->have_snap = true;
}

static void serve(iface* d, conn* c, http_route route, int64_t mono)
{
    if (route == HTTP_ROUTE_NONE) {
        respond_error(c, 404);
        return;
    }
    refresh(d, mono);
    if (route == HTTP_ROUTE_INDEX) {
        if (d->html_len < 0) {
            respond_error(c, 500);
        } else {
            respond(c, 200, HTML, d->html, (size_t)d->html_len);
        }
    } else if (d->json_len < 0) {
        respond_error(c, 500);
    } else {
        respond(c, 200, JSON, d->json, (size_t)d->json_len);
    }
}

static void conn_read(iface* d, conn* c, int64_t mono)
{
    ssize_t  n = recv(c->fd, c->in + c->in_len, sizeof c->in - c->in_len, 0);
    http_req req;
    int      r;

    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return;
        }
        if (errno != ECONNRESET) {
            slog("WARN", "recv failed errno=%d", errno);
        }
        conn_close(c);
        return;
    }
    if (n == 0) { /* the client left before finishing its request */
        conn_close(c);
        return;
    }
    c->in_len += (size_t)n;
    r = http_parse(c->in, c->in_len, &req);
    if (r == HTTP_NEED_MORE && c->in_len == sizeof c->in) {
        r = 431; /* http_parse says this itself; never wait on a full buffer */
    }
    if (r == HTTP_NEED_MORE) {
        return;
    }
    if (r != HTTP_OK) {
        respond_error(c, r);
        return;
    }
    serve(d, c, req.route, mono);
}

/* Accept what is waiting (a bounded number).  Returns false if the listening socket is broken. */
static bool accept_ready(iface* d, int lfd, int64_t mono)
{
    for (unsigned i = 0; i < ACCEPTS_PER_WAKE; i++) {
        struct sockaddr_storage ss;
        socklen_t               sl = sizeof ss;
        unsigned                skipped = 0;
        conn*                   slot = NULL;
        int                     fd = accept4(lfd, (struct sockaddr*)&ss, &sl, SOCK_CLOEXEC | SOCK_NONBLOCK);

        if (fd < 0) {
            int e = errno;

            if (e == EAGAIN) {
                return true;
            }
            if (e == EINTR || e == ECONNABORTED || e == EPROTO) {
                continue; /* that one connection is gone; carry on */
            }
            if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM) {
                if (limit_ok(&d->lim_accept, mono, &skipped)) {
                    slog("ERROR", "accept failed errno=%d skipped=%u", e, skipped);
                }
                d->accept_resume = mono + ACCEPT_PAUSE_MS; /* else poll would spin on it */
                return true;
            }
            slog("ERROR", "accept failed errno=%d", e);
            return false;
        }
        if (!http_peer_allowed((const struct sockaddr*)&ss, sl)) {
            if (limit_ok(&d->lim_refused, mono, &skipped)) {
                slog("WARN", "peer_refused family=%d skipped=%u", (int)ss.ss_family, skipped);
            }
            sensor_close_fd(fd);
            continue;
        }
        for (size_t k = 0; k < IFACE_MAX_CONNS; k++) {
            if (d->c[k].st == C_FREE) {
                slot = &d->c[k];
                break;
            }
        }
        if (slot == NULL) { /* full: say so if the socket will take it right now, and move on */
            char tmp[512];
            int  n = http_build(tmp, sizeof tmp, 503, TEXT, http_error_body(503), strlen(http_error_body(503)));

            if (limit_ok(&d->lim_full, mono, &skipped)) {
                slog("WARN", "connections_full max=%u skipped=%u", IFACE_MAX_CONNS, skipped);
            }
            if (n < 0 || send(fd, tmp, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
                slog("INFO", "reject_send_failed errno=%d", n < 0 ? EMSGSIZE : errno);
            }
            sensor_close_fd(fd);
            continue;
        }
        slot->fd = fd;
        slot->st = C_READING;
        slot->deadline = mono + d->o->deadline_ms;
        slot->in_len = 0;
        slot->out_len = 0;
        slot->out_off = 0;
    }
    return true;
}

/* Close every connection whose time is up.  One still waiting for its request gets a 408 first. */
static void expire(iface* d, int64_t mono)
{
    for (size_t k = 0; k < IFACE_MAX_CONNS; k++) {
        conn*    c = &d->c[k];
        unsigned skipped = 0;

        if (c->st == C_FREE || mono < c->deadline) {
            continue;
        }
        if (limit_ok(&d->lim_timeout, mono, &skipped)) {
            slog("WARN", "conn_timeout state=%d skipped=%u", (int)c->st, skipped);
        }
        if (c->st == C_READING) {
            respond_error(c, 408);
            if (c->st == C_WRITING) {
                c->deadline = mono + TIMEOUT_GRACE_MS;
            }
        } else {
            conn_close(c);
        }
    }
}

static bool opts_ok(const iface_opts* o, int listen_fd)
{
    return o != NULL && o->dir != NULL && o->deadline_ms >= 1 && o->refresh_ms >= 1 &&
           o->heartbeat_ms >= 1 && listen_fd >= 0;
}

int iface_run(const iface_opts* o, int listen_fd)
{
    iface         d;
    struct pollfd pfd[LISTEN_SLOT + 1 + IFACE_MAX_CONNS];
    int64_t       next_hb;
    int           rc = GT_EXIT_FATAL;

    slog_init("interface");
    if (!opts_ok(o, listen_fd)) {
        slog("ERROR", "bad_options");
        return GT_EXIT_CONFIG;
    }
    (void)memset(&d, 0, sizeof d);
    d.o = o;
    for (size_t k = 0; k < IFACE_MAX_CONNS; k++) {
        d.c[k].fd = -1;
    }
    pfd[0].fd = sensor_setup(o->ctl_fd);
    if (pfd[0].fd < 0) {
        return rc;
    }
    next_hb = sensor_clock_ms(CLOCK_MONOTONIC); /* the first heartbeat goes out at once */
    for (;;) {
        int64_t now = sensor_clock_ms(CLOCK_MONOTONIC);
        int64_t wake = next_hb;
        int64_t wait;
        int     e;

        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = o->ctl_fd;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        pfd[LISTEN_SLOT].fd = now < d.accept_resume ? -1 : listen_fd;
        pfd[LISTEN_SLOT].events = POLLIN;
        pfd[LISTEN_SLOT].revents = 0;
        if (now < d.accept_resume && d.accept_resume < wake) {
            wake = d.accept_resume;
        }
        for (size_t k = 0; k < IFACE_MAX_CONNS; k++) {
            struct pollfd* p = &pfd[LISTEN_SLOT + 1 + k];

            p->fd = d.c[k].fd;
            p->events = d.c[k].st == C_WRITING ? POLLOUT : POLLIN;
            p->revents = 0;
            if (d.c[k].st != C_FREE && d.c[k].deadline < wake) {
                wake = d.c[k].deadline;
            }
        }
        wait = wake > now ? wake - now : 0;
        if (poll(pfd, LISTEN_SLOT + 1 + IFACE_MAX_CONNS, wait > INT_MAX ? INT_MAX : (int)wait) < 0 &&
            errno != EINTR) {
            slog("ERROR", "poll failed errno=%d", errno);
            goto out;
        }
        e = sensor_stop_code(pfd);
        if (e >= 0) {
            rc = e;
            goto out;
        }
        if ((pfd[LISTEN_SLOT].revents & (POLLERR | POLLNVAL)) != 0) {
            slog("ERROR", "listener broken revents=0x%x", (unsigned)pfd[LISTEN_SLOT].revents);
            goto out;
        }
        now = sensor_clock_ms(CLOCK_MONOTONIC);
        if (now >= next_hb) {
            gt_child_state st = d.hb_seq == 0 ? GT_CHILD_STARTING
                                              : (d.store_down ? GT_CHILD_DEGRADED : GT_CHILD_RUNNING);

            e = sensor_heartbeat(o->ctl_fd, GT_SENSOR_NONE, &d.hb_seq, st, &d.hb_down);
            if (e >= 0) {
                rc = e;
                goto out;
            }
            next_hb = sensor_next_due(next_hb, o->heartbeat_ms, now);
        }
        for (size_t k = 0; k < IFACE_MAX_CONNS; k++) { /* before accepting, so finished ones free their slots */
            const struct pollfd* p = &pfd[LISTEN_SLOT + 1 + k];
            conn*                c = &d.c[k];

            if (p->revents == 0 || c->st == C_FREE || c->fd != p->fd) { /* new this round: next one */
                continue;
            }
            if (c->st == C_READING) {
                conn_read(&d, c, now);
            } else if (conn_write(c)) {
                conn_close(c);
            }
        }
        if (pfd[LISTEN_SLOT].revents != 0 && !accept_ready(&d, listen_fd, now)) {
            goto out;
        }
        expire(&d, now);
    }

out:
    for (size_t k = 0; k < IFACE_MAX_CONNS; k++) {
        if (d.c[k].st != C_FREE) {
            conn_close(&d.c[k]);
        }
    }
    sensor_close_fd(pfd[0].fd);
    return rc;
}
