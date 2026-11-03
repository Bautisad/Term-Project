#include "hub_proc.h"

#include <errno.h>
#include <poll.h>

#include "hub_logic.h"
#include "ipc_io.h"
#include "sensor_log.h"
#include "sensor_util.h"
#include "store.h"

#define MAX_CLIENTS 4 /* two sensors, plus room for a reconnect before the old end reads as closed */
#define DRAIN_MAX   8 /* messages per client per wake-up, so a flood cannot starve the rest */

/* Positions in hub.pfd.  sensor_stop_code() needs the signal fd first and the control socket
 * second; client slots follow the listening socket (fd -1 when free). */
#define P_SIG     0
#define P_CTL     1
#define P_LISTEN  2
#define P_CLIENT0 3
#define NPFD      (P_CLIENT0 + MAX_CLIENTS)

typedef struct hub {
    const hub_opts* o;
    struct pollfd   pfd[NPFD];
    gt_store*       store;    /* NULL after an I/O error until it has been reopened */
    hub_state       st;
    hub_cycle       cyc;
    bool            degraded; /* the last store attempt failed */
    bool            recorded; /* at least one record written by this run */
    unsigned        missing;  /* bit 0 soil, bit 1 light: silent in the last record */
    uint32_t        hb_seq;
    bool            hb_down;
} hub;

hub_opts hub_opts_default(void)
{
    hub_opts o = {.sock_path = GT_HUB_SOCK_DEFAULT,
                  .store_dir = HUB_STORE_DEFAULT,
                  .ctl_fd = GT_CHILD_CTL_FD,
                  .grace_ms = HUB_GRACE_MS,
                  .idle_ms = HUB_IDLE_MS,
                  .heartbeat_ms = GT_HEARTBEAT_INTERVAL_S * 1000u};
    return o;
}

static void accept_clients(hub* h)
{
    for (;;) {
        int    fd = ipc_accept(h->pfd[P_LISTEN].fd);
        size_t i = 0;

        if (fd < 0) {
            if (errno != EAGAIN) {
                slog("ERROR", "accept failed errno=%d", errno);
            }
            return;
        }
        while (i < MAX_CLIENTS && h->pfd[P_CLIENT0 + i].fd >= 0) {
            i++;
        }
        if (i == MAX_CLIENTS || !sensor_set_nonblock(fd)) {
            slog("WARN", "client_rejected full=%d errno=%d", i == MAX_CLIENTS, errno);
            sensor_close_fd(fd);
        } else {
            h->pfd[P_CLIENT0 + i].fd = fd;
        }
    }
}

/* Read what one client has sent.  Closes its slot on EOF or a socket error. */
static void drain_client(hub* h, size_t slot, int64_t now)
{
    struct pollfd* p = &h->pfd[P_CLIENT0 + slot];

    for (unsigned k = 0; k < DRAIN_MAX; k++) {
        gt_msg_hdr hdr;
        union {
            gt_msg_soil  soil;
            gt_msg_light light;
        } m;
        int n = ipc_recv(p->fd, &hdr, &m, sizeof m);

        if (n >= 0) { /* ipc_recv has checked the type against the sender and the length */
            bool kept = hdr.type == GT_MSG_SOIL ? hub_cycle_add_soil(&h->cyc, now, &m.soil)
                        : hdr.type == GT_MSG_LIGHT && hub_cycle_add_light(&h->cyc, now, &m.light);

            if (!kept) { /* a full queue, or a heartbeat sent to the wrong socket */
                slog("WARN", "message_dropped type=%u seq=%u slot=%zu", hdr.type, hdr.seq, slot);
            }
        } else if (n == IPC_BAD) {
            slog("WARN", "bad_message slot=%zu", slot);
        } else if (n == -1 && errno == EAGAIN) {
            return;
        } else {
            slog(n == IPC_EOF ? "INFO" : "ERROR", "client_closed slot=%zu errno=%d", slot,
                 n == IPC_EOF ? 0 : errno);
            sensor_close_fd(p->fd);
            p->fd = -1;
            return;
        }
    }
}

/* Build the record for the cycle that is due and append it.  A failure drops the record; the
 * sequence number is not used, so the log has no gap, and the next cycle tries again. */
static void emit_record(hub* h, int64_t now)
{
    gt_record rec;
    unsigned  missing = (h->cyc.n_soil == 0 ? 1u : 0u) | (h->cyc.n_light == 0 ? 2u : 0u);
    bool      ok = false;
    int       err = 0;

    if (missing != h->missing) { /* logged when it changes, not every minute */
        slog(missing != 0 ? "WARN" : "INFO", "sensors_silent mask=0x%x seq=%u", missing,
             h->st.next_seq);
        h->missing = missing;
    }
    hub_cycle_take(&h->cyc, now, &h->st, (uint64_t)sensor_clock_ms(CLOCK_REALTIME), &rec);

    if (h->store == NULL && store_recover(h->o->store_dir, &h->store, NULL) != 0) {
        err = errno;
    }
    if (h->store != NULL) {
        ok = store_append(h->store, &rec) == 0;
        if (!ok) {
            err = errno;
            if (err == EIO) { /* the store gave up on this handle: reopen it next cycle */
                store_close(h->store);
                h->store = NULL;
            }
        }
    }
    sensor_track(&h->degraded, ok, "store_append", err);
    if (ok) {
        hub_state_commit(&h->st, &rec);
        h->recorded = true;
    }
}

int hub_run(const hub_opts* o)
{
    hub               h = {.o = o};
    gt_store_recovery info;
    int64_t           next_hb;
    int               rc = GT_EXIT_FATAL;

    slog_init("hub");
    if (o->sock_path == NULL || o->store_dir == NULL || o->grace_ms < 1 || o->idle_ms < 1 ||
        o->heartbeat_ms < 1) {
        slog("ERROR", "bad_options");
        return GT_EXIT_CONFIG;
    }
    for (size_t i = 0; i < NPFD; i++) {
        h.pfd[i] = (struct pollfd){.fd = -1, .events = POLLIN};
    }
    h.pfd[P_CTL].fd = o->ctl_fd; /* the supervisor's end: used, never closed here */
    h.pfd[P_SIG].fd = sensor_setup(o->ctl_fd);
    if (h.pfd[P_SIG].fd < 0) {
        return rc;
    }

    if (store_recover(o->store_dir, &h.store, &info) != 0) {
        int e = errno;

        slog("ERROR", "store_open_failed dir=%s errno=%d", o->store_dir, e);
        rc = e == EUCLEAN ? GT_EXIT_CONFIG : GT_EXIT_FATAL;
        goto out;
    }
    hub_state_init(&h.st, info.has_records != 0, info.last_seq, HUB_TIME_FLOOR_MS);
    slog("INFO", "start resumed_seq=%u", h.st.next_seq);

    h.pfd[P_LISTEN].fd = ipc_listen(o->sock_path);
    if (h.pfd[P_LISTEN].fd < 0 || !sensor_set_nonblock(h.pfd[P_LISTEN].fd)) {
        slog("ERROR", "listen_failed path=%s errno=%d", o->sock_path, errno);
        goto out;
    }

    next_hb = sensor_clock_ms(CLOCK_MONOTONIC); /* heartbeat first, then the first record */
    hub_cycle_init(&h.cyc, next_hb, o->grace_ms, o->idle_ms);
    for (;;) {
        int64_t now = sensor_clock_ms(CLOCK_MONOTONIC);
        int64_t due = hub_cycle_deadline(&h.cyc);
        int64_t wake = next_hb < due ? next_hb : due;
        int     e;

        if (poll(h.pfd, NPFD, wake > now ? (int)(wake - now) : 0) < 0 && errno != EINTR) {
            slog("ERROR", "poll failed errno=%d", errno);
            goto out;
        }
        e = sensor_stop_code(h.pfd);
        if (e >= 0) {
            rc = e;
            goto out;
        }
        now = sensor_clock_ms(CLOCK_MONOTONIC);
        if (h.pfd[P_LISTEN].revents != 0) {
            accept_clients(&h);
        }
        for (size_t i = 0; i < MAX_CLIENTS; i++) {
            if (h.pfd[P_CLIENT0 + i].revents != 0) {
                drain_client(&h, i, now);
            }
        }
        if (now >= next_hb) {
            gt_child_state state = h.degraded ? GT_CHILD_DEGRADED
                                              : (h.recorded ? GT_CHILD_RUNNING : GT_CHILD_STARTING);

            e = sensor_heartbeat(o->ctl_fd, GT_SENSOR_NONE, &h.hb_seq, state, &h.hb_down);
            if (e >= 0) {
                rc = e;
                goto out;
            }
            next_hb = now + o->heartbeat_ms;
        }
        if (hub_cycle_deadline(&h.cyc) <= now) {
            emit_record(&h, now);
        }
    }

out:
    for (size_t i = P_LISTEN; i < NPFD; i++) {
        sensor_close_fd(h.pfd[i].fd);
    }
    sensor_close_fd(h.pfd[P_SIG].fd);
    store_close(h.store);
    return rc;
}
