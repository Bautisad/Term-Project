/* gt-sensor-soil: reads the STEMMA soil sensor once a minute and reports to the hub.
*
* usage: gt-sensor-soil [--dev PATH] [--hub PATH] [--samples N] [--lo N] [--hi N] [--stuck N]
* Started by the supervisor with no arguments.  Exit codes are soil_run's, plus
* GT_EXIT_CONFIG for bad arguments and GT_EXIT_HW_INIT when the sensor device will not open. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ipc.h"
#include "sensor_log.h"
#include "sensor_util.h"
#include "soil_proc.h"

#define DEFAULT_DEV "/dev/i2c-1"

/* Flags come in "--name value" pairs. Returns 0, or -1 for anything unrecognised. */
static int parse_args(int argc, char** argv, soil_opts* o, const char** dev)
{
    for (int i = 1; i < argc; i += 2) {
        const char*   flag = argv[i];
        const char*   val = i + 1 < argc ? argv[i + 1] : NULL;
        unsigned long n = 0;

        if (val == NULL) {
            return -1;
        }
        else if (strcmp(flag, "--dev") == 0) {
            *dev = val;
        }
        else if (strcmp(flag, "--hub") == 0) {
            o->hub_path = val;
        }
        else if (strcmp(flag, "--samples") == 0 && sensor_parse_uint(val, SOIL_MAX_SAMPLES, &n)) {
            o->samples = (unsigned)n;
        }
        else if (strcmp(flag, "--lo") == 0 && sensor_parse_uint(val, UINT16_MAX, &n)) {
            o->cfg.lo = (uint16_t)n;
        }
        else if (strcmp(flag, "--hi") == 0 && sensor_parse_uint(val, UINT16_MAX, &n)) {
            o->cfg.hi = (uint16_t)n;
        }
        else if (strcmp(flag, "--stuck") == 0 && sensor_parse_uint(val, 1000000, &n)) {
            o->cfg.stuck_n = (unsigned)n;
        }
        else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    soil_opts   o = soil_opts_default();
    const char* dev = DEFAULT_DEV;
    soil_i2c    i2c;
    int         rc;

    slog_init("soil");
    if (parse_args(argc, argv, &o, &dev) != 0 || o.cfg.lo > o.cfg.hi || o.cfg.stuck_n < 1) {
        (void)fprintf(stderr, "usage: %s [--dev PATH] [--hub PATH] [--samples N] [--lo N] [--hi N] "
            "[--stuck N]\n", argv[0]);
        return GT_EXIT_CONFIG;
    }
    o.ctl_fd = sensor_ctl_fd(o.ctl_fd);
    if (soil_i2c_open(&i2c, dev) != 0) {
        slog("ERROR", "open_failed dev=%s errno=%d", dev, errno);
        return GT_EXIT_HW_INIT;
    }
    rc = soil_run(&i2c.dev, &o);
    soil_i2c_close(&i2c);
    return rc;
}
