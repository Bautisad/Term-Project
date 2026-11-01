/* light_logic_test: window totals, stuck and chatter rules, with no hardware.  Exits 1 at the
 * first failed check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "light_logic.h"

#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            (void)fprintf(stderr, "light_logic_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

static const light_cfg CFG = {.stuck_ms = LIGHT_STUCK_MS_DEFAULT, .chatter_edges = LIGHT_CHATTER_EDGES_DEFAULT};
static const light_cfg SHORT_STUCK = {.stuck_ms = 1000, .chatter_edges = LIGHT_CHATTER_EDGES_DEFAULT};

static gt_msg_light close_at(light_state* st, const light_cfg* cfg, int64_t t)
{
    gt_msg_light m;

    memset(&m, 0, sizeof m);
    light_close(st, cfg, t, &m);
    return m;
}

/* n changes, one per millisecond from t0, each the opposite of the last. */
static void toggle(light_state* st, unsigned n, int64_t t0)
{
    for (unsigned i = 0; i < n; i++) {
        light_edge(st, !st->level, t0 + i);
    }
}

/* Dark, bright from 20 s to 50 s, then a quiet window. */
static void window_totals(void)
{
    light_state  st;
    gt_msg_light m;

    light_init(&st, false, 0);
    light_edge(&st, true, 20000);
    light_edge(&st, false, 50000);
    m = close_at(&st, &CFG, 60000);
    CHECK(m.bright_ms == 30000 && m.edges == 2 && m.level == 0 && m.status == GT_ST_OK);

    m = close_at(&st, &CFG, 120000); /* the next window starts empty */
    CHECK(m.bright_ms == 0 && m.edges == 0 && m.level == 0 && m.status == GT_ST_OK);

    /* Still bright when the window closes: the last stretch counts, and the next window
     * counts from where this one ended. */
    light_init(&st, true, 0);
    m = close_at(&st, &CFG, 60000);
    CHECK(m.bright_ms == 60000 && m.edges == 0 && m.level == 1);
    m = close_at(&st, &CFG, 90000);
    CHECK(m.bright_ms == 30000);

    /* A repeated level is not a change. */
    light_init(&st, false, 0);
    light_edge(&st, false, 10);
    m = close_at(&st, &CFG, 1000);
    CHECK(m.edges == 0);
}

/* More than chatter_edges changes in one window is INVALID; the next quiet window is not. */
static void chatter(void)
{
    light_state st;

    light_init(&st, false, 0);
    toggle(&st, LIGHT_CHATTER_EDGES_DEFAULT, 1);
    CHECK(close_at(&st, &CFG, 1000).status == GT_ST_OK);
    toggle(&st, LIGHT_CHATTER_EDGES_DEFAULT + 1, 1001);
    CHECK(close_at(&st, &CFG, 2000).status == GT_ST_INVALID);
    CHECK(close_at(&st, &CFG, 3000).status == GT_ST_OK);
}

/* No change for stuck_ms is INVALID (a window closing does not count as a change); the next
 * change clears it. */
static void stuck(void)
{
    light_state st;

    light_init(&st, false, 0);
    CHECK(close_at(&st, &SHORT_STUCK, 999).status == GT_ST_OK);
    CHECK(close_at(&st, &SHORT_STUCK, 1000).status == GT_ST_INVALID);
    light_edge(&st, true, 1500);
    CHECK(close_at(&st, &SHORT_STUCK, 2000).status == GT_ST_OK);
}

/* The 16-bit message fields saturate instead of wrapping. */
static void saturation(void)
{
    light_state  st;
    gt_msg_light m;

    light_init(&st, true, 0);
    m = close_at(&st, &CFG, 100000);
    CHECK(m.bright_ms == UINT16_MAX && m.edges == 0);
    toggle(&st, 70000, 100000);
    m = close_at(&st, &CFG, 200000);
    CHECK(m.edges == UINT16_MAX && m.status == GT_ST_INVALID);
}

int main(void)
{
    window_totals();
    chatter();
    stuck();
    saturation();
    (void)puts("light_logic_test: ok");
    return 0;
}
