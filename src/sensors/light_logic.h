/* light_logic.h: what to make of the light module's edge events.  No I/O, so it is tested
 * without hardware.  Times are monotonic milliseconds, supplied by the caller.
 *
 * One window is the time between two messages to the hub.  Over it we total how long the
 * flag read bright and how many times it changed, as gt_msg_light carries them. */
#ifndef GT_LIGHT_LOGIC_H
#define GT_LIGHT_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "ipc.h"

/* Drafts, like the soil bounds: a plant sees day and night, so a flag that never moves for
 * two days is a dead sensor; a real flag changes a few times a day, so many changes inside
 * one window is a floating input.  Overridable on the command line. */
#define LIGHT_STUCK_MS_DEFAULT      (48u * 3600u * 1000u)
#define LIGHT_CHATTER_EDGES_DEFAULT 20u /* changes in one window above this are chatter */

typedef struct light_cfg {
    uint32_t stuck_ms;      /* no change for this long is INVALID */
    uint32_t chatter_edges; /* more changes than this in one window is INVALID */
} light_cfg;

typedef struct light_state {
    bool     level;     /* 1 bright, 0 dark */
    int64_t  mark_ms;   /* start of the span not yet added to bright_ms */
    int64_t  change_ms; /* when the level last changed (or we started watching) */
    uint32_t bright_ms; /* bright time in this window, up to mark_ms */
    uint32_t edges;     /* changes in this window */
} light_state;

/* Start watching at `level`. */
void light_init(light_state* st, bool level, int64_t now_ms);

/* The flag is now `level`.  Counts a change only if it differs from the last level, so a
 * repeated event does no harm. */
void light_edge(light_state* st, bool level, int64_t now_ms);

/* End the window at now_ms: fill out->bright_ms, edges (both saturate at 65535), level and
 * status (GT_ST_INVALID if stuck or chattering, else GT_ST_OK), then start the next window.
 * Other fields of *out are left alone. */
void light_close(light_state* st, const light_cfg* cfg, int64_t now_ms, gt_msg_light* out);

#endif /* GT_LIGHT_LOGIC_H */
