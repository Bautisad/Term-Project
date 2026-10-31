#include "light_drv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/gpio.h>

#include "sensor_util.h"

/* The comparator has no hysteresis, so near the threshold it can flicker. Changes shorter
 * than this are ignored (the kernel does it, in software if the hardware cannot). */
#define DEBOUNCE_US 10000u

static int gpio_next(void* ctx, bool* level)
{
    const light_dev*          dev = ctx;
    struct gpio_v2_line_event ev;
    ssize_t                   n = read(dev->fd, &ev, sizeof ev);

    if (n < 0) {
        return errno == EAGAIN ? 0 : -1;
    }
    if (n != (ssize_t)sizeof ev) {
        errno = EIO;
        return -1;
    }
    *level = ev.id == GPIO_V2_LINE_EVENT_RISING_EDGE; /* active-low is applied: rising is brighter */
    return 1;
}

int light_gpio_open(light_dev* dev, const char* chip_path, unsigned line)
{
    struct gpio_v2_line_request req;
    struct gpio_v2_line_values  val = {.mask = 1};
    int                         chip = open(chip_path, O_RDONLY | O_CLOEXEC);
    int                         rc;
    int                         err;

    if (chip < 0) {
        return -1;
    }
    memset(&req, 0, sizeof req);
    req.offsets[0] = line;
    req.num_lines = 1;
    (void)snprintf(req.consumer, sizeof req.consumer, "greenthumb");
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                       GPIO_V2_LINE_FLAG_EDGE_FALLING | GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
    req.config.attrs[0].attr.debounce_period_us = DEBOUNCE_US;
    req.config.attrs[0].mask = 1;
    rc = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req);
    err = errno;
    sensor_close_fd(chip); /* the line request stands on its own without the chip fd */
    if (rc != 0) {
        errno = err;
        return -1;
    }
    if (ioctl(req.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &val) != 0 || !sensor_set_nonblock(req.fd)) {
        err = errno;
        sensor_close_fd(req.fd);
        errno = err;
        return -1;
    }
    dev->fd = req.fd;
    dev->level = (val.bits & 1) != 0;
    dev->next = gpio_next;
    dev->ctx = dev;
    return 0;
}

void light_gpio_close(light_dev* dev)
{
    sensor_close_fd(dev->fd);
    dev->fd = -1;
}
