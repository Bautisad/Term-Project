/* soil_drv.h: reading the STEMMA soil sensor.  soil_dev is all the sensor process knows
 * about a driver, so tests substitute a fake and the real I2C driver is one implementation. */
#ifndef GT_SOIL_DRV_H
#define GT_SOIL_DRV_H

#include <stdint.h>

typedef struct soil_dev {
    /* One raw 16-bit reading.  Returns 0, or -1 with errno set.  Does not retry. */
    int   (*read)(void* ctx, uint16_t* raw);
    void* ctx;
} soil_dev;

/* The real driver: the sensor at 0x36 on an i2c-dev node. */
typedef struct soil_i2c {
    soil_dev dev; /* pass &i2c.dev to the process loop */
    int      fd;
} soil_i2c;

/* Opens path (for example /dev/i2c-1) and selects the sensor.  Returns 0, or -1 with
 * errno set and nothing left open. */
int soil_i2c_open(soil_i2c* i2c, const char* path);

void soil_i2c_close(soil_i2c* i2c);

#endif /* GT_SOIL_DRV_H */
