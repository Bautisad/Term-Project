/*gt-hub: collects the sensor messages and appends one record per cycle to the store
*
* usage: gt-hub [--sock PATH] [--store DIR]
* Started by the supervisor with no arguments. Exit codes are hub_run's, plus GT_EXIT_CONFIG
*for bad arguments. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include"hub_proc.h"
#include "ipc.h"
#include"sensor_log.h"
#include "sensor_util.h"

/*flags come in "--name value" pairs.*/
static bool parse_args(int argc, char** argv, hub_opts* o)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--sock") == 0) {
            o->sock_path = argv[i + 1];
        }
        else if (strcmp(argv[i], "--store") == 0) {
            o->store_dir = argv[i + 1];
        }
        else {
            return false;
        }
    }
    return argc % 2 == 1; /* an odd argc means no flag is missing its value */
}

int main(int argc, char** argv)
{
    hub_opts o = hub_opts_default();

    slog_init("hub");
    if (!parse_args(argc, argv, &o)) {
        (void)fprintf(stderr, "usage: %s [--sock PATH] [--store DIR]\n", argv[0]);
        return GT_EXIT_CONFIG;
    }
    o.ctl_fd = sensor_ctl_fd(o.ctl_fd);
    return hub_run(&o);
}