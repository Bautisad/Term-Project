/* gt-interface: serves the LAN page (HTML and JSON) from the store.
 *
 * usage: gt-interface [--dir PATH] [--port N] [--bind IPV4] [--dry N] [--heartbeat-ms N]
 *                      [--refresh-ms N]
 * Started by the supervisor with no arguments: it listens on 0.0.0.0:8080 and reads the store
 * in IFACE_DIR_DEFAULT.  The flags are for running it by hand and for the tests.  --dry N is the
 * soil value at or below which the plant needs water (0..65535, default 450, the top of the
 * typical STEMMA dry range).  Exit codes are iface_run's, plus GT_EXIT_CONFIG for bad arguments (or an
 * address that is not IPv4) and GT_EXIT_FATAL when the port cannot be listened on. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "daemon.h"
#include "ipc.h"
#include "sensor_log.h"
#include "sensor_util.h"

#define BIND_DEFAULT        "0.0.0.0"
#define HEARTBEAT_MS_MAX    (GT_HEARTBEAT_SOFT_S * 1000ul) /* longer would make the supervisor warn */
#define REFRESH_MS_MAX      3600000ul

/* Flags come in "--name value" pairs.  Returns 0, or -1 for anything unrecognised. */
static int parse_args(int argc, char** argv, iface_opts* o, const char** addr, unsigned* port)
{
    for (int i = 1; i < argc; i += 2) {
        const char*   flag = argv[i];
        const char*   val = i + 1 < argc ? argv[i + 1] : NULL;
        unsigned long n = 0;

        if (val == NULL) {
            return -1;
        }
        else if (strcmp(flag, "--dir") == 0) {
            o->dir = val;
        }
        else if (strcmp(flag, "--bind") == 0) {
            *addr = val;
        }
        else if (strcmp(flag, "--port") == 0 && sensor_parse_uint(val, 65535, &n)) {
            *port = (unsigned)n;
        }
        else if (strcmp(flag, "--dry") == 0 && sensor_parse_uint(val, 65535, &n)) {
            o->dry_raw = (uint16_t)n;
        }
        else if (strcmp(flag, "--heartbeat-ms") == 0 && sensor_parse_uint(val, HEARTBEAT_MS_MAX, &n) && n >= 1) {
            o->heartbeat_ms = (unsigned)n;
        }
        else if (strcmp(flag, "--refresh-ms") == 0 && sensor_parse_uint(val, REFRESH_MS_MAX, &n) && n >= 1) {
            o->refresh_ms = (unsigned)n;
        }
        else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    iface_opts  o = iface_opts_default();
    const char* addr = BIND_DEFAULT;
    unsigned    port = IFACE_PORT_DEFAULT;
    int         lfd;
    int         rc;

    slog_init("interface");
    if (parse_args(argc, argv, &o, &addr, &port) != 0) {
        (void)fprintf(stderr, "usage: %s [--dir PATH] [--port N] [--bind IPV4] [--dry N] "
            "[--heartbeat-ms N] [--refresh-ms N]\n", argv[0]);
        return GT_EXIT_CONFIG;
    }
    o.ctl_fd = sensor_ctl_fd(o.ctl_fd);
    lfd = iface_listen(addr, port, NULL);
    if (lfd < 0) {
        /* A bad address will not get better by restarting; a port in use might. */
        return errno == EINVAL ? GT_EXIT_CONFIG : GT_EXIT_FATAL;
    }
    rc = iface_run(&o, lfd);
    sensor_close_fd(lfd);
    return rc;
}
