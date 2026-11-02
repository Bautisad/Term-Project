/* daemon.h: the LAN page process loop.  main.c and the tests both call iface_run, so the real
 * binary and the tests run the same code.
 *
 * One poll loop, no threads.  It serves the page the way page.h and http.h define it:
 *   GET / and /index.html   the HTML page
 *   GET /status.json        the same state as JSON
 * The store is read-only here: it is re-read at most once every refresh_ms, and only when a
 * request needs it.  The product makes no outgoing network connection; the only sockets are the
 * listening socket and the connections it accepts (the heartbeat socket to the supervisor is a
 * local AF_UNIX pair). */
#ifndef GT_IFACE_DAEMON_H
#define GT_IFACE_DAEMON_H

#include <stdint.h>

#define IFACE_PORT_DEFAULT 8080u
#define IFACE_DIR_DEFAULT  "/var/lib/greenthumb/store" /* the hub must write here, or be told --dir */
#define IFACE_MAX_CONNS    8u     /* open at once; the next one is answered 503 and closed */
#define IFACE_BODY_CAP     8192u  /* largest HTML or JSON body (page_render_* fail if it is larger) */
#define IFACE_RESPONSE_CAP (IFACE_BODY_CAP + 512u) /* body plus the response headers */

typedef struct iface_opts {
    const char* dir;          /* the store directory (read only) */
    int         ctl_fd;       /* supervisor control socket for heartbeats; -1 for none */
    unsigned    deadline_ms;  /* a connection gets this long, from accept, to finish */
    unsigned    refresh_ms;   /* the store is not read again within this long */
    unsigned    heartbeat_ms; /* time between heartbeats */
} iface_opts;

/* The production settings: GT_CHILD_CTL_FD, 5 s deadline, 5 s refresh, ipc.h's heartbeat
 * interval, and IFACE_DIR_DEFAULT. */
iface_opts iface_opts_default(void);

/* A nonblocking, close-on-exec listening socket on ipv4:port ("0.0.0.0" for every interface,
 * "127.0.0.1" for the tests).  port 0 picks a free one; *bound (may be NULL) gets the port in
 * use.  Returns the descriptor, or -1 with errno set (EINVAL for a bad address or port, EADDRINUSE
 * and the rest from the failing call); the failure is logged. */
int iface_listen(const char* ipv4, unsigned port, unsigned* bound);

/* Heartbeat, accept, read a request, answer, until told to stop.  listen_fd is the socket from
 * iface_listen; the caller keeps it and closes it.  Returns a gt_exit_code:
 *   GT_EXIT_OK      SIGTERM or SIGINT
 *   GT_EXIT_FATAL   poll or signal setup failed, or the listening socket broke
 *   GT_EXIT_IPC     the supervisor's control socket closed
 *   GT_EXIT_CONFIG  opts out of range (restarting would not help)
 * A store that cannot be read is not an exit: the page says so (degraded) and the next
 * request after refresh_ms tries again.  All connections are closed before returning. */
int iface_run(const iface_opts* opts, int listen_fd);

#endif /* GT_IFACE_DAEMON_H */
