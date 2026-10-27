/* ipc.h: messages exchanged between supervisor, hub and sensor processes.
 * DRAFT: shared contract, needs both owners' sign-off.
 *
 * Transport: AF_UNIX SOCK_SEQPACKET, so each send is one whole message and a dead
 * peer shows up as EOF.  Same host, so native byte order; every field is fixed-width
 * and every struct has explicit reserved fields instead of implicit padding.
 *
 *   gt-sensor-soil / gt-sensor-light -> hub   GT_MSG_SOIL / GT_MSG_LIGHT over the hub's
 *                                             listening socket (reconnect after restart)
 *   any child -> supervisor                   GT_MSG_HEARTBEAT over the socketpair
 *                                             inherited as fd GT_CHILD_CTL_FD
 */
#ifndef GT_IPC_H
#define GT_IPC_H

#include <stdint.h>

#define GT_IPC_MAGIC   0x47544D32u /* "GTM2" */
#define GT_IPC_VERSION 1u

#define GT_HUB_SOCK_DEFAULT "/run/greenthumb/hub.sock"
#define GT_CHILD_CTL_FD     3 /* supervisor socketpair end, inherited by every child */

/* Heartbeats.  A child sends its first one right after init, before its first reading. */
#define GT_HEARTBEAT_FIRST_S    60u /* first heartbeat due this long after start (hard limit) */
#define GT_HEARTBEAT_INTERVAL_S 10u /* children send at least this often */
#define GT_HEARTBEAT_SOFT_S     30u /* silent this long: supervisor logs a warning */
#define GT_HEARTBEAT_HARD_S     60u /* silent this long: supervisor kills the child */
#define GT_KILL_GRACE_S         5u  /* SIGTERM, then SIGKILL after this long */

typedef enum gt_msg_type {
    GT_MSG_SOIL      = 1,
    GT_MSG_LIGHT     = 2,
    GT_MSG_HEARTBEAT = 3
} gt_msg_type;

typedef enum gt_sensor_id {
    GT_SENSOR_NONE  = 0, /* hub or supervisor */
    GT_SENSOR_SOIL  = 1,
    GT_SENSOR_LIGHT = 2
} gt_sensor_id;

typedef struct gt_msg_hdr {
    uint32_t magic;     /* GT_IPC_MAGIC */
    uint16_t version;   /* GT_IPC_VERSION */
    uint16_t type;      /* gt_msg_type */
    uint16_t sensor_id; /* gt_sensor_id */
    uint16_t len;       /* payload bytes that follow; must match the type */
    uint32_t seq;       /* per-sender counter */
} gt_msg_hdr;

/* Sensor-reported status bits.  The sensor process decides validity (range, stuck,
 * chatter); the hub only copies the result into the record flags. */
#define GT_ST_OK      0x00u
#define GT_ST_INVALID 0x01u /* no usable value this cycle */
#define GT_ST_STUCK   0x02u /* soil only: value repeated too many samples */

/* Soil: one STEMMA reading per cycle (median of several quick samples). */
typedef struct gt_msg_soil {
    uint64_t ts_unix_ms;
    uint16_t raw;      /* 16-bit capacitive value */
    uint16_t status;   /* GT_ST_* */
    uint32_t reserved; /* zero */
} gt_msg_soil;

/* Light: summary of the GPIO edge events seen since the previous message. */
typedef struct gt_msg_light {
    uint64_t ts_unix_ms;
    uint16_t bright_ms; /* ms spent bright in the window */
    uint16_t edges;     /* transitions in the window (saturates) */
    uint8_t  level;     /* current level: 1 bright, 0 dark */
    uint8_t  status;    /* GT_ST_* */
    uint16_t reserved;  /* zero */
} gt_msg_light;

typedef enum gt_child_state {
    GT_CHILD_STARTING = 0,
    GT_CHILD_RUNNING  = 1,
    GT_CHILD_DEGRADED = 2 /* alive but its hardware is failing */
} gt_child_state;

typedef struct gt_msg_heartbeat {
    int32_t  pid;
    uint32_t state; /* gt_child_state */
} gt_msg_heartbeat;

_Static_assert(sizeof(gt_msg_hdr) == 16, "gt_msg_hdr must be 16 bytes");
_Static_assert(sizeof(gt_msg_soil) == 16, "gt_msg_soil must be 16 bytes");
_Static_assert(sizeof(gt_msg_light) == 16, "gt_msg_light must be 16 bytes");
_Static_assert(sizeof(gt_msg_heartbeat) == 8, "gt_msg_heartbeat must be 8 bytes");

#define GT_MSG_MAX (sizeof(gt_msg_hdr) + sizeof(gt_msg_soil))

/* Child exit codes: the supervisor's restart policy keys off these. */
typedef enum gt_exit_code {
    GT_EXIT_OK      = 0, /* clean shutdown on SIGTERM: do not restart */
    GT_EXIT_FATAL   = 1, /* unexpected failure: restart with backoff */
    GT_EXIT_CONFIG  = 2, /* bad arguments or config: do not restart */
    GT_EXIT_HW_INIT = 3, /* device missing or init failed: restart with backoff */
    GT_EXIT_IPC     = 4  /* lost hub or supervisor channel: restart with backoff */
} gt_exit_code;

/* Restart rules: exponential backoff 1s, 2s, 4s ... capped at 60s.  More than
 * GT_RESTART_MAX restarts inside GT_RESTART_WINDOW_S is a crash loop: the child is
 * marked DEGRADED and held at the backoff cap; the other children keep running. */
#define GT_BACKOFF_MIN_S    1u
#define GT_BACKOFF_MAX_S    60u
#define GT_RESTART_MAX      5u
#define GT_RESTART_WINDOW_S 300u

#endif /* GT_IPC_H */
