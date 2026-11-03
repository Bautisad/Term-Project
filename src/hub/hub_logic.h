/* hub_logic.h: the hub's decisions, with no sockets, no store and no clock reads, so tests
 * can drive them with made-up messages and times.  hub_proc.c does the I/O around it.
 *
 * A cycle opens on the first sensor message after the previous record.  It closes when both
 * sensors have reported, or HUB_GRACE_MS after it opened, and then one record is built.  A
 * sensor that did not report is marked INVALID in the record.  A second message from a sensor
 * inside an open cycle belongs to the next cycle (it is queued, never overwritten).  If no
 * cycle opens for HUB_IDLE_MS after the last record, an all-INVALID record is built, so a
 * record still appears about once a minute with both sensors dead.
 */
#ifndef GT_HUB_LOGIC_H
#define GT_HUB_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "ipc.h"
#include "record.h"

#define HUB_GRACE_MS 15000 /* wait this long for the second sensor once a cycle is open */
#define HUB_IDLE_MS  75000 /* no cycle opened this long after a record: write an empty one */
#define HUB_QUEUE    2     /* per sensor: [0] this cycle, [1] the next one */

/* 2026-01-01T00:00:00Z.  A timestamp before this is a clock that was never set (the Pi has
 * no network time), so the record gets GT_REC_TIME_UNSET. */
#define HUB_TIME_FLOOR_MS UINT64_C(1767225600000)

/* What the hub remembers between records. */
typedef struct hub_state {
    uint32_t next_seq;   /* seq of the next record */
    uint64_t last_ts_ms; /* timestamp of the last record written by this run; 0 if none */
    uint64_t floor_ms;   /* timestamps below this are TIME_UNSET */
} hub_state;

/* Messages collected for the current and the next cycle.  Fields are public so it can live
 * on the stack; use the functions below rather than writing them directly. */
typedef struct hub_cycle {
    gt_msg_soil  soil[HUB_QUEUE];
    gt_msg_light light[HUB_QUEUE];
    unsigned     n_soil;        /* queued soil messages, 0..HUB_QUEUE */
    unsigned     n_light;       /* queued light messages, 0..HUB_QUEUE */
    bool         open;          /* a cycle is waiting for its second sensor */
    int64_t      opened_ms;     /* monotonic time the cycle opened */
    int64_t      last_close_ms; /* monotonic time of the last record, or of start-up */
    int64_t      grace_ms;      /* set by hub_cycle_init */
    int64_t      idle_ms;       /* set by hub_cycle_init */
} hub_cycle;

/* has_records and last_seq come from store_recover: the first record gets last_seq + 1, or 0
 * for an empty store.  floor_ms is HUB_TIME_FLOOR_MS unless a test needs another. */
void hub_state_init(hub_state* st, bool has_records, uint32_t last_seq, uint64_t floor_ms);

/* start_ms is the monotonic time the hub started; the idle timer counts from it.  Production
 * passes HUB_GRACE_MS and HUB_IDLE_MS; tests pass short times. */
void hub_cycle_init(hub_cycle* c, int64_t start_ms, unsigned grace_ms, unsigned idle_ms);

/* Queue one message at monotonic time now_ms.  Returns false (the message is not kept; the
 * caller logs it) when that sensor already has HUB_QUEUE messages waiting.  Opens a cycle if
 * none is open. */
bool hub_cycle_add_soil(hub_cycle* c, int64_t now_ms, const gt_msg_soil* m);
bool hub_cycle_add_light(hub_cycle* c, int64_t now_ms, const gt_msg_light* m);

/* The monotonic time a record becomes due: opened_ms (already past) for a cycle where both
 * sensors reported, opened_ms + grace_ms for an open one still waiting, last_close_ms +
 * idle_ms when none is open.  A record is due when this is <= the current time, so the hub uses it both as
 * the poll timeout and as the due check. */
int64_t hub_cycle_deadline(const hub_cycle* c);

/* Builds the record for the current cycle into *out (crc32 left 0; the store fills it in),
 * then moves any queued messages up and reopens the cycle if one was waiting.  wall_ms is
 * the wall clock now, used only when no sensor sent a timestamp.  Does not change st: call
 * hub_state_commit once the store has accepted the record. */
void hub_cycle_take(hub_cycle* c, int64_t now_ms, const hub_state* st, uint64_t wall_ms,
                    gt_record* out);

/* The record a cycle would produce from these messages.  Either pointer may be NULL (that
 * sensor did not report).  Sets seq from st.  Exposed for tests. */
void hub_build_record(const hub_state* st, const gt_msg_soil* soil, const gt_msg_light* light,
                      uint64_t wall_ms, gt_record* out);

/* After a successful store_append of rec: seq advances and the timestamp is remembered. */
void hub_state_commit(hub_state* st, const gt_record* rec);

#endif /* GT_HUB_LOGIC_H */
