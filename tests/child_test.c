/* child_test: crash-loop detection and recovery in schedule_restart and child_watch, with no
 * processes and no clock.  Exits 1 at the first failed check.  That the loop is logged exactly
 * once is checked in supervisor_test.sh, where the log is a file. */
#include <stdio.h>
#include <stdlib.h>

#include "child.h"

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            (void)fprintf(stderr, "child_test: line %d: %s\n", __LINE__, #c);   \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static const hb_limits LIM = {30, 60, 5, GT_RESTART_MAX, 300, 3600};

/* A fresh child with GT_RESTART_MAX restarts scheduled: the first at t=0, the rest 1 s apart. */
static void worn(child* c)
{
    child_init(c, "/x/gt-hub");
    for (unsigned k = 0; k < GT_RESTART_MAX; k++) {
        schedule_restart(c, (int64_t)k * 1000, &LIM);
        CHECK(!c->degraded);
    }
}

/* The restart after GT_RESTART_MAX of them inside the window is the loop, and it stays one. */
static void trips_after_max(void)
{
    child c;

    worn(&c);
    schedule_restart(&c, 5000, &LIM);
    CHECK(c.degraded && c.restart_at_ms - 5000 == GT_BACKOFF_MAX_S * 1000);
    schedule_restart(&c, 6000, &LIM);
    CHECK(c.degraded && c.restart_at_ms - 6000 == GT_BACKOFF_MAX_S * 1000);
}

/* The window is inclusive: the 6th restart exactly window_s after the 1st is a loop. */
static void window_edge(void)
{
    child a;
    child b;

    worn(&a);
    schedule_restart(&a, 300000, &LIM);
    CHECK(a.degraded);
    worn(&b);
    schedule_restart(&b, 300001, &LIM);
    CHECK(!b.degraded);
}

/* Restarts spread wider than the window never add up to a loop. */
static void slow_trickle(void)
{
    child c;

    child_init(&c, "/x/gt-hub");
    for (unsigned k = 0; k < 20; k++) {
        schedule_restart(&c, (int64_t)k * 61000, &LIM); /* 5 of them span 305 s */
        CHECK(!c.degraded);
    }
}

static void own_max(void)
{
    hb_limits l = LIM;
    child     c;

    l.restart_max = 2;
    child_init(&c, "/x/gt-hub");
    schedule_restart(&c, 0, &l);
    schedule_restart(&c, 1000, &l);
    CHECK(!c.degraded);
    schedule_restart(&c, 2000, &l);
    CHECK(c.degraded);
}

/* A degraded child that stays up a full window is stable, and its next crash is just a crash. */
static void recovers(void)
{
    child c;

    worn(&c);
    schedule_restart(&c, 5000, &LIM);
    CHECK(c.degraded && c.backoff_s == GT_BACKOFF_MAX_S);

    c.pid = 1; /* child_watch ignores a child that is not running; no signal is sent at age 0 */
    c.started_ms = 10000;
    c.last_hb_ms = 309999;
    child_watch(&c, 309999, &LIM);
    CHECK(c.degraded);
    c.last_hb_ms = 310000;
    child_watch(&c, 310000, &LIM);
    CHECK(!c.degraded && c.backoff_s == GT_BACKOFF_MIN_S);

    schedule_restart(&c, 310500, &LIM);
    CHECK(!c.degraded && c.restart_at_ms - 310500 == GT_BACKOFF_MIN_S * 1000);
}

/* A fault is an exit that was not a clean 0: a signal, a non-zero code, or a kill for silence.
 * Nothing counts while the supervisor is stopping.  Wait statuses are built by hand:
 * code << 8 for an exit, the bare signal number for a death by signal. */
static void counts_faults(void)
{
    child c;

    child_init(&c, "/x/gt-hub");
    c.pid = 1;
    child_exited(&c, 0, true, 0, &LIM); /* stopping: exit 0 */
    CHECK(c.faults == 0);

    child_init(&c, "/x/gt-hub");
    c.pid = 1;
    child_exited(&c, 0, false, 0, &LIM); /* clean exit 0 */
    CHECK(c.faults == 0 && c.stopped);

    child_init(&c, "/x/gt-hub");
    c.pid = 1;
    child_exited(&c, 1 << 8, false, 0, &LIM); /* exit 1 */
    CHECK(c.faults == 1);
    c.pid = 1;
    child_exited(&c, 11, false, 1000, &LIM); /* SIGSEGV */
    CHECK(c.faults == 2);

    child_init(&c, "/x/gt-hub");
    c.pid = 1;
    c.hung = true;
    child_exited(&c, 0, false, 0, &LIM); /* killed for silence, then exit 0 */
    CHECK(c.faults == 1);

    child_init(&c, "/x/gt-hub");
    c.pid = 1;
    child_exited(&c, 15, true, 0, &LIM); /* SIGTERM at shutdown */
    CHECK(c.faults == 0);
}

int main(void)
{
    trips_after_max();
    window_edge();
    slow_trickle();
    own_max();
    recovers();
    counts_faults();
    return 0;
}
