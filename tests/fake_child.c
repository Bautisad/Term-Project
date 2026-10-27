/* gt-fake-child: stand-in daemon for the supervisor tests.  Behaviour comes from GT_FAKE:
 *   run       wait for SIGTERM, then exit 0
 *   exit:N    exit with code N immediately
 *   crash     kill itself with SIGKILL
 *   fdcheck   exit 0 if fd 3 is the control socket and no fd above 3 is open,
 *             7 if an extra fd leaked in, 8 if fd 3 is missing */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ipc.h"

static void on_term(int sig)
{
    (void)sig;
    _exit(GT_EXIT_OK);
}

int main(void)
{
    const char* mode = getenv("GT_FAKE");

    if (mode == NULL) {
        (void)fprintf(stderr, "gt-fake-child: GT_FAKE not set\n");
        return GT_EXIT_CONFIG;
    }
    if (strcmp(mode, "run") == 0) {
        struct sigaction sa;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_term;
        if (sigaction(SIGTERM, &sa, NULL) != 0) {
            return GT_EXIT_FATAL;
        }
        for (;;) {
            pause();
        }
    }
    if (strncmp(mode, "exit:", 5) == 0) {
        return atoi(mode + 5);
    }
    if (strcmp(mode, "crash") == 0) {
        (void)kill(getpid(), SIGKILL);
        return GT_EXIT_FATAL;
    }
    if (strcmp(mode, "fdcheck") == 0) {
        struct stat st;

        if (fstat(GT_CHILD_CTL_FD, &st) != 0 || !S_ISSOCK(st.st_mode)) {
            return 8;
        }
        for (int fd = GT_CHILD_CTL_FD + 1; fd < 256; fd++) {
            if (fcntl(fd, F_GETFD) != -1) {
                return 7;
            }
        }
        return GT_EXIT_OK;
    }
    (void)fprintf(stderr, "gt-fake-child: unknown GT_FAKE=%s\n", mode);
    return GT_EXIT_CONFIG;
}
