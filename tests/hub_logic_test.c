/* hub_logic_test: record building, cycle timing and queueing, with no sockets, store or clock.
 * Exits 1 at the first failed check. */
#include <stdio.h>
#include <stdlib.h>

#include "hub_logic.h"

#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            (void)fprintf(stderr, "hub_logic_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

#define FLOOR 1000 /* a small floor, so the test timestamps stay short */
#define GRACE 15000
#define IDLE  75000

static gt_msg_soil soil(uint64_t ts, uint16_t raw, uint16_t status)
{
    gt_msg_soil m = {.ts_unix_ms = ts, .raw = raw, .status = status};

    return m;
}

static gt_msg_light light(uint64_t ts, uint16_t bright_ms, uint16_t edges, uint8_t level, uint8_t status)
{
    gt_msg_light m = {.ts_unix_ms = ts, .bright_ms = bright_ms, .edges = edges, .level = level,
                      .status = status};

    return m;
}

/* The sensor decides validity; the hub copies it into the flags and zeroes invalid values. */
static void build_flags(void)
{
    hub_state    st;
    gt_record    r;
    gt_msg_soil  s;
    gt_msg_light l;

    hub_state_init(&st, true, 41, FLOOR); /* a store with records resumes after the last one */

    s = soil(5000, 800, GT_ST_OK);
    l = light(5100, 30000, 3, 1, GT_ST_OK);
    hub_build_record(&st, &s, &l, 9999, &r);
    CHECK(r.seq == 42 && r.ts_unix_ms == 5000 && r.soil_raw == 800);
    CHECK(r.light_bright_ms == 30000 && r.light_edges == 3 && r.flags == GT_REC_LIGHT_BRIGHT);
    CHECK(r.crc32 == 0); /* the store computes it */

    l = light(5100, 30000, 3, 0, GT_ST_OK); /* dark at the end of the cycle */
    hub_build_record(&st, &s, &l, 9999, &r);
    CHECK(r.flags == 0 && r.light_bright_ms == 30000);

    s = soil(5000, 999, GT_ST_INVALID);
    hub_build_record(&st, &s, NULL, 9999, &r);
    CHECK(r.soil_raw == 0 && r.flags == (GT_REC_SOIL_INVALID | GT_REC_LIGHT_INVALID));

    s = soil(5000, 700, GT_ST_STUCK); /* stuck keeps its value */
    hub_build_record(&st, &s, NULL, 9999, &r);
    CHECK(r.soil_raw == 700 && (r.flags & GT_REC_SOIL_STUCK) != 0 && (r.flags & GT_REC_SOIL_INVALID) == 0);

    s = soil(5000, 700, GT_ST_INVALID | GT_ST_STUCK); /* invalid wins */
    hub_build_record(&st, &s, NULL, 9999, &r);
    CHECK(r.soil_raw == 0 && (r.flags & GT_REC_SOIL_INVALID) != 0 && (r.flags & GT_REC_SOIL_STUCK) == 0);

    l = light(5100, 100, 9, 1, GT_ST_INVALID); /* invalid light: no values, never bright */
    hub_build_record(&st, NULL, &l, 9999, &r);
    CHECK(r.light_bright_ms == 0 && r.light_edges == 0);
    CHECK((r.flags & GT_REC_LIGHT_INVALID) != 0 && (r.flags & GT_REC_LIGHT_BRIGHT) == 0);

    l = light(5100, 100, 1, 1, GT_ST_OK); /* soil silent: the light message supplies the time */
    hub_build_record(&st, NULL, &l, 9999, &r);
    CHECK(r.ts_unix_ms == 5100 && (r.flags & GT_REC_SOIL_INVALID) != 0 && r.soil_raw == 0);

    hub_build_record(&st, NULL, NULL, 9999, &r); /* both silent: the hub's own clock */
    CHECK(r.ts_unix_ms == 9999 && r.flags == (GT_REC_SOIL_INVALID | GT_REC_LIGHT_INVALID));
}

static void time_unset(void)
{
    hub_state   st;
    gt_record   r;
    gt_msg_soil s;

    hub_state_init(&st, false, 0, FLOOR);

    s = soil(FLOOR - 1, 800, GT_ST_OK); /* a clock that was never set */
    hub_build_record(&st, &s, NULL, 0, &r);
    CHECK((r.flags & GT_REC_TIME_UNSET) != 0);
    s = soil(FLOOR, 800, GT_ST_OK);
    hub_build_record(&st, &s, NULL, 0, &r);
    CHECK((r.flags & GT_REC_TIME_UNSET) == 0);

    s = soil(2000, 800, GT_ST_OK);
    hub_build_record(&st, &s, NULL, 0, &r);
    hub_state_commit(&st, &r);
    CHECK(st.next_seq == 1 && st.last_ts_ms == 2000);

    s = soil(1500, 800, GT_ST_OK); /* earlier than the last record */
    hub_build_record(&st, &s, NULL, 0, &r);
    CHECK(r.seq == 1 && (r.flags & GT_REC_TIME_UNSET) != 0);
    hub_state_commit(&st, &r); /* the number is used, but an untrusted time is not remembered */
    CHECK(st.next_seq == 2 && st.last_ts_ms == 2000);

    s = soil(2000, 800, GT_ST_OK); /* equal is not earlier */
    hub_build_record(&st, &s, NULL, 0, &r);
    CHECK((r.flags & GT_REC_TIME_UNSET) == 0);
}

/* Both sensors, then one late (grace), then none (idle). */
static void cycle_timing(void)
{
    hub_cycle    c;
    hub_state    st;
    gt_record    r;
    gt_msg_soil  s;
    gt_msg_light l;

    hub_state_init(&st, false, 0, FLOOR);
    hub_cycle_init(&c, 0, GRACE, IDLE);
    CHECK(!c.open && hub_cycle_deadline(&c) == IDLE); /* idle time counts from start-up */

    s = soil(5000, 800, GT_ST_OK);
    l = light(5100, 30000, 3, 1, GT_ST_OK);
    CHECK(hub_cycle_add_soil(&c, 1000, &s));
    CHECK(c.open && hub_cycle_deadline(&c) == 1000 + GRACE);
    CHECK(hub_cycle_add_light(&c, 3000, &l));
    CHECK(hub_cycle_deadline(&c) == 1000); /* complete: due at once */

    hub_cycle_take(&c, 3000, &st, 9999, &r);
    CHECK(r.seq == 0 && r.soil_raw == 800 && r.flags == GT_REC_LIGHT_BRIGHT);
    CHECK(st.next_seq == 0); /* take does not touch the state: the store may refuse the record */
    CHECK(!c.open && hub_cycle_deadline(&c) == 3000 + IDLE);
    hub_state_commit(&st, &r);

    s = soil(100000, 801, GT_ST_OK); /* light never comes */
    CHECK(hub_cycle_add_soil(&c, 100000, &s));
    CHECK(hub_cycle_deadline(&c) == 100000 + GRACE);
    hub_cycle_take(&c, 100000 + GRACE, &st, 9999, &r);
    CHECK(r.seq == 1 && r.soil_raw == 801 && r.flags == GT_REC_LIGHT_INVALID);
    hub_state_commit(&st, &r);

    hub_cycle_take(&c, 100000 + GRACE + IDLE, &st, 777777, &r); /* nothing at all */
    CHECK(r.ts_unix_ms == 777777 && r.flags == (GT_REC_SOIL_INVALID | GT_REC_LIGHT_INVALID));
}

/* A second message from a sensor waits for the next cycle; a third is refused. */
static void cycle_queue(void)
{
    hub_cycle    c;
    hub_state    st;
    gt_record    r;
    gt_msg_soil  s1 = soil(5000, 1, GT_ST_OK);
    gt_msg_soil  s2 = soil(5001, 2, GT_ST_OK);
    gt_msg_soil  s3 = soil(5002, 3, GT_ST_OK);
    gt_msg_light l = light(5000, 0, 0, 0, GT_ST_OK);

    hub_state_init(&st, false, 0, FLOOR);
    hub_cycle_init(&c, 0, GRACE, IDLE);

    CHECK(hub_cycle_add_soil(&c, 1000, &s1));
    CHECK(hub_cycle_add_soil(&c, 2000, &s2));
    CHECK(!hub_cycle_add_soil(&c, 3000, &s3));
    CHECK(hub_cycle_deadline(&c) == 1000 + GRACE); /* the cycle opened with the first message */

    hub_cycle_take(&c, 1000 + GRACE, &st, 9999, &r);
    CHECK(r.soil_raw == 1);
    CHECK(c.open && hub_cycle_deadline(&c) == 1000 + 2 * GRACE); /* s2 opened the next cycle */

    CHECK(hub_cycle_add_light(&c, 20000, &l)); /* joins s2's cycle */
    CHECK(hub_cycle_deadline(&c) == 1000 + GRACE);
    hub_cycle_take(&c, 20000, &st, 9999, &r);
    CHECK(r.soil_raw == 2 && (r.flags & GT_REC_LIGHT_INVALID) == 0);
    CHECK(!c.open);

    CHECK(hub_cycle_add_light(&c, 90000, &l) && hub_cycle_add_light(&c, 90001, &l));
    CHECK(!hub_cycle_add_light(&c, 90002, &l));
}

int main(void)
{
    build_flags();
    time_unset();
    cycle_timing();
    cycle_queue();
    return 0;
}
