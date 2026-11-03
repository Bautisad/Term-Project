#include "hub_logic.h"

#include <string.h>

void hub_state_init(hub_state* st, bool has_records, uint32_t last_seq, uint64_t floor_ms)
{
    st->next_seq = has_records ? last_seq + 1u : 0u;
    st->last_ts_ms = 0;
    st->floor_ms = floor_ms;
}

void hub_cycle_init(hub_cycle* c, int64_t start_ms, unsigned grace_ms, unsigned idle_ms)
{
    memset(c, 0, sizeof *c);
    c->last_close_ms = start_ms;
    c->grace_ms = grace_ms;
    c->idle_ms = idle_ms;
}

/* Room for one more message from a sensor that already has n queued?  Opens the cycle on the
 * first message, so `open` is true exactly when something is queued. */
static bool admit(hub_cycle* c, unsigned n, int64_t now_ms)
{
    if (n >= HUB_QUEUE) {
        return false;
    }
    if (!c->open) {
        c->open = true;
        c->opened_ms = now_ms;
    }
    return true;
}

bool hub_cycle_add_soil(hub_cycle* c, int64_t now_ms, const gt_msg_soil* m)
{
    if (!admit(c, c->n_soil, now_ms)) {
        return false;
    }
    c->soil[c->n_soil++] = *m;
    return true;
}

bool hub_cycle_add_light(hub_cycle* c, int64_t now_ms, const gt_msg_light* m)
{
    if (!admit(c, c->n_light, now_ms)) {
        return false;
    }
    c->light[c->n_light++] = *m;
    return true;
}

int64_t hub_cycle_deadline(const hub_cycle* c)
{
    if (!c->open) {
        return c->last_close_ms + c->idle_ms;
    }
    if (c->n_soil > 0 && c->n_light > 0) {
        return c->opened_ms;
    }
    return c->opened_ms + c->grace_ms;
}

void hub_build_record(const hub_state* st, const gt_msg_soil* soil, const gt_msg_light* light,
                      uint64_t wall_ms, gt_record* out)
{
    memset(out, 0, sizeof *out);
    out->seq = st->next_seq;
    out->ts_unix_ms = soil != NULL ? soil->ts_unix_ms : (light != NULL ? light->ts_unix_ms : wall_ms);

    /* The sensor decides validity; the hub only copies it.  An invalid value is stored as 0. */
    if (soil != NULL && (soil->status & GT_ST_INVALID) == 0) {
        out->soil_raw = soil->raw;
        if ((soil->status & GT_ST_STUCK) != 0) {
            out->flags |= GT_REC_SOIL_STUCK;
        }
    } else {
        out->flags |= GT_REC_SOIL_INVALID;
    }
    if (light != NULL && (light->status & GT_ST_INVALID) == 0) {
        out->light_bright_ms = light->bright_ms;
        out->light_edges = light->edges;
        if (light->level != 0) {
            out->flags |= GT_REC_LIGHT_BRIGHT;
        }
    } else {
        out->flags |= GT_REC_LIGHT_INVALID;
    }

    if (out->ts_unix_ms < st->floor_ms || out->ts_unix_ms < st->last_ts_ms) {
        out->flags |= GT_REC_TIME_UNSET;
    }
}

void hub_cycle_take(hub_cycle* c, int64_t now_ms, const hub_state* st, uint64_t wall_ms,
                    gt_record* out)
{
    hub_build_record(st, c->n_soil > 0 ? &c->soil[0] : NULL, c->n_light > 0 ? &c->light[0] : NULL,
                     wall_ms, out);

    /* Whatever was queued for the next cycle moves up and opens it. */
    if (c->n_soil > 0) {
        memmove(&c->soil[0], &c->soil[1], (HUB_QUEUE - 1) * sizeof c->soil[0]);
        c->n_soil--;
    }
    if (c->n_light > 0) {
        memmove(&c->light[0], &c->light[1], (HUB_QUEUE - 1) * sizeof c->light[0]);
        c->n_light--;
    }
    c->open = c->n_soil > 0 || c->n_light > 0;
    c->opened_ms = now_ms;
    c->last_close_ms = now_ms;
}

void hub_state_commit(hub_state* st, const gt_record* rec)
{
    st->next_seq = rec->seq + 1u;
    /* A record with an untrusted time must not become the reference for the next one. */
    if ((rec->flags & GT_REC_TIME_UNSET) == 0) {
        st->last_ts_ms = rec->ts_unix_ms;
    }
}
