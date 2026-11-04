/* ipc_test: ipc_io over a socketpair (round trips, malformed messages, EOF) and over a real
 * listening socket.  Exits 1 at the first failed check. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ipc_io.h"

#define CHECK(c)                                                           \
    do {                                                                   \
        if (!(c)) {                                                        \
            (void)fprintf(stderr, "ipc_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static const gt_msg_soil SOIL = {.ts_unix_ms = 1700000000123u, .raw = 0x1234, .status = GT_ST_STUCK};
static const gt_msg_light LIGHT = {.ts_unix_ms = 99, .bright_ms = 40000, .edges = 6, .level = 1};
static const gt_msg_heartbeat BEAT = {.pid = 4242, .state = GT_CHILD_RUNNING};

/* Send, receive and compare one message; the payload must come back identical. */
static void roundtrip(int tx, int rx, gt_msg_type type, gt_sensor_id sensor, const void* p, size_t n)
{
    gt_msg_hdr h;
    uint8_t out[GT_MSG_MAX];

    CHECK(ipc_send(tx, type, sensor, 7, p, n) == 0);
    CHECK(ipc_recv(rx, &h, out, sizeof out) == (int)n);
    CHECK(h.type == type && h.sensor_id == sensor && h.seq == 7 && h.len == n);
    CHECK(memcmp(out, p, n) == 0);
}

/* A valid soil header; each bad case below breaks one thing. */
static gt_msg_hdr good_hdr(void)
{
    gt_msg_hdr h = {GT_IPC_MAGIC, GT_IPC_VERSION, GT_MSG_SOIL, GT_SENSOR_SOIL, sizeof(gt_msg_soil), 1};
    return h;
}

static void bad_messages(int tx, int rx)
{
    gt_msg_hdr h[7];
    size_t total[7];
    uint8_t raw[GT_MSG_MAX + 8];
    gt_msg_hdr in;
    uint8_t out[GT_MSG_MAX];

    for (int i = 0; i < 7; i++) {
        h[i] = good_hdr();
        total[i] = sizeof h[i] + sizeof(gt_msg_soil);
    }
    h[0].magic ^= 1u;               /* wrong magic */
    h[1].version = 2;               /* wrong version */
    h[2].type = 9;                  /* unknown type */
    h[3].len = 8;                   /* len disagrees with the type */
    total[4] = 8;                   /* shorter than a header */
    total[5] = total[5] + 1;        /* one byte too long */
    h[6].sensor_id = GT_SENSOR_LIGHT; /* soil message from the light sensor */

    for (int i = 0; i < 7; i++) {
        memset(raw, 0, sizeof raw);
        memcpy(raw, &h[i], sizeof h[i]);
        CHECK(send(tx, raw, total[i], 0) == (ssize_t)total[i]);
        CHECK(ipc_recv(rx, &in, out, sizeof out) == IPC_BAD);
        roundtrip(tx, rx, GT_MSG_SOIL, GT_SENSOR_SOIL, &SOIL, sizeof SOIL); /* stream still usable */
    }
}

static void socketpair_cases(void)
{
    int sv[2];
    gt_msg_hdr h;
    uint8_t out[GT_MSG_MAX];

    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    roundtrip(sv[0], sv[1], GT_MSG_SOIL, GT_SENSOR_SOIL, &SOIL, sizeof SOIL);
    roundtrip(sv[0], sv[1], GT_MSG_LIGHT, GT_SENSOR_LIGHT, &LIGHT, sizeof LIGHT);
    roundtrip(sv[0], sv[1], GT_MSG_HEARTBEAT, GT_SENSOR_NONE, &BEAT, sizeof BEAT);

    /* send refuses a wrong length, and a sensor id that does not suit the type */
    CHECK(ipc_send(sv[0], GT_MSG_SOIL, GT_SENSOR_SOIL, 1, &SOIL, 8) == -1 && errno == EINVAL);
    CHECK(ipc_send(sv[0], GT_MSG_LIGHT, GT_SENSOR_SOIL, 1, &LIGHT, sizeof LIGHT) == -1 &&
          errno == EINVAL);

    bad_messages(sv[0], sv[1]);

    CHECK(ipc_send(sv[0], GT_MSG_SOIL, GT_SENSOR_SOIL, 1, &SOIL, sizeof SOIL) == 0);
    CHECK(ipc_recv(sv[1], &h, out, 4) == -1 && errno == EMSGSIZE); /* consumed anyway */

    CHECK(fcntl(sv[1], F_SETFL, O_NONBLOCK) == 0);
    CHECK(ipc_recv(sv[1], &h, out, sizeof out) == -1 && errno == EAGAIN);

    CHECK(close(sv[0]) == 0);
    CHECK(ipc_recv(sv[1], &h, out, sizeof out) == IPC_EOF);
    CHECK(close(sv[1]) == 0);
}

static void socket_file_cases(void)
{
    char dir[] = "/tmp/ipc_testXXXXXX";
    char path[64];
    char longpath[200];
    int ls, c, a;
    gt_msg_hdr h;
    uint8_t out[GT_MSG_MAX];

    CHECK(mkdtemp(dir) != NULL);
    CHECK(snprintf(path, sizeof path, "%s/hub.sock", dir) < (int)sizeof path);

    ls = ipc_listen(path);
    CHECK(ls >= 0);
    CHECK(close(ls) == 0); /* the socket file stays behind, like after a crash */
    ls = ipc_listen(path);
    CHECK(ls >= 0);

    c = ipc_connect(path);
    CHECK(c >= 0);
    a = ipc_accept(ls);
    CHECK(a >= 0 && (fcntl(a, F_GETFD) & FD_CLOEXEC) != 0);
    roundtrip(c, a, GT_MSG_LIGHT, GT_SENSOR_LIGHT, &LIGHT, sizeof LIGHT);
    CHECK(close(c) == 0);
    CHECK(ipc_recv(a, &h, out, sizeof out) == IPC_EOF);

    memset(longpath, 'a', sizeof longpath - 1);
    longpath[0] = '/';
    longpath[sizeof longpath - 1] = '\0';
    CHECK(ipc_listen(longpath) == -1 && errno == ENAMETOOLONG);
    CHECK(ipc_connect(longpath) == -1 && errno == ENAMETOOLONG);

    CHECK(close(a) == 0 && close(ls) == 0);
    CHECK(unlink(path) == 0 && rmdir(dir) == 0);
}

int main(void)
{
    socketpair_cases();
    socket_file_cases();
    (void)puts("PASS");
    return 0;
}
