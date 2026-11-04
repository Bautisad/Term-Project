#include "light_logic.h"

static uint16_t sat16(uint32_t v)
{
    return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

/* Add the bright time since mark_ms, and start the next span at now_ms. */
static void settle(light_state* st, int64_t now_ms)
{
    if (st->level && now_ms > st->mark_ms) {
        st->bright_ms += (uint32_t)(now_ms - st->mark_ms);
    }
    st->mark_ms = now_ms;
}

void light_init(light_state* st, bool level, int64_t now_ms)
{
    st->level = level;
    st->mark_ms = now_ms;
    st->change_ms = now_ms;
    st->bright_ms = 0;
    st->edges = 0;
}

void light_edge(light_state* st, bool level, int64_t now_ms)
{
    if (level == st->level) {
        return;
    }
    settle(st, now_ms);
    st->level = level;
    st->edges++;
    st->change_ms = now_ms;
}

void light_close(light_state* st, const light_cfg* cfg, int64_t now_ms, gt_msg_light* out)
{
    bool stuck = now_ms - st->change_ms >= (int64_t)cfg->stuck_ms;
    bool chatter = st->edges > cfg->chatter_edges;

    settle(st, now_ms);
    out->bright_ms = sat16(st->bright_ms);
    out->edges = sat16(st->edges);
    out->level = st->level ? 1 : 0;
    out->status = stuck || chatter ? GT_ST_INVALID : GT_ST_OK;
    st->bright_ms = 0;
    st->edges = 0;
}
