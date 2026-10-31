/* gt-sensor-light: watches the LM393 light module and reports bright/dark windows to the hub.
 *
 * usage: gt-sensor-light [--chip PATH] [--line N] [--hub PATH] [--window MS] [--stuck MS]
 *                        [--chatter N]
 * Started by the supervisor with no arguments.  Exit codes are light_run's, plus
 * GT_EXIT_CONFIG for bad arguments and GT_EXIT_HW_INIT when the GPIO line will not open. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ipc.h"
#include "light_proc.h"
#include "sensor_log.h"
#include "sensor_util.h"

#define DEFAULT_CHIP "/dev/gpiochip4" /* the Pi 5 header */
#define DEFAULT_LINE 17

/* Flags come in "--name value" pairs. Returns 0, or -1 for anything unrecognised. */
static int parse_args(int argc, char** argv, light_opts* o, const char** chip, unsigned* line)
{
    for (int i = 1; i < argc; i += 2) {
        const char*   flag = argv[i];
        const char*   val = i + 1 < argc ? argv[i + 1] : NULL;
        unsigned long n = 0;

        if (val == NULL) {
            return -1;
        }
        else if (strcmp(flag, "--chip") == 0) {
            *chip = val;
        }
        else if (strcmp(flag, "--hub") == 0) {
            o->hub_path = val;
        }
        else if (strcmp(flag, "--line") == 0 && sensor_parse_uint(val, 1023, &n)) {
            *line = (unsigned)n;
        }
        else if (strcmp(flag, "--window") == 0 && sensor_parse_uint(val, UINT16_MAX, &n)) {
            o->window_ms = (unsigned)n;
        }
        else if (strcmp(flag, "--stuck") == 0 && sensor_parse_uint(val, UINT32_MAX, &n)) {
            o->cfg.stuck_ms = (uint32_t)n;
        }
        else if (strcmp(flag, "--chatter") == 0 && sensor_parse_uint(val, UINT16_MAX, &n)) {
            o->cfg.chatter_edges = (uint32_t)n;
        }
        else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    light_opts  o = light_opts_default();
    const char* chip = DEFAULT_CHIP;
    unsigned    line = DEFAULT_LINE;
    light_dev   dev;
    int         rc;

    slog_init("light");
    if (parse_args(argc, argv, &o, &chip, &line) != 0) {
        (void)fprintf(stderr, "usage: %s [--chip PATH] [--line N] [--hub PATH] [--window MS] "
            "[--stuck MS] [--chatter N]\n", argv[0]);
        return GT_EXIT_CONFIG;
    }
    o.ctl_fd = sensor_ctl_fd(o.ctl_fd);
    if (light_gpio_open(&dev, chip, line) != 0) {
        slog("ERROR", "open_failed chip=%s line=%u errno=%d", chip, line, errno);
        return GT_EXIT_HW_INIT;
    }
    rc = light_run(&dev, &o);
    light_gpio_close(&dev);
    return rc;
}
