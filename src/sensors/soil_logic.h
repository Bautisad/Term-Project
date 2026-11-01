/* soil_logic.h: what to make of the soil sensor's numbers.  No I/O, so it is tested
 * without hardware.  The sensor process decides validity; the hub only copies it. */
#ifndef GT_SOIL_LOGIC_H
#define GT_SOIL_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc.h"

#define SOIL_MAX_SAMPLES 32u

/* Drafts from the DESIGN.md baseline (air 320 .. wet soil 977).  S1 was descoped, so
 * these are not calibrated; they are overridable on the command line. */
#define SOIL_LO_DEFAULT    250u
#define SOIL_HI_DEFAULT    1100u
#define SOIL_STUCK_DEFAULT 60u /* identical cycles in a row */

typedef struct soil_cfg {
    uint16_t lo;      /* below this is out of range */
    uint16_t hi;      /* above this is out of range (0xffff after i2cdetect lands here) */
    unsigned stuck_n; /* this many identical cycles in a row is STUCK */
} soil_cfg;

typedef struct soil_state {
    bool     have_last;
    uint16_t last;
    unsigned repeat; /* cycles in a row that produced `last` */
} soil_state;

/* Median of n samples, 1 <= n <= SOIL_MAX_SAMPLES (the upper median for even n);
 * 0 for any other n. */
uint16_t soil_median(const uint16_t* v, size_t n);

/* Judge one cycle.  read_ok is false when the sensor could not be read at all; then the
 * cycle is INVALID and the stuck counter is left alone.  Otherwise `value` is the cycle's
 * median: INVALID when outside [lo, hi], and STUCK once it has repeated stuck_n cycles
 * (an out-of-range value that repeats is both).  Returns GT_ST_* bits. */
uint16_t soil_judge(soil_state* st, const soil_cfg* cfg, bool read_ok, uint16_t value);

#endif /* GT_SOIL_LOGIC_H */
