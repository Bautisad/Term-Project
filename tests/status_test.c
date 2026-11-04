/* status_test: the supervisor's /proc reader and status line, with no processes.  proc_stats runs
 * against a fake proc tree in a temp directory, and once against the real /proc/self.  Exits 1 at
 * the first failed check.  That the line appears on a timer is checked in supervisor_test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "child.h"
#include "status.h"

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            (void)fprintf(stderr, "status_test: line %d: %s\n", __LINE__, #c);  \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static char root[256];

static void path_of(char* out, size_t n, const char* rel)
{
    CHECK(snprintf(out, n, "%s/%s", root, rel) < (int)n);
}

static void make_dir(const char* rel)
{
    char p[512];

    path_of(p, sizeof p, rel);
    CHECK(mkdir(p, 0700) == 0);
}

static void make_file(const char* rel, const char* text)
{
    char  p[512];
    FILE* f;

    path_of(p, sizeof p, rel);
    f = fopen(p, "w");
    CHECK(f != NULL);
    CHECK(fputs(text, f) >= 0);
    CHECK(fclose(f) == 0);
}

static void remove_path(const char* rel, int is_dir)
{
    char p[512];

    path_of(p, sizeof p, rel);
    CHECK((is_dir ? rmdir(p) : unlink(p)) == 0);
}

/* pid 100: normal.  101: no VmRSS line.  102: empty fd dir.  103: status but no fd dir.
 * 104: a VmRSS line with no number.  No pid 999. */
static void build_tree(void)
{
    const char* tmp = getenv("TMPDIR");

    CHECK(snprintf(root, sizeof root, "%s/status_test.XXXXXX", tmp != NULL ? tmp : "/tmp") <
          (int)sizeof root);
    CHECK(mkdtemp(root) != NULL);
    make_dir("100");
    make_dir("100/fd");
    make_file("100/status", "Name:\tgt-hub\nVmPeak:\t 9000 kB\nVmRSS:\t    4096 kB\nThreads:\t1\n");
    make_file("100/fd/0", "");
    make_file("100/fd/1", "");
    make_file("100/fd/2", "");
    make_dir("101");
    make_dir("101/fd");
    make_file("101/status", "Name:\tkthreadd\nThreads:\t1\n");
    make_dir("102");
    make_dir("102/fd");
    make_file("102/status", "VmRSS:\t2048 kB\n");
    make_dir("103");
    make_file("103/status", "VmRSS:\t2048 kB\n");
    make_dir("104");
    make_dir("104/fd");
    make_file("104/status", "VmRSS:\t kB\n");
}

static void drop_tree(void)
{
    remove_path("100/fd/0", 0);
    remove_path("100/fd/1", 0);
    remove_path("100/fd/2", 0);
    remove_path("100/fd", 1);
    remove_path("100/status", 0);
    remove_path("100", 1);
    remove_path("101/status", 0);
    remove_path("101/fd", 1);
    remove_path("101", 1);
    remove_path("102/status", 0);
    remove_path("102/fd", 1);
    remove_path("102", 1);
    remove_path("103/status", 0);
    remove_path("103", 1);
    remove_path("104/status", 0);
    remove_path("104/fd", 1);
    remove_path("104", 1);
    CHECK(rmdir(root) == 0);
}

static void reads_fake_tree(void)
{
    long rss = 0;
    long fds = 0;

    CHECK(proc_stats(root, 100, &rss, &fds) == 0 && rss == 4096 && fds == 3);
    CHECK(proc_stats(root, 102, &rss, &fds) == 0 && rss == 2048 && fds == 0);
}

/* Anything unreadable gives -1 and -1, never a half-filled answer. */
static void gives_up_cleanly(void)
{
    static const pid_t bad[] = {999, 101, 103, 104}; /* missing, no VmRSS, no fd dir, no number */

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        long rss = 7;
        long fds = 7;

        CHECK(proc_stats(root, bad[i], &rss, &fds) == -1 && rss == -1 && fds == -1);
    }
}

static void reads_real_proc(void)
{
    long rss = 0;
    long fds = 0;

    CHECK(proc_stats("/proc", getpid(), &rss, &fds) == 0);
    CHECK(rss > 0 && fds >= 1); /* at least the descriptor the listing itself holds */
}

static void formats_line(void)
{
    char  line[256];
    child c;

    child_init(&c, "/x/gt-hub");
    CHECK(status_format(line, sizeof line, &c, -1, -1) < (int)sizeof line);
    CHECK(strcmp(line, "status child=gt-hub alive=0 state=starting degraded=0 rss_kb=-1 fds=-1 "
                       "restarts=0 faults=0") == 0);

    c.pid = 42;
    c.state = GT_CHILD_RUNNING;
    c.restarts = 3;
    c.faults = 2;
    CHECK(status_format(line, sizeof line, &c, 4096, 7) < (int)sizeof line);
    CHECK(strcmp(line, "status child=gt-hub alive=1 state=running degraded=0 rss_kb=4096 fds=7 "
                       "restarts=3 faults=2") == 0);

    c.degraded = true;
    c.state = 99; /* not a state we know */
    CHECK(status_format(line, sizeof line, &c, 4096, 7) < (int)sizeof line);
    CHECK(strstr(line, "state=unknown degraded=1 ") != NULL);

    CHECK(status_format(line, 10, &c, 4096, 7) >= 10); /* a cut line is reported */
}

int main(void)
{
    build_tree();
    reads_fake_tree();
    gives_up_cleanly();
    reads_real_proc();
    formats_line();
    drop_tree();
    return 0;
}
