/* gt-supervisor: placeholder entry point so the build has a target.
 * Real work (start hub and sensors, watch exit codes and heartbeats) comes later. */
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        if (puts("gt-supervisor 0.1.0 (stub)") == EOF) {
            return 1;
        }
        return 0;
    }

    (void)fprintf(stderr, "gt-supervisor: stub, nothing to supervise yet\n");
    return 1;
}