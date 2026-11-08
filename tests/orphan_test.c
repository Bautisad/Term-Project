/* orphan_test: proc_start against a fake proc tree (names with spaces and parentheses, a zombie,
 * truncated files) and the real /proc/self, pids_save, and orphans_kill on files that must not
 * kill anything (wrong start time, missing, garbled).  That it kills real orphans is checked in
 * supervisor_test.sh.  Exits 1 at the first failed check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "child.h"
#include "orphan.h"

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            (void)fprintf(stderr, "orphan_test: line %d: %s\n", __LINE__, #c);  \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static char root[256];
static char pf[300]; /* the pids file */

/* Creates <root>/<pid>/stat holding `text`. */
static void put(int pid, const char* text)
{
    char  d[300];
    char  p[320];
    FILE* f;

    CHECK(snprintf(d, sizeof d, "%s/%d", root, pid) < (int)sizeof d);
    CHECK(snprintf(p, sizeof p, "%s/stat", d) < (int)sizeof p);
    CHECK(mkdir(d, 0700) == 0);
    f = fopen(p, "w");
    CHECK(f != NULL && fputs(text, f) >= 0 && fclose(f) == 0);
}

/* A stat line as proc(5) lays it out: pid (comm) state, 18 fields, start time, then more. */
static void put_stat(int pid, const char* comm, char state, unsigned long long start)
{
    char t[256];

    CHECK(snprintf(t, sizeof t, "%d (%s) %c 1 1 1 0 -1 4194560 100 0 0 0 1 1 0 0 20 0 1 0 %llu 1000 1\n",
                   pid, comm, state, start) < (int)sizeof t);
    put(pid, t);
}

/* Makes `text` the pids file and runs the sweep on it. */
static void sweep(const char* text)
{
    FILE* f = fopen(pf, "w");

    CHECK(f != NULL && fputs(text, f) >= 0 && fclose(f) == 0);
    orphans_kill(pf, 1);
}

/* The same, for a line that names this process with start time `start`. */
static void sweep_self(unsigned long long start)
{
    char line[64];

    CHECK(snprintf(line, sizeof line, "%d %llu\n", getpid(), start) < (int)sizeof line);
    sweep(line);
}

int main(void)
{
    const char*        tmp = getenv("TMPDIR");
    unsigned long long self;
    unsigned long long start;
    char               cmd[300];
    child              ch[2];
    FILE*              f;
    int                pid;

    CHECK(snprintf(root, sizeof root, "%s/orphan_test.XXXXXX", tmp != NULL ? tmp : "/tmp") <
          (int)sizeof root);
    CHECK(mkdtemp(root) != NULL);
    CHECK(snprintf(pf, sizeof pf, "%s/pids", root) < (int)sizeof pf);

    put_stat(100, "gt-hub", 'S', 12345);
    put_stat(101, "a b) (c d", 'S', 777); /* the name may hold spaces and parentheses */
    put_stat(102, "gt-hub", 'Z', 555);    /* a zombie is gone */
    put(103, "103 (x) S 1 2\n");          /* cut short */
    put(104, "");
    CHECK(proc_start(root, 100) == 12345);
    CHECK(proc_start(root, 101) == 777);
    CHECK(proc_start(root, 102) == 0);
    CHECK(proc_start(root, 103) == 0);
    CHECK(proc_start(root, 104) == 0);
    CHECK(proc_start(root, 999) == 0);
    self = proc_start("/proc", getpid());
    CHECK(self != 0 && proc_start("/proc", getpid()) == self);

    child_init(&ch[0], "./gt-a");
    child_init(&ch[1], "./gt-b"); /* not running: no line */
    ch[0].pid = getpid();
    pids_save(pf, ch, 2);
    f = fopen(pf, "r");
    CHECK(f != NULL && fscanf(f, "%d %llu", &pid, &start) == 2 && pid == getpid() && start == self);
    CHECK(fscanf(f, "%d %llu", &pid, &start) != 2);
    CHECK(fclose(f) == 0);

    /* Each of these would kill this process if orphans_kill were wrong about it. */
    sweep_self(self + 1); /* the pid was reused: another start time */
    sweep_self(0);
    sweep("not a pid\n");
    CHECK(unlink(pf) == 0);
    orphans_kill(pf, 1); /* no file */

    CHECK(snprintf(cmd, sizeof cmd, "rm -rf '%s'", root) < (int)sizeof cmd);
    CHECK(system(cmd) == 0);
    return 0;
}
