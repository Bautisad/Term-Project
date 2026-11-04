#include "soil_drv.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/i2c-dev.h>

#include "sensor_util.h"

#define SOIL_ADDR 0x36
#define SETTLE_NS 3000000L /* the sensor needs a moment between the request and the answer */

static const uint8_t TOUCH_REG[2] = {0x0F, 0x10}; /* same bytes as the README's i2ctransfer */

static int soil_read(void* ctx, uint16_t* raw)
{
    const soil_i2c* i2c = ctx;
    struct timespec gap = {0, SETTLE_NS};
    uint8_t         buf[2];
    ssize_t         n = write(i2c->fd, TOUCH_REG, sizeof TOUCH_REG);

    if (n != (ssize_t)sizeof TOUCH_REG) {
        errno = n < 0 ? errno : EIO; /* a short write is an I/O error */
        return -1;
    }
    while (nanosleep(&gap, &gap) != 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    n = read(i2c->fd, buf, sizeof buf);
    if (n != (ssize_t)sizeof buf) {
        errno = n < 0 ? errno : EIO;
        return -1;
    }
    *raw = (uint16_t)((buf[0] << 8) | buf[1]); /* high byte first */
    return 0;
}

int soil_i2c_open(soil_i2c* i2c, const char* path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, SOIL_ADDR) != 0) {
        int saved = errno;

        sensor_close_fd(fd);
        errno = saved;
        return -1;
    }
    i2c->fd = fd;
    i2c->dev.read = soil_read;
    i2c->dev.ctx = i2c;
    return 0;
}

void soil_i2c_close(soil_i2c* i2c)
{
    sensor_close_fd(i2c->fd);
    i2c->fd = -1;
}
