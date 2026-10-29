/* logcheck_test: runs the real gt-logcheck as a child process against logs built with
 * store_append (clean, corrupted, gapped, with hand-written writer logs and progress files),
 * then once end to end with the real gt-feeder, as the P1 kill loop will.
 * argv[1] is gt-logcheck, argv[2] is gt-feeder. */
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

/* 1700000000000 is 2023-11-14 22:13:20 UTC. */
#define T_BASE 1700000000000ull
#define SEG1   "2023-11-14.log"
#define REC    24

static const char* g_check;
static const char* g_feeder;

static void path_of(char* out, size_t cap, const char* dir, const char* name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);

    CHECK(n > 0 && (size_t)n < cap);
}

static void make_tmp(char* dir, size_t cap)
{
    const char* base = getenv("TMPDIR");
    int         n = snprintf(dir, cap, "%s/gt-logcheck-XXXXXX", base != NULL ? base : "/tmp");

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

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static double now_s(void)
{
    struct timespec ts;

    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Starts prog with stdout and stderr appended to the given files (NULL: /dev/null). */
static pid_t spawn(const char* prog, const char* out_path, const char* err_path,
                   char* const args[])
{
    char*  argv[16];
    size_t n = 0;
    pid_t  pid;

    argv[n++] = (char*)prog;
    for (size_t i = 0; args[i] != NULL; i++) {
        CHECK(n < 15);
        argv[n++] = args[i];
    }
    argv[n] = NULL;
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int out = open(out_path != NULL ? out_path : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
        int err = open(err_path != NULL ? err_path : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (out < 0 || err < 0 || dup2(out, STDOUT_FILENO) < 0 || dup2(err, STDERR_FILENO) < 0) {
            _exit(127);
        }
        (void)close(out);
        (void)close(err);
        execv(prog, argv);
        _exit(127);
    }
    return pid;
}

/* Exit code, or 128 + signal if the child was killed. */
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

static void slurp(const char* path, char* buf, size_t cap)
{
    FILE*  f = fopen(path, "r");
    size_t n;

    buf[0] = '\0';
    if (f == NULL) {
        return;
    }
    n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    CHECK(fclose(f) == 0);
}

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

/* Runs gt-logcheck in a scratch dir; returns its exit code and puts its stdout in out. */
static int checker(const char* scratch, char* const args[], char* out, size_t cap)
{
    char o[512];
    char e[512];
    int  rc;

    path_of(o, sizeof o, scratch, "check.out");
    path_of(e, sizeof e, scratch, "check.err");
    CHECK(unlink(o) == 0 || errno == ENOENT);
    rc = reap(spawn(g_check, o, e, args));
    slurp(o, out, cap);
    return rc;
}

static void write_bytes(const char* path, const void* data, size_t len)
{
    FILE* f = fopen(path, "wb");

    CHECK(f != NULL);
    CHECK(fwrite(data, 1, len, f) == len);
    CHECK(fclose(f) == 0);
}

static void write_progress(const char* path, uint32_t seq)
{
    uint8_t b[4] = {(uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24)};

    write_bytes(path, b, sizeof b);
}

/* Appends one record per entry of seqs, one second apart, in the 2023-11-14 segment. */
static void build_log(const char* dir, const uint32_t* seqs, size_t n)
{
    gt_store* s = NULL;

    CHECK(store_recover(dir, &s, NULL) == 0);
    for (size_t i = 0; i < n; i++) {
        gt_record r;

        memset(&r, 0, sizeof r);
        r.ts_unix_ms = T_BASE + i * 1000u;
        r.seq = seqs[i];
        r.soil_raw = 500;
        CHECK(store_append(s, &r) == 0);
    }
    store_close(s);
}

static void build_clean(const char* dir, uint32_t n)
{
    uint32_t seqs[200];

    CHECK(n <= 200);
    for (uint32_t i = 0; i < n; i++) {
        seqs[i] = i + 1;
    }
    build_log(dir, seqs, n);
}

static void flip(const char* dir, const char* name, off_t at)
{
    char          path[512];
    unsigned char b;
    int           fd;

    path_of(path, sizeof path, dir, name);
    fd = open(path, O_RDWR);
    CHECK(fd >= 0 && pread(fd, &b, 1, at) == 1);
    b ^= 0x5Au;
    CHECK(pwrite(fd, &b, 1, at) == 1 && close(fd) == 0);
}

/* A scratch dir holding the store at root/store plus room for logs and progress files. */
typedef struct env {
    char root[256];
    char store[300];
    char wlog[300];
    char prog[300];
} env;

static void env_init(env* e)
{
    make_tmp(e->root, sizeof e->root);
    path_of(e->store, sizeof e->store, e->root, "store");
    path_of(e->wlog, sizeof e->wlog, e->root, "writer.log");
    path_of(e->prog, sizeof e->prog, e->root, "progress");
}

static void test_clean_log(void)
{
    env  e;
    char out[1024];
    char* args[] = {"--dir", NULL, NULL};

    env_init(&e);
    build_clean(e.store, 50);
    args[1] = e.store;
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    CHECK(strstr(out, "records=50 first_seq=1 last_seq=50 lost=0 corrupt=0") != NULL);
    CHECK(strstr(out, "PASS") != NULL && strstr(out, "FAIL") == NULL);
    rm_tree(e.root);
}

static void test_empty_and_missing(void)
{
    env   e;
    char  out[1024];
    char  missing[300];
    char* empty[] = {"--dir", NULL, NULL};
    char* gone[] = {"--dir", missing, NULL};

    env_init(&e);
    CHECK(mkdir(e.store, 0755) == 0);
    empty[1] = e.store;
    CHECK(checker(e.root, empty, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL no records") != NULL);

    path_of(missing, sizeof missing, e.root, "nope");
    CHECK(checker(e.root, gone, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL cannot read the store") != NULL);
    rm_tree(e.root);
}

static void test_corruption(void)
{
    env   e;
    char  out[1024];
    char  seg[300];
    char* args[] = {"--dir", NULL, NULL};

    env_init(&e);
    build_clean(e.store, 10);
    args[1] = e.store;
    flip(e.store, SEG1, 4 * REC + 5); /* record 5, in the middle */
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "corrupt=1") != NULL && strstr(out, "FAIL 1 record(s) failed their CRC") != NULL);
    rm_tree(e.root);

    /* Damage at the tail of the newest segment is a torn write the next recovery repairs. */
    env_init(&e);
    build_clean(e.store, 10);
    args[1] = e.store;
    flip(e.store, SEG1, 9 * REC + 5);
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    CHECK(strstr(out, "records=9 ") != NULL && strstr(out, "PASS") != NULL);
    path_of(seg, sizeof seg, e.store, SEG1);
    CHECK(truncate(seg, 8 * REC + 7) == 0); /* now a short final record as well */
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    CHECK(strstr(out, "records=8 ") != NULL);
    rm_tree(e.root);
}

static void test_seq_rules(void)
{
    static const uint32_t one_lost[] = {1, 2, 4};
    static const uint32_t two_lost[] = {1, 2, 5};
    static const uint32_t repeated[] = {1, 2, 2};
    static const uint32_t backwards[] = {1, 3, 2};
    env                   e;
    char                  out[1024];
    char*                 args[] = {"--dir", NULL, NULL};

    env_init(&e);
    args[1] = e.store;

    build_log(e.store, one_lost, 3);
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    CHECK(strstr(out, "lost=1") != NULL && strstr(out, "PASS") != NULL);
    rm_tree(e.store);

    build_log(e.store, two_lost, 3);
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL seq skipped more than one number 1 time(s), first after seq=2") != NULL);
    rm_tree(e.store);

    build_log(e.store, repeated, 3);
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL seq repeated or went backwards 1 time(s), first after seq=2") != NULL);
    rm_tree(e.store);

    build_log(e.store, backwards, 3);
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL seq repeated or went backwards") != NULL);
    rm_tree(e.root);
}

static void test_writer_log(void)
{
    static const char clean[] =
        "2026-10-29T04:52:49Z INFO store recovered segment=none dropped_bytes=0 last_seq=none\n"
        "2026-10-29T04:52:49Z INFO feeder start dir=x rate=1000 resumed_seq=1 dropped_bytes=0\n";
    static const char torn[] =
        "2026-10-29T04:52:50Z WARN store recovered segment=2023-11-14.log dropped_bytes=17 last_seq=4811\n";
    static const char too_much[] =
        "2026-10-29T04:52:50Z WARN store recovered segment=2023-11-14.log dropped_bytes=25 last_seq=4811\n";
    static const char no_entry[] =
        "2026-10-29T04:52:49Z INFO feeder start dir=x rate=1000 resumed_seq=1 dropped_bytes=0\n";
    static const char bad_entry[] = "2026-10-29T04:52:49Z INFO store recovered segment=x\n";
    static const char error_line[] =
        "2026-10-29T04:52:51Z ERROR store append_failed errno=5 seq=9\n";
    env   e;
    char  out[1024];
    char  text[1024];
    char* args[] = {"--dir", NULL, "--writer-log", NULL, NULL};
    char* two[] = {"--dir", NULL, "--writer-log", NULL, "--restarts", "2", NULL};

    env_init(&e);
    build_clean(e.store, 5);
    args[1] = two[1] = e.store;
    args[3] = two[3] = e.wlog;

    write_bytes(e.wlog, clean, strlen(clean));
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    CHECK(strstr(out, "recovered=1 max_dropped=0") != NULL);

    (void)snprintf(text, sizeof text, "%s%s", clean, torn); /* a torn write: 17 bytes cut */
    write_bytes(e.wlog, text, strlen(text));
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    CHECK(strstr(out, "recovered=2 max_dropped=17") != NULL);

    write_bytes(e.wlog, too_much, strlen(too_much));
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL recovery dropped 25 bytes, more than one record (24)") != NULL);

    write_bytes(e.wlog, no_entry, strlen(no_entry));
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL no 'store recovered' entry") != NULL);

    write_bytes(e.wlog, bad_entry, strlen(bad_entry));
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "without a readable dropped_bytes") != NULL);

    (void)snprintf(text, sizeof text, "%s%s", clean, error_line);
    write_bytes(e.wlog, text, strlen(text));
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL 1 ERROR line(s) in the writer log") != NULL);

    (void)snprintf(text, sizeof text, "%s%s%s", clean, torn, torn); /* three starts */
    write_bytes(e.wlog, text, strlen(text));
    CHECK(checker(e.root, two, out, sizeof out) == 0);
    (void)snprintf(text, sizeof text, "%s%s", clean, torn); /* only two starts */
    write_bytes(e.wlog, text, strlen(text));
    CHECK(checker(e.root, two, out, sizeof out) == 1);
    CHECK(strstr(out, "expected 3 'store recovered' entries for 2 restart(s), found 2") != NULL);

    CHECK(unlink(e.wlog) == 0); /* a writer log that does not exist */
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL cannot read the writer log") != NULL);
    rm_tree(e.root);
}

static void test_progress(void)
{
    env   e;
    char  out[1024];
    char* args[] = {"--dir", NULL, "--progress", NULL, NULL};

    env_init(&e);
    build_clean(e.store, 50);
    args[1] = e.store;
    args[3] = e.prog;

    write_progress(e.prog, 50); /* acknowledged and present */
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    CHECK(strstr(out, "acked=50") != NULL);
    write_progress(e.prog, 49); /* the feeder trails the log by one: fine */
    CHECK(checker(e.root, args, out, sizeof out) == 0);
    write_progress(e.prog, 51); /* an acknowledged record is missing */
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL the log ends at seq=50 but seq=51 was acknowledged") != NULL);
    write_bytes(e.prog, "ab", 2); /* too short to be a seq */
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL cannot read the progress file") != NULL);
    CHECK(unlink(e.prog) == 0);
    CHECK(checker(e.root, args, out, sizeof out) == 1);
    CHECK(strstr(out, "FAIL cannot read the progress file") != NULL);
    rm_tree(e.root);
}

/* The P1 chain once: kill the feeder, check, restart it, check again, then damage the log. */
static void test_end_to_end(void)
{
    env   e;
    char  out[1024];
    char  seg[300];
    char* feed[] = {"--dir", NULL, "--progress", NULL, NULL};
    char* again[] = {"--dir", NULL, "--rate", "0", "--count", "100", NULL};
    char* after_kill[] = {"--dir", NULL, "--progress", NULL, NULL};
    char* after_restart[] = {"--dir", NULL, "--writer-log", NULL, "--restarts", "1", NULL};
    pid_t pid;
    DIR*  dp;
    struct dirent* ent;

    env_init(&e);
    feed[1] = again[1] = after_kill[1] = after_restart[1] = e.store;
    feed[3] = after_kill[3] = e.prog;
    after_restart[3] = e.wlog;

    pid = spawn(g_feeder, NULL, e.wlog, feed);
    wait_started(e.wlog);
    sleep_ms(200);
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(reap(pid) == 128 + SIGKILL);
    CHECK(checker(e.root, after_kill, out, sizeof out) == 0); /* writer stopped: progress holds */
    CHECK(strstr(out, "PASS") != NULL && strstr(out, "acked=") != NULL);

    CHECK(reap(spawn(g_feeder, NULL, e.wlog, again)) == GT_EXIT_OK);
    CHECK(checker(e.root, after_restart, out, sizeof out) == 0);
    CHECK(strstr(out, "recovered=2") != NULL && strstr(out, "PASS") != NULL);

    dp = opendir(e.store);
    CHECK(dp != NULL);
    seg[0] = '\0';
    while ((ent = readdir(dp)) != NULL) {
        size_t len = strlen(ent->d_name);

        if (len > 4 && strcmp(ent->d_name + len - 4, ".log") == 0 &&
            (seg[0] == '\0' || strcmp(ent->d_name, seg) < 0)) {
            CHECK(len < sizeof seg);
            memcpy(seg, ent->d_name, len + 1);
        }
    }
    CHECK(closedir(dp) == 0 && seg[0] != '\0');
    flip(e.store, seg, 3 * REC + 5);
    CHECK(checker(e.root, after_restart, out, sizeof out) == 1);
    CHECK(strstr(out, "corrupt=1") != NULL);
    rm_tree(e.root);
}

static void test_bad_arguments(void)
{
    env   e;
    char  out[1024];
    char* none[] = {NULL};
    char* bogus[] = {"--bogus", NULL};
    char* no_value[] = {"--dir", NULL};
    char* orphan[] = {"--dir", "x", "--restarts", "1", NULL};
    char* bad_n[] = {"--dir", "x", "--writer-log", "y", "--restarts", "abc", NULL};

    env_init(&e);
    CHECK(checker(e.root, none, out, sizeof out) == 2);
    CHECK(checker(e.root, bogus, out, sizeof out) == 2);
    CHECK(checker(e.root, no_value, out, sizeof out) == 2);
    CHECK(checker(e.root, orphan, out, sizeof out) == 2);
    CHECK(checker(e.root, bad_n, out, sizeof out) == 2);
    rm_tree(e.root);
}

int main(int argc, char** argv)
{
    CHECK(argc == 3);
    g_check = argv[1];
    g_feeder = argv[2];
    test_clean_log();
    test_empty_and_missing();
    test_corruption();
    test_seq_rules();
    test_writer_log();
    test_progress();
    test_end_to_end();
    test_bad_arguments();
    (void)puts("PASS");
    return 0;
}
