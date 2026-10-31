/* light_drv.h: watching the LM393 module's output.  light_dev is all the sensor process knows
 * about a driver, so tests substitute a fake and the real GPIO driver is one implementation.
 *
 * Unlike the soil sensor there is nothing to read on a timer: the kernel reports each change
 * of the pin as an event, so the process sleeps in poll on dev->fd and wakes only on a change. */
#ifndef GT_LIGHT_DRV_H
#define GT_LIGHT_DRV_H

#include <stdbool.h>

typedef struct light_dev {
    int   fd;    /* poll() says POLLIN while changes are waiting */
    bool  level; /* the flag when the device was opened: true is bright */
    /* The next waiting change.  Returns 1 and sets *level, 0 if none is waiting, or -1 with
     * errno set. */
    int   (*next)(void* ctx, bool* level);
    void* ctx;
} light_dev;

/* The real driver: one line of a GPIO character device (the Pi 5 header is gpiochip4).
 * The module is high when dark; the driver inverts, so level is true when bright.
 * Returns 0, or -1 with errno set and nothing left open. */
int light_gpio_open(light_dev* dev, const char* chip_path, unsigned line);

void light_gpio_close(light_dev* dev);

#endif /* GT_LIGHT_DRV_H */
