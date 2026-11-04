#define _GNU_SOURCE /* accept4 */
#include "ipc_io.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Payload size a message of this type and sender must have; 0 if the combination is invalid. */
static size_t payload_size(unsigned type, unsigned sensor)
{
    switch (type) {
    case GT_MSG_SOIL:
        return sensor == GT_SENSOR_SOIL ? sizeof(gt_msg_soil) : 0;
    case GT_MSG_LIGHT:
        return sensor == GT_SENSOR_LIGHT ? sizeof(gt_msg_light) : 0;
    case GT_MSG_HEARTBEAT:
        return sensor <= GT_SENSOR_LIGHT ? sizeof(gt_msg_heartbeat) : 0;
    default:
        return 0;
    }
}

static int make_addr(const char* path, struct sockaddr_un* sa)
{
    size_t n = strlen(path);

    if (n == 0 || n >= sizeof sa->sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path, path, n);
    return 0;
}

int ipc_listen(const char* path)
{
    struct sockaddr_un sa;
    int fd;

    if (make_addr(path, &sa) != 0) {
        return -1;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        int saved = errno;

        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int ipc_accept(int listen_fd)
{
    int fd;

    do {
        fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int ipc_connect(const char* path)
{
    struct sockaddr_un sa;
    int fd;

    if (make_addr(path, &sa) != 0) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
        int saved = errno;

        (void)close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int ipc_send(int fd, gt_msg_type type, gt_sensor_id sensor, uint32_t seq, const void* payload,
             size_t len)
{
    uint8_t buf[GT_MSG_MAX];
    gt_msg_hdr hdr;
    ssize_t n;

    if (payload == NULL || len == 0 || len != payload_size((unsigned)type, (unsigned)sensor)) {
        errno = EINVAL;
        return -1;
    }
    hdr.magic = GT_IPC_MAGIC;
    hdr.version = (uint16_t)GT_IPC_VERSION;
    hdr.type = (uint16_t)type;
    hdr.sensor_id = (uint16_t)sensor;
    hdr.len = (uint16_t)len;
    hdr.seq = seq;
    memcpy(buf, &hdr, sizeof hdr);
    memcpy(buf + sizeof hdr, payload, len);
    do {
        n = send(fd, buf, sizeof hdr + len, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : 0;
}

int ipc_recv(int fd, gt_msg_hdr* hdr, void* payload, size_t cap)
{
    uint8_t buf[GT_MSG_MAX + 1]; /* one spare byte: an oversize message is seen, not truncated */
    ssize_t n;
    size_t want;

    do {
        n = recv(fd, buf, sizeof buf, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return -1;
    }
    if (n == 0) {
        return IPC_EOF;
    }
    if ((size_t)n < sizeof *hdr) {
        return IPC_BAD;
    }
    memcpy(hdr, buf, sizeof *hdr);
    want = payload_size(hdr->type, hdr->sensor_id);
    if (hdr->magic != GT_IPC_MAGIC || hdr->version != GT_IPC_VERSION || want == 0 ||
        hdr->len != want || (size_t)n != sizeof *hdr + want) {
        return IPC_BAD;
    }
    if (cap < want) {
        errno = EMSGSIZE;
        return -1;
    }
    memcpy(payload, buf + sizeof *hdr, want);
    return (int)want;
}
