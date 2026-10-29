/* store_test: write path of the storage layer (CRC32, record encoding, segments, append,
 * the directory lock and the tail check).  It reads the segment files directly, because
 * store_read_range is a separate piece of work. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.h"
#include "crc32.h"
#include "store.h"

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            (void)fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

/* 1700000000000 is 2023-11-14 22:13:20 UTC; the next two days start at these. */
#define T_BASE 1700000000000ull
#define T_MIN  60000ull
#define T_DAY2 1700006400000ull /* 2023-11-15 00:00:00 UTC */
#define T_DAY3 1700092800000ull /* 2023-11-16 00:00:00 UTC */

static void path_of(char* out, size_t cap, const char* dir, const char* name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);

    CHECK(n > 0 && (size_t)n < cap);
}

static void make_tmp(char* dir, size_t cap)
{
    const char* base = getenv("TMPDIR");
    int         n = snprintf(dir, cap, "%s/gt-store-XXXXXX", base != NULL ? base : "/tmp");

    CHECK(n > 0 && (size_t)n < cap);
    CHECK(mkdtemp(dir) != NULL);
}

/* Removes dir and everything under it. */
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

static gt_record make_rec(uint64_t ts, uint32_t seq)
{
    gt_record r;

    memset(&r, 0, sizeof r);
    r.ts_unix_ms = ts;
    r.seq = seq;
    r.soil_raw = (uint16_t)(500 + seq % 400);
    r.flags = (uint16_t)((seq % 2) != 0 ? GT_REC_LIGHT_BRIGHT : 0u);
    r.light_bright_ms = (uint16_t)(seq * 10);
    r.light_edges = (uint16_t)(seq % 5);
    return r;
}

static bool same(const gt_record* a, const gt_record* b)
{
    return a->ts_unix_ms == b->ts_unix_ms && a->seq == b->seq && a->soil_raw == b->soil_raw &&
           a->flags == b->flags && a->light_bright_ms == b->light_bright_ms &&
           a->light_edges == b->light_edges;
}

/* Reads dir/name whole into buf; returns its length. */
static size_t read_file(const char* dir, const char* name, uint8_t* buf, size_t cap)
{
    char    path[512];
    ssize_t n;
    int     fd;

    path_of(path, sizeof path, dir, name);
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    n = read(fd, buf, cap);
    CHECK(n >= 0 && (size_t)n < cap);
    CHECK(close(fd) == 0);
    return (size_t)n;
}

static off_t file_size(const char* dir, const char* name)
{
    char        path[512];
    struct stat st;

    path_of(path, sizeof path, dir, name);
    CHECK(stat(path, &st) == 0);
    return st.st_size;
}

static void append_ok(gt_store* s, uint64_t ts, uint32_t seq)
{
    gt_record r = make_rec(ts, seq);

    CHECK(store_append(s, &r) == 0);
}

static void test_crc(void)
{
    CHECK(gt_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
    CHECK(gt_crc32((const uint8_t*)"", 0) == 0u);
}

static void test_codec(void)
{
    static const uint8_t want[20] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
                                     0x01, 0x0D, 0x0C, 0x0B, 0x0A, 0x34, 0x12,
                                     0x08, 0x00, 0xFF, 0x00, 0x03, 0x00};
    gt_record            rec;
    gt_record            back;
    uint8_t              b[GT_RECORD_SIZE];
    uint32_t             crc;

    memset(&rec, 0, sizeof rec);
    rec.ts_unix_ms = 0x0102030405060708ull;
    rec.seq = 0x0A0B0C0Du;
    rec.soil_raw = 0x1234u;
    rec.flags = GT_REC_LIGHT_BRIGHT;
    rec.light_bright_ms = 0x00FFu;
    rec.light_edges = 3u;
    rec.crc32 = 0xDEADBEEFu; /* ignored by the encoder */

    record_encode(&rec, b);
    CHECK(memcmp(b, want, sizeof want) == 0);
    crc = gt_crc32(want, sizeof want);
    CHECK(b[20] == (uint8_t)crc && b[21] == (uint8_t)(crc >> 8) && b[22] == (uint8_t)(crc >> 16) &&
          b[23] == (uint8_t)(crc >> 24));

    CHECK(record_decode(b, &back));
    CHECK(same(&rec, &back) && back.crc32 == crc);

    b[5] ^= 0x01u; /* one flipped bit must fail the CRC */
    CHECK(!record_decode(b, &back));
}

static void test_append_roundtrip(void)
{
    enum { N = 50 };
    char              dir[256];
    char              sub[300];
    uint8_t           buf[N * GT_RECORD_SIZE + 1];
    gt_store*         s = NULL;
    gt_store_recovery info;

    make_tmp(dir, sizeof dir);
    path_of(sub, sizeof sub, dir, "store"); /* does not exist; store_recover creates it */

    CHECK(store_recover(sub, &s, &info) == 0 && s != NULL);
    CHECK(info.has_records == 0 && info.segment[0] == '\0' && info.dropped_bytes == 0);
    for (uint32_t i = 0; i < N; i++) {
        append_ok(s, T_BASE + i * T_MIN, i + 1);
    }
    store_close(s);

    CHECK(read_file(sub, "2023-11-14.log", buf, sizeof buf) == (size_t)N * GT_RECORD_SIZE);
    for (uint32_t i = 0; i < N; i++) {
        gt_record want = make_rec(T_BASE + i * T_MIN, i + 1);
        gt_record got;

        CHECK(record_decode(buf + i * GT_RECORD_SIZE, &got));
        CHECK(same(&want, &got));
    }
    rm_tree(dir);
}

/* Day rollover, a clock that goes backwards, and an empty newest segment. */
static void test_segments(void)
{
    char              dir[256];
    char              path[512];
    uint8_t           buf[4 * GT_RECORD_SIZE];
    gt_store*         s = NULL;
    gt_store_recovery info;
    gt_record         got;
    int               fd;

    make_tmp(dir, sizeof dir);
    CHECK(store_recover(dir, &s, NULL) == 0);
    append_ok(s, T_DAY2 - 1000, 1);   /* 2023-11-14 23:59:59 */
    append_ok(s, T_DAY2 + 1000, 2);   /* 2023-11-15 00:00:01 */
    append_ok(s, T_BASE, 3);          /* earlier than the open segment: goes to the newest */
    store_close(s);

    CHECK(file_size(dir, "2023-11-14.log") == (off_t)GT_RECORD_SIZE);
    CHECK(file_size(dir, "2023-11-15.log") == (off_t)(2 * GT_RECORD_SIZE));
    CHECK(read_file(dir, "2023-11-15.log", buf, sizeof buf) == 2 * GT_RECORD_SIZE);
    CHECK(record_decode(buf + GT_RECORD_SIZE, &got) && got.seq == 3 && got.ts_unix_ms == T_BASE);

    /* A segment created but never written to must not hide the last sequence number. */
    path_of(path, sizeof path, dir, "2023-11-16.log");
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0 && close(fd) == 0);

    CHECK(store_recover(dir, &s, &info) == 0);
    CHECK(info.has_records == 1 && info.last_seq == 3);
    CHECK(strcmp(info.segment, "2023-11-16.log") == 0);
    append_ok(s, T_DAY3 + 1000, 4);
    store_close(s);
    CHECK(file_size(dir, "2023-11-16.log") == (off_t)GT_RECORD_SIZE);
    rm_tree(dir);
}

static void test_lock_and_reopen(void)
{
    char              dir[256];
    gt_store*         s1 = NULL;
    gt_store*         s2 = NULL;
    gt_store_recovery info;

    make_tmp(dir, sizeof dir);
    CHECK(store_recover(dir, &s1, NULL) == 0);
    append_ok(s1, T_BASE, 1);
    append_ok(s1, T_BASE + T_MIN, 2);

    errno = 0;
    CHECK(store_recover(dir, &s2, NULL) == -1 && errno == EBUSY && s2 == NULL);
    store_close(s1);

    CHECK(store_recover(dir, &s2, &info) == 0);
    CHECK(info.has_records == 1 && info.last_seq == 2);
    CHECK(strcmp(info.segment, "2023-11-14.log") == 0);
    append_ok(s2, T_BASE + 2 * T_MIN, 3);
    store_close(s2);
    CHECK(file_size(dir, "2023-11-14.log") == (off_t)(3 * GT_RECORD_SIZE));
    rm_tree(dir);
}

#define SEG1 "2023-11-14.log"
#define SEG2 "2023-11-15.log"
#define REC  ((off_t)GT_RECORD_SIZE)

/* Writes n records (seq 1..n, one minute apart) into dir's first segment. */
static void fill(const char* dir, uint32_t n)
{
    gt_store* s = NULL;

    CHECK(store_recover(dir, &s, NULL) == 0);
    for (uint32_t i = 0; i < n; i++) {
        append_ok(s, T_BASE + i * T_MIN, i + 1);
    }
    store_close(s);
}

/* Damages one byte of dir/name at offset at, so its record fails its CRC. */
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

/* Cuts dir/name to len bytes, as a torn write would. */
static void cut(const char* dir, const char* name, off_t len)
{
    char path[512];

    path_of(path, sizeof path, dir, name);
    CHECK(truncate(path, len) == 0);
}

/* Damage beyond one interrupted write: store_recover refuses with EUCLEAN and the
 * segment is left byte for byte as it was. */
static void expect_refused(const char* dir, const char* name)
{
    uint8_t   before[512];
    uint8_t   after[512];
    gt_store* s = NULL;
    size_t    n = read_file(dir, name, before, sizeof before);

    errno = 0;
    CHECK(store_recover(dir, &s, NULL) == -1 && errno == EUCLEAN && s == NULL);
    CHECK(read_file(dir, name, after, sizeof after) == n && memcmp(before, after, n) == 0);
}

/* One interrupted write is repaired: the damaged final record is cut, the rest is kept. */
static void test_recover_torn_tail(void)
{
    char              dir[256];
    uint8_t           buf[4 * GT_RECORD_SIZE];
    gt_store*         s = NULL;
    gt_store_recovery info;
    gt_record         got;

    /* A short final record: 43 bytes is one whole record plus 19 stray bytes. */
    make_tmp(dir, sizeof dir);
    fill(dir, 2);
    cut(dir, SEG1, 2 * REC - 5);
    CHECK(store_recover(dir, &s, &info) == 0);
    CHECK(info.has_records == 1 && info.last_seq == 1 && info.dropped_bytes == 19);
    CHECK(strcmp(info.segment, SEG1) == 0 && file_size(dir, SEG1) == REC);
    append_ok(s, T_BASE + T_MIN, 2); /* appends continue cleanly behind the kept record */
    store_close(s);
    CHECK(read_file(dir, SEG1, buf, sizeof buf) == 2 * GT_RECORD_SIZE);
    CHECK(record_decode(buf, &got) && got.seq == 1);
    CHECK(record_decode(buf + GT_RECORD_SIZE, &got) && got.seq == 2);
    rm_tree(dir);

    /* A full-size final record with a bad CRC. */
    make_tmp(dir, sizeof dir);
    fill(dir, 2);
    flip(dir, SEG1, REC + 6);
    CHECK(store_recover(dir, &s, &info) == 0);
    CHECK(info.has_records == 1 && info.last_seq == 1 && info.dropped_bytes == GT_RECORD_SIZE);
    CHECK(file_size(dir, SEG1) == REC);
    store_close(s);
    rm_tree(dir);

    /* The caller does not have to ask for the report. */
    make_tmp(dir, sizeof dir);
    fill(dir, 2);
    cut(dir, SEG1, 2 * REC - 5);
    CHECK(store_recover(dir, &s, NULL) == 0 && file_size(dir, SEG1) == REC);
    store_close(s);
    rm_tree(dir);
}

/* More damage than one interrupted write can cause points at the card, not at a crash:
 * recovery stops and tells, and does not delete anything. */
static void test_recover_refuses_more_damage(void)
{
    char dir[256];

    make_tmp(dir, sizeof dir);
    fill(dir, 2);
    flip(dir, SEG1, 5);        /* record 1 is bad ... */
    cut(dir, SEG1, 2 * REC - 5); /* ... and a torn record follows it */
    expect_refused(dir, SEG1);
    rm_tree(dir);

    make_tmp(dir, sizeof dir);
    fill(dir, 3);
    flip(dir, SEG1, REC + 5);     /* two bad records at the end */
    flip(dir, SEG1, 2 * REC + 5);
    expect_refused(dir, SEG1);
    rm_tree(dir);
}

/* Recovery only looks at the end of the file. */
static void test_recover_leaves_the_rest(void)
{
    char              dir[256];
    gt_store*         s = NULL;
    gt_store_recovery info;

    /* Damage in the middle is the reader's business (it reports the gap), not recovery's. */
    make_tmp(dir, sizeof dir);
    fill(dir, 3);
    flip(dir, SEG1, REC + 5);
    CHECK(store_recover(dir, &s, &info) == 0);
    CHECK(info.has_records == 1 && info.last_seq == 3 && info.dropped_bytes == 0);
    CHECK(file_size(dir, SEG1) == 3 * REC);
    store_close(s);
    rm_tree(dir);

    /* A clean log is not touched, however often it is recovered. */
    make_tmp(dir, sizeof dir);
    fill(dir, 3);
    for (int i = 0; i < 2; i++) {
        CHECK(store_recover(dir, &s, &info) == 0);
        CHECK(info.last_seq == 3 && info.dropped_bytes == 0 && file_size(dir, SEG1) == 3 * REC);
        store_close(s);
    }
    rm_tree(dir);
}

/* The only record in the newest segment was the torn one: the segment becomes empty and
 * the sequence number comes from the older segment. */
static void test_recover_empties_newest(void)
{
    char              dir[256];
    gt_store*         s = NULL;
    gt_store_recovery info;

    make_tmp(dir, sizeof dir);
    CHECK(store_recover(dir, &s, NULL) == 0);
    append_ok(s, T_BASE, 1);
    append_ok(s, T_DAY2 + 1000, 2);
    store_close(s);
    cut(dir, SEG2, 10);

    CHECK(store_recover(dir, &s, &info) == 0);
    CHECK(info.has_records == 1 && info.last_seq == 1 && info.dropped_bytes == 10);
    CHECK(strcmp(info.segment, SEG2) == 0 && file_size(dir, SEG2) == 0);
    append_ok(s, T_DAY2 + 1000, 2);
    store_close(s);
    CHECK(file_size(dir, SEG2) == REC);
    rm_tree(dir);
}

static void test_bad_args(void)
{
    char      dir[256];
    gt_store* s = NULL;
    gt_record r = make_rec(T_BASE, 1);

    errno = 0;
    CHECK(store_recover(NULL, &s, NULL) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(store_recover("/nonexistent-dir/x", NULL, NULL) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(store_append(NULL, &r) == -1 && errno == EINVAL);
    store_close(NULL);

    make_tmp(dir, sizeof dir);
    CHECK(store_recover(dir, &s, NULL) == 0);
    errno = 0;
    CHECK(store_append(s, NULL) == -1 && errno == EINVAL);

    /* A timestamp whose year does not fit four digits is refused and writes nothing. */
    append_ok(s, T_BASE, 1);
    r = make_rec(UINT64_MAX, 2);
    errno = 0;
    CHECK(store_append(s, &r) == -1 && errno == EINVAL);
    append_ok(s, T_BASE + T_MIN, 2); /* the store is still usable */
    store_close(s);
    CHECK(file_size(dir, "2023-11-14.log") == (off_t)(2 * GT_RECORD_SIZE));
    rm_tree(dir);
}

int main(void)
{
    test_crc();
    test_codec();
    test_append_roundtrip();
    test_segments();
    test_lock_and_reopen();
    test_recover_torn_tail();
    test_recover_refuses_more_damage();
    test_recover_leaves_the_rest();
    test_recover_empties_newest();
    test_bad_args();
    puts("PASS");
    return 0;
}
