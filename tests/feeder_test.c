/* feeder_test: runs the real gt-feeder as a child process, the way the P1 kill loop will,
 * and checks the log it leaves behind with store_read_range.  argv[1] is the feeder. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "store.h"

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            (void)fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

static const char* g_feeder;

static void path_of(char* out, size_t cap, const char* dir, const char* name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);

    CHECK(n > 0 && (size_t)n < cap);
}

static void make_tmp(char* dir, size_t cap)
{
    const char* base = getenv("TMPDIR");
    int         n = snprintf(dir, cap, "%s/gt-feeder-XXXXXX", base != NULL ? base : "/tmp");

    CHECK(n > 0 && (size_t)n < cap);
    CHECK(mkdtemp(dir) != NULL);
}

static void rm_tree(const char* dir)
{
    DIR*           dp = opendir(dir);
    struct dirent* ent;

    CHECK(dp != NULL);
    while ((ent = readdir(dp)) != NULL) {
        char path[512];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        path_of(path, sizeof path, dir, ent->d_name);
        if (unlink(path) != 0) {
            CHECK(errno == EISDIR);
            rm_tree(path);
        }
    }
    CHECK(closedir(dp) == 0);
    CHECK(rmdir(dir) == 0);
}

static double now_s(void)
{
    struct timespec ts;

    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* Starts the feeder with its stderr going to logfile; returns the pid. */
static pid_t spawn(const char* logfile, char* const args[])
{
    char*  argv[16];
    size_t n = 0;
    pid_t  pid;

    argv[n++] = (char*)g_feeder;
    for (size_t i = 0; args[i] != NULL; i++) {
        CHECK(n < 15);
        argv[n++] = args[i];
    }
    argv[n] = NULL;
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) {
            _exit(127);
        }
        (void)close(fd);
        execv(g_feeder, argv);
        _exit(127);
    }
    return pid;
}

/* Waits for the child and returns its exit code, or 128 + signal if it was killed. */
static int reap(pid_t pid)
{
    int st;

    CHECK(waitpid(pid, &st, 0) == pid);
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    CHECK(WIFSIGNALED(st));
    return 128 + WTERMSIG(st);
}

/* The whole log file, NUL terminated, in buf. */
static void slurp(const char* logfile, char* buf, size_t cap)
{
    FILE*  f = fopen(logfile, "r");
    size_t n;

    buf[0] = '\0';
    if (f == NULL) {
        return;
    }
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    CHECK(fclose(f) == 0);
}

/* The "start" line is the readiness signal: no signal is sent before it appears. */
static void wait_started(const char* logfile)
{
    char   buf[4096];
    double deadline = now_s() + 20.0;

    while (now_s() < deadline) {
        slurp(logfile, buf, sizeof buf);
        if (strstr(buf, "feeder start ") != NULL) {
            return;
        }
        sleep_ms(10);
    }
    CHECK(!"feeder never logged its start line");
}

typedef struct verify {
    uint64_t n;
    uint64_t last_ts;
} verify;

static int verify_fn(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    verify*  v = ctx;
    uint32_t seq = rec->seq;

    CHECK(status == GT_STORE_REC_OK);
    CHECK(seq == v->n + 1); /* contiguous from 1 */
    CHECK(rec->ts_unix_ms >= v->last_ts);
    CHECK(rec->soil_raw == 300u + (seq * 37u) % 700u);
    CHECK(rec->flags == ((seq % 2u != 0u) ? 0x0008u : 0u));
    CHECK(rec->light_bright_ms == (seq * 13u) % 60001u);
    CHECK(rec->light_edges == seq % 7u);
    v->n++;
    v->last_ts = rec->ts_unix_ms;
    return 0;
}

/* Reads the whole store, checks every record, returns how many there are. */
static uint64_t verify_store(const char* dir)
{
    verify v = {0, 0};

    CHECK(store_read_range(dir, 0, UINT64_MAX, verify_fn, &v) == 0);
    return v.n;
}

static int run(const char* logfile, char* const args[])
{
    return reap(spawn(logfile, args));
}

static void test_count_and_resume(void)
{
    char root[256];
    char store[300];
    char log[300];
    char text[8192];

    make_tmp(root, sizeof root);
    path_of(store, sizeof store, root, "store");
    path_of(log, sizeof log, root, "feeder.log");

    {
        char* args[] = {"--dir", store, "--rate", "0", "--count", "500", NULL};

        CHECK(run(log, args) == GT_EXIT_OK);
    }
    slurp(log, text, sizeof text);
    CHECK(strstr(text, "resumed_seq=1 ") != NULL);
    CHECK(strstr(text, "done records=500 last_seq=500 ") != NULL);
    CHECK(verify_store(store) == 500);

    {
        char* args[] = {"--dir", store, "--rate", "0", "--count", "200", NULL};

        CHECK(run(log, args) == GT_EXIT_OK);
    }
    slurp(log, text, sizeof text);
    CHECK(strstr(text, "resumed_seq=501 ") != NULL);
    CHECK(strstr(text, "done records=200 last_seq=700 ") != NULL);
    CHECK(verify_store(store) == 700);
    rm_tree(root);
}

/* Only a lower bound on the time: a slow or sanitized run must not make this flaky. */
static void test_pacing(void)
{
    char   root[256];
    char   store[300];
    char   log[300];
    double t0;
    char*  args[] = {"--dir", store, "--rate", "200", "--count", "100", NULL};

    make_tmp(root, sizeof root);
    path_of(store, sizeof store, root, "store");
    path_of(log, sizeof log, root, "feeder.log");
    t0 = now_s();
    CHECK(run(log, args) == GT_EXIT_OK);
    CHECK(now_s() - t0 >= 0.45); /* 99 gaps of 5 ms */
    CHECK(verify_store(store) == 100);
    rm_tree(root);
}

static void test_sigterm(void)
{
    char          root[256];
    char          store[300];
    char          log[300];
    char          text[8192];
    char*         args[] = {"--dir", store, NULL};
    unsigned long long reported = 0;
    const char*   done;
    pid_t         pid;
    uint64_t      n;

    make_tmp(root, sizeof root);
    path_of(store, sizeof store, root, "store");
    path_of(log, sizeof log, root, "feeder.log");
    pid = spawn(log, args);
    wait_started(log);
    sleep_ms(200);
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(reap(pid) == GT_EXIT_OK);
    slurp(log, text, sizeof text);
    done = strstr(text, "feeder done records=");
    CHECK(done != NULL);
    CHECK(sscanf(done, "feeder done records=%llu", &reported) == 1);
    n = verify_store(store);
    CHECK(n > 0 && n == reported);
    rm_tree(root);
}

static void test_sigkill_then_restart(void)
{
    char     root[256];
    char     store[300];
    char     log[300];
    char     text[8192];
    char     want[64];
    char*    args[] = {"--dir", store, NULL};
    char*    again[] = {"--dir", store, "--rate", "0", "--count", "100", NULL};
    pid_t    pid;
    uint64_t before;

    make_tmp(root, sizeof root);
    path_of(store, sizeof store, root, "store");
    path_of(log, sizeof log, root, "feeder.log");
    pid = spawn(log, args);
    wait_started(log);
    sleep_ms(200);
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(reap(pid) == 128 + SIGKILL);
    before = verify_store(store); /* intact up to the kill (a torn tail is skipped) */
    CHECK(before > 0);

    CHECK(run(log, again) == GT_EXIT_OK);
    slurp(log, text, sizeof text);
    (void)snprintf(want, sizeof want, "resumed_seq=%llu ", (unsigned long long)before + 1);
    CHECK(strstr(text, want) != NULL);
    CHECK(verify_store(store) == before + 100);
    rm_tree(root);
}

/* The progress file: 4 bytes, little-endian, the last seq the store acknowledged. */
static uint32_t read_progress(const char* path)
{
    uint8_t b[5];
    FILE*   f = fopen(path, "rb");

    CHECK(f != NULL);
    CHECK(fread(b, 1, sizeof b, f) == 4); /* exactly four bytes */
    CHECK(fclose(f) == 0);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void test_progress_file(void)
{
    char     root[256];
    char     store[300];
    char     log[300];
    char     prog[300];
    char     bad[300];
    char*    counted[] = {"--dir", store, "--rate", "0", "--count", "500", "--progress", prog, NULL};
    char*    endless[] = {"--dir", store, "--progress", prog, NULL};
    char*    unwritable[] = {"--dir", store, "--count", "1", "--progress", bad, NULL};
    pid_t    pid;
    uint64_t n;
    uint32_t acked;

    make_tmp(root, sizeof root);
    path_of(store, sizeof store, root, "store");
    path_of(log, sizeof log, root, "feeder.log");
    path_of(prog, sizeof prog, root, "progress");
    path_of(bad, sizeof bad, root, "missing-dir/progress");

    CHECK(run(log, counted) == GT_EXIT_OK);
    CHECK(read_progress(prog) == 500);

    /* Killed mid-run: the page-cache write survives, and trails the log by at most the one
     * record that was acknowledged just before the kill. */
    pid = spawn(log, endless);
    wait_started(log);
    sleep_ms(200);
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(reap(pid) == 128 + SIGKILL);
    n = verify_store(store);
    acked = read_progress(prog);
    CHECK(n > 500 && acked <= n && n - acked <= 1);

    CHECK(run(log, unwritable) == GT_EXIT_FATAL);
    rm_tree(root);
}

static void test_bad_arguments(void)
{
    char  root[256];
    char  log[300];
    char  file[300];
    char* none[] = {NULL};
    char* no_dir[] = {"--rate", "10", NULL};
    char* bad_rate[] = {"--dir", "x", "--rate", "abc", NULL};
    char* neg_rate[] = {"--dir", "x", "--rate", "-1", NULL};
    char* huge_rate[] = {"--dir", "x", "--rate", "99999999", NULL};
    char* bad_flag[] = {"--bogus", NULL};
    char* no_value[] = {"--dir", NULL};
    char* on_file[] = {"--dir", file, "--count", "1", NULL};
    int   fd;

    make_tmp(root, sizeof root);
    path_of(log, sizeof log, root, "feeder.log");
    path_of(file, sizeof file, root, "not-a-dir");
    CHECK(run(log, none) == GT_EXIT_CONFIG);
    CHECK(run(log, no_dir) == GT_EXIT_CONFIG);
    CHECK(run(log, bad_rate) == GT_EXIT_CONFIG);
    CHECK(run(log, neg_rate) == GT_EXIT_CONFIG);
    CHECK(run(log, huge_rate) == GT_EXIT_CONFIG);
    CHECK(run(log, bad_flag) == GT_EXIT_CONFIG);
    CHECK(run(log, no_value) == GT_EXIT_CONFIG);

    fd = open(file, O_WRONLY | O_CREAT, 0644); /* a store path that is a regular file */
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(run(log, on_file) == GT_EXIT_FATAL);
    rm_tree(root);
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    g_feeder = argv[1];
    test_count_and_resume();
    test_pacing();
    test_sigterm();
    test_sigkill_then_restart();
    test_progress_file();
    test_bad_arguments();
    (void)puts("PASS");
    return 0;
}
