/* interface_page_test: what the LAN page decides and says, with no network and no store on
 * disk.  Exits 1 at the first failed check. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daemon.h"
#include "page.h"

#define CHECK(c)                                                                       \
    do {                                                                                \
        if (!(c)) {                                                                     \
            (void)fprintf(stderr, "interface_page_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

#define NOW      1700000000000ull
#define GB       (1024ull * 1024ull * 1024ull)
#define HTML_CAP IFACE_BODY_CAP /* the daemon's own limit: a page that does not fit it is never served */

static gt_record rec(uint64_t ts, uint32_t seq, uint16_t soil, uint16_t flags)
{
    gt_record r;

    (void)memset(&r, 0, sizeof r);
    r.ts_unix_ms = ts;
    r.seq = seq;
    r.soil_raw = soil;
    r.flags = flags;
    return r;
}

static void add(page_acc* a, gt_record r)
{
    CHECK(page_acc_visit(&r, GT_STORE_REC_OK, a) == 0);
}

static void judge(const page_acc* a, uint64_t now, int err, bool disk_known, uint64_t disk, page_view* v)
{
    page_input in = {a, now, err, disk_known, disk};

    page_judge(&in, v);
}

/* The common case: readable store, plenty of disk. */
static void judge_ok(const page_acc* a, uint64_t now, page_view* v)
{
    judge(a, now, 0, true, GB, v);
}

static bool has(const char* text, const char* needle)
{
    return strstr(text, needle) != NULL;
}

static void empty_store(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];
    char      json[HTML_CAP];

    page_acc_init(&a);
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_NO_DATA && v.reasons == 0 && !v.have_last);
    CHECK(!v.soil_known && !v.light_known);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "No data yet"));
    CHECK(!has(html, "Readings are fresh"));
    CHECK(has(html, "Needs water") && has(html, "Unknown: not enough data"));
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    CHECK(has(json, "\"state\":\"no_data\"") && has(json, "\"age_s\":null"));
    CHECK(has(json, "\"soil\":null") && has(json, "\"light\":null"));
    CHECK(has(json, "\"needs_water\":\"unknown\"") && v.water == THIRST_UNKNOWN);
}

static void fresh_and_valid(void)
{
    page_acc  a;
    page_view v;
    gt_record newest = rec(NOW - 5000, 2, 914, GT_REC_LIGHT_BRIGHT);
    char      html[HTML_CAP];
    char      json[HTML_CAP];

    newest.light_bright_ms = 1234;
    page_acc_init(&a);
    add(&a, rec(NOW - 65000, 1, 900, 0));
    add(&a, newest);
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_OK && v.reasons == 0);
    CHECK(v.have_last && v.age_ms == 5000);
    CHECK(v.soil_known && v.soil_raw == 914 && v.soil_age_ms == 5000);
    CHECK(v.light_known && v.light_bright && v.light_bright_ms == 1234);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "Readings are fresh and valid"));
    CHECK(has(html, "Raw value 914") && has(html, "5 s ago"));
    CHECK(has(html, "Bright at the end") && has(html, "1234 ms bright"));
    CHECK(has(html, "No, this plant does not need water."));
    CHECK(!has(html, "Unknown: not enough data") && !has(html, "Yes, water this plant"));
    CHECK(!has(html, "has not been calibrated") && !has(html, "dry line")); /* status only, no numbers */
    CHECK(!has(html, "Degraded"));
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    CHECK(has(json, "\"state\":\"ok\"") && has(json, "\"reasons\":[]"));
    CHECK(has(json, "\"soil\":{\"raw\":914,\"age_s\":5}"));
    CHECK(has(json, "\"light\":{\"bright\":true,\"bright_ms\":1234,\"age_s\":5}"));
    CHECK(has(json, "\"needs_water\":\"no\"") && has(json, "\"dry_raw\":750,"));
    CHECK(has(json, "\"time_until_dry\":null"));
    CHECK(!has(html, "Time until dry")); /* the page no longer has that section (the JSON still has the field) */
}

static void stale_boundary(void)
{
    page_acc  a;
    page_view v;

    page_acc_init(&a);
    add(&a, rec(NOW - PAGE_STALE_MS, 1, 900, 0));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_OK); /* exactly 3 minutes is not yet stale */
    judge_ok(&a, NOW + 1, &v);
    CHECK(v.state == PAGE_DEGRADED && v.reasons == PAGE_R_STALE);
}

/* One flag on the newest record, one reason, DEGRADED. */
static void flag_case(uint16_t flag, unsigned reason)
{
    page_acc  a;
    page_view v;

    page_acc_init(&a);
    add(&a, rec(NOW - 1000, 1, 900, 0));
    add(&a, rec(NOW - 500, 2, 901, flag));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_DEGRADED);
    CHECK((v.reasons & reason) != 0);
    CHECK((v.reasons & ~reason) == 0);
}

static void newest_record_flags(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    flag_case(GT_REC_SOIL_STUCK, PAGE_R_SOIL_STUCK);
    flag_case(GT_REC_LIGHT_INVALID, PAGE_R_LIGHT_INVALID);
    flag_case(GT_REC_UNDERVOLT, PAGE_R_UNDERVOLT);
    flag_case(GT_REC_TIME_UNSET, PAGE_R_TIME_UNSET);
    flag_case(GT_REC_SOIL_INVALID, PAGE_R_SOIL_INVALID);

    /* A bright flag alone is just a reading. */
    page_acc_init(&a);
    add(&a, rec(NOW - 500, 1, 900, GT_REC_LIGHT_BRIGHT));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_OK);

    /* Invalid soil on the newest record: the page keeps the last valid value and says so. */
    page_acc_init(&a);
    add(&a, rec(NOW - 90000, 1, 880, 0));
    add(&a, rec(NOW - 30000, 2, 0, GT_REC_SOIL_INVALID));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_DEGRADED && (v.reasons & PAGE_R_SOIL_INVALID) != 0);
    CHECK((v.reasons & PAGE_R_NO_SOIL) == 0);
    CHECK(v.soil_known && v.soil_raw == 880 && v.soil_age_ms == 90000);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "Raw value 880") && has(html, "1 min ago"));
    CHECK(has(html, "The latest soil reading is invalid."));

    /* Nothing valid at all. */
    page_acc_init(&a);
    add(&a, rec(NOW - 30000, 1, 0, GT_REC_SOIL_INVALID));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_DEGRADED);
    CHECK((v.reasons & PAGE_R_NO_SOIL) != 0 && !v.soil_known);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "No valid soil reading in the last 24 hours."));
}

static void store_and_disk(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    page_acc_init(&a);
    add(&a, rec(NOW - 500, 1, 900, 0));

    judge(&a, NOW, EIO, true, GB, &v);
    CHECK(v.state == PAGE_DEGRADED && (v.reasons & PAGE_R_STORE_UNREADABLE) != 0);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "The reading history cannot be read.") && has(html, "Read error code 5"));

    page_acc_init(&a); /* unreadable and empty is a fault, not "no data" */
    judge(&a, NOW, EACCES, true, GB, &v);
    CHECK(v.state == PAGE_DEGRADED);

    page_acc_init(&a);
    add(&a, rec(NOW - 500, 1, 900, 0));
    judge(&a, NOW, 0, true, PAGE_LOW_DISK_BYTES, &v);
    CHECK(v.state == PAGE_OK);
    judge(&a, NOW, 0, true, PAGE_LOW_DISK_BYTES - 1, &v);
    CHECK(v.state == PAGE_DEGRADED && v.reasons == PAGE_R_LOW_DISK);
    judge(&a, NOW, 0, false, 0, &v); /* statvfs failed: say nothing about disk */
    CHECK(v.state == PAGE_OK && !v.disk_known);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(!has(html, "MB of disk free"));
}

static void clock_cases(void)
{
    page_acc  a;
    page_view v;

    page_acc_init(&a);
    add(&a, rec(NOW + PAGE_FUTURE_MS, 1, 900, 0));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_OK && v.age_ms == 0);
    page_acc_init(&a);
    add(&a, rec(NOW + PAGE_FUTURE_MS + 1, 1, 900, 0));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_DEGRADED && v.reasons == PAGE_R_CLOCK_AHEAD && v.age_ms == 0);
}

static void history_notes(void)
{
    page_acc  a;
    page_view v;
    gt_record bad;
    char      json[HTML_CAP];
    char      html[HTML_CAP];

    page_acc_init(&a);
    add(&a, rec(NOW - 9000, 1, 900, 0));
    add(&a, rec(NOW - 8000, 2, 900, GT_REC_UNDERVOLT));
    add(&a, rec(NOW - 7000, 5, 900, GT_REC_SOIL_STUCK | GT_REC_TIME_UNSET)); /* skips 3, 4 */
    add(&a, rec(NOW - 6000, 6, 0, GT_REC_SOIL_INVALID | GT_REC_LIGHT_INVALID));
    add(&a, rec(NOW - 3000, 10, 910, 0));                                    /* skips 7, 8, 9 */
    (void)memset(&bad, 0, sizeof bad);
    CHECK(page_acc_visit(&bad, GT_STORE_REC_CORRUPT, &a) == 0);
    judge_ok(&a, NOW, &v);

    CHECK(a.records == 5 && a.corrupt == 1);
    CHECK(a.gap_events == 2 && a.missing == 5);
    CHECK(a.soil_invalid == 1 && a.soil_stuck == 1 && a.light_invalid == 1);
    CHECK(a.undervolt == 1 && a.time_unset == 1);
    CHECK(a.prev_seq == 10 && a.last.seq == 10); /* a damaged record moves nothing */
    CHECK(v.state == PAGE_OK);                   /* history facts do not change the state */
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    CHECK(has(json, "\"gap_events\":2") && has(json, "\"missing\":5") && has(json, "\"corrupt\":1"));
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "2 gaps (5 readings missing), 1 damaged records"));

    /* The sequence number wrapping is not a gap. */
    page_acc_init(&a);
    add(&a, rec(NOW - 2000, UINT32_MAX, 900, 0));
    add(&a, rec(NOW - 1000, 0, 900, 0));
    CHECK(a.gap_events == 0);
}

/* A degraded page never carries the all-clear wording, whatever the reasons. */
static void never_ok_when_not(void)
{
    static const char* const IDS[] = {"store_unreadable", "stale",      "soil_invalid", "soil_stuck",
                                      "light_invalid",    "undervolt",  "time_unset",   "clock_ahead",
                                      "no_valid_soil",    "low_disk"};
    page_view v;
    char      html[HTML_CAP];
    char      json[HTML_CAP];

    (void)memset(&v, 0, sizeof v);
    v.state = PAGE_DEGRADED;
    v.reasons = 0x03FFu;
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(!has(html, "Readings are fresh") && has(html, "Degraded"));
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        char needle[40];

        CHECK(snprintf(needle, sizeof needle, "\"%s\"", IDS[i]) > 0);
        CHECK(has(json, needle)); /* every reason bit has an entry */
    }
    v.state = PAGE_NO_DATA;
    v.reasons = 0;
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(!has(html, "Readings are fresh"));
}

/* Brackets and quotes balance outside strings. */
static bool json_balanced(const char* s)
{
    int  depth = 0;
    bool in_str = false;

    for (; *s != '\0'; s++) {
        if (*s == '"') {
            in_str = !in_str;
        } else if (!in_str && (*s == '{' || *s == '[')) {
            depth++;
        } else if (!in_str && (*s == '}' || *s == ']')) {
            depth--;
            if (depth < 0) {
                return false;
            }
        }
    }
    return depth == 0 && !in_str;
}

/* Every too-small buffer is refused and left empty; nothing is written past it. */
static void buffer_limits(void)
{
    page_acc  a;
    page_view v;
    char      big[HTML_CAP];
    int       html_len;
    int       json_len;

    page_acc_init(&a);
    add(&a, rec(NOW - 5000, 1, 914, GT_REC_UNDERVOLT));
    judge(&a, NOW, EIO, true, 5ull * 1024 * 1024, &v);
    html_len = page_render_html(big, sizeof big, &v);
    CHECK(html_len > 0 && (size_t)html_len < sizeof big);
    json_len = page_render_json(big, sizeof big, &v);
    CHECK(json_len > 0 && json_balanced(big));
    CHECK(strlen(big) == (size_t)json_len && big[json_len - 1] == '\n' && big[0] == '{');

    for (int pass = 0; pass < 2; pass++) {
        int full = pass == 0 ? html_len : json_len;

        for (size_t cap = 0; cap <= (size_t)full + 1; cap++) {
            char buf[HTML_CAP + 8];
            int  n;

            (void)memset(buf, 0x5A, sizeof buf);
            n = pass == 0 ? page_render_html(buf, cap, &v) : page_render_json(buf, cap, &v);
            for (size_t i = cap; i < sizeof buf; i++) {
                CHECK(buf[i] == 0x5A); /* nothing past cap */
            }
            if (cap < (size_t)full + 1) {
                CHECK(n == -1);
                CHECK(cap == 0 || buf[0] == '\0');
            } else {
                CHECK(n == full && strlen(buf) == (size_t)full);
            }
        }
    }
}

static void extreme_values(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];
    char      json[HTML_CAP];

    page_acc_init(&a);
    add(&a, rec(0, 1, 65535, 0));
    judge_ok(&a, UINT64_MAX, &v); /* a record from 1970, the clock at the far end */
    CHECK(v.state == PAGE_DEGRADED && (v.reasons & PAGE_R_STALE) != 0);
    CHECK(page_render_html(html, sizeof html, &v) > 0 && has(html, "Raw value 65535"));
    CHECK(v.water == THIRST_UNKNOWN); /* 65535 is far above the line, but it is 53 years old */
    CHECK(page_render_json(json, sizeof json, &v) > 0 && json_balanced(json));

    page_acc_init(&a);
    add(&a, rec(UINT64_MAX, UINT32_MAX, 0, 0xFFFFu));
    judge_ok(&a, 0, &v);
    CHECK(v.state == PAGE_DEGRADED && v.age_ms == 0);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(page_render_json(json, sizeof json, &v) > 0 && json_balanced(json));
}

/* One fresh record, soil `soil` and `flags`, 5 s old, on a readable store with plenty of disk. */
static void one_record(page_acc* a, page_view* v, uint16_t soil, uint16_t flags)
{
    page_acc_init(a);
    add(a, rec(NOW - 5000, 1, soil, flags));
    judge_ok(a, NOW, v);
}

/* Both bodies say the same thing about needs-water, and say no more than the view allows. */
static void water_is(const page_view* v, thirst_state want, const char* word)
{
    char html[HTML_CAP];
    char json[HTML_CAP];
    char needle[48];

    CHECK(v->water == want);
    CHECK(page_render_html(html, sizeof html, v) > 0);
    CHECK(page_render_json(json, sizeof json, v) > 0 && json_balanced(json));
    CHECK(snprintf(needle, sizeof needle, "\"needs_water\":\"%s\"", word) > 0);
    CHECK(has(json, needle));
    CHECK(has(html, "Yes, water this plant") == (want == THIRST_NEEDS_WATER));
    CHECK(has(html, "No, this plant does not need water.") == (want == THIRST_OK));
    CHECK(has(html, "Unknown: not enough data") == (want == THIRST_UNKNOWN));
    CHECK(has(html, "class=\"state needs\"") == (want == THIRST_NEEDS_WATER));
}

static void needs_water_states(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];
    char      json[HTML_CAP];

    /* At or below the dry line is dry; one above is not. */
    one_record(&a, &v, 400, 0);
    CHECK(v.state == PAGE_OK && v.water_level_raw == 400 && v.water_dry_raw == THIRST_DRY_DEFAULT);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "Yes, water this plant.") && !has(html, "dry line"));
    CHECK(!has(html, "has not been calibrated"));
    one_record(&a, &v, THIRST_DRY_DEFAULT, 0);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    one_record(&a, &v, THIRST_DRY_DEFAULT + 1u, 0);
    water_is(&v, THIRST_OK, "no");

    /* Hysteresis through the page: stays dry until the reading is 48 above the line. */
    page_acc_init(&a);
    add(&a, rec(NOW - 40000, 1, 700, 0));
    add(&a, rec(NOW - 30000, 2, 797, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    add(&a, rec(NOW - 20000, 3, 798, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_OK, "no");
    add(&a, rec(NOW - 10000, 4, 751, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_OK, "no");
    add(&a, rec(NOW - 5000, 5, 750, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");

    /* A threshold set by the caller. */
    page_acc_init(&a);
    page_acc_set_dry(&a, 700);
    add(&a, rec(NOW - 5000, 1, 650, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    CHECK(v.water_dry_raw == 700);
    CHECK(page_render_json(json, sizeof json, &v) > 0 && has(json, "\"dry_raw\":700,"));
    CHECK(page_render_html(html, sizeof html, &v) > 0 && !has(html, "dry line"));
    page_acc_init(&a);
    page_acc_set_dry(&a, 0);
    add(&a, rec(NOW - 5000, 1, 1, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_OK, "no");
    page_acc_init(&a);
    page_acc_set_dry(&a, 65535);
    add(&a, rec(NOW - 5000, 1, 65535, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
}

/* Not enough to say: never "no", and never "yes" from a reading the page does not trust. */
static void needs_water_unknown(void)
{
    page_acc  a;
    page_view v;

    page_acc_init(&a);
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_UNKNOWN, "unknown");

    /* Only unusable records. */
    page_acc_init(&a);
    add(&a, rec(NOW - 3000, 1, 0, GT_REC_SOIL_INVALID));
    add(&a, rec(NOW - 2000, 2, 400, GT_REC_SOIL_STUCK));
    add(&a, rec(NOW - 1000, 3, 400, GT_REC_TIME_UNSET));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_UNKNOWN, "unknown");

    /* The newest record is bad: an older good one does not stand in, dry or wet. */
    for (int dry = 0; dry < 2; dry++) {
        uint16_t soil = dry ? 400 : 900;

        page_acc_init(&a);
        add(&a, rec(NOW - 10000, 1, soil, 0));
        add(&a, rec(NOW - 5000, 2, 0, GT_REC_SOIL_INVALID));
        judge_ok(&a, NOW, &v);
        CHECK(v.state == PAGE_DEGRADED);
        water_is(&v, THIRST_UNKNOWN, "unknown");

        page_acc_init(&a);
        add(&a, rec(NOW - 10000, 1, soil, 0));
        add(&a, rec(NOW - 5000, 2, soil, GT_REC_SOIL_STUCK));
        judge_ok(&a, NOW, &v);
        water_is(&v, THIRST_UNKNOWN, "unknown");

        page_acc_init(&a);
        add(&a, rec(NOW - 10000, 1, soil, 0));
        add(&a, rec(NOW - 5000, 2, soil, GT_REC_TIME_UNSET));
        judge_ok(&a, NOW, &v);
        water_is(&v, THIRST_UNKNOWN, "unknown");
    }

    /* Stale, dry or wet: the plant may have been watered since. */
    for (int dry = 0; dry < 2; dry++) {
        page_acc_init(&a);
        add(&a, rec(NOW - PAGE_STALE_MS, 1, dry ? 400 : 900, 0));
        judge_ok(&a, NOW, &v);
        water_is(&v, dry ? THIRST_NEEDS_WATER : THIRST_OK, dry ? "yes" : "no"); /* exactly 3 min */
        judge_ok(&a, NOW + 1, &v);
        CHECK(v.state == PAGE_DEGRADED);
        water_is(&v, THIRST_UNKNOWN, "unknown");
    }

    /* Dated in the future by more than the page allows. */
    page_acc_init(&a);
    add(&a, rec(NOW + PAGE_FUTURE_MS, 1, 400, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    page_acc_init(&a);
    add(&a, rec(NOW + PAGE_FUTURE_MS + 1u, 1, 400, 0));
    judge_ok(&a, NOW, &v);
    CHECK(v.state == PAGE_DEGRADED && (v.reasons & PAGE_R_CLOCK_AHEAD) != 0);
    water_is(&v, THIRST_UNKNOWN, "unknown");
}

/* A degraded page never says "no", but an alert still shows. */
static void needs_water_on_a_degraded_page(void)
{
    page_acc  a;
    page_view v;

    /* Wet and fresh, but the page is degraded for another reason: unknown. */
    one_record(&a, &v, 900, GT_REC_UNDERVOLT);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");
    one_record(&a, &v, 900, GT_REC_LIGHT_INVALID);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");
    one_record(&a, &v, 900, 0);
    judge(&a, NOW, 0, true, 5ull * 1024 * 1024, &v); /* low disk */
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");
    judge(&a, NOW, EIO, true, GB, &v); /* store unreadable */
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");

    /* Dry and fresh on a degraded page: the alert still shows. */
    one_record(&a, &v, 400, GT_REC_UNDERVOLT);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    one_record(&a, &v, 400, 0);
    judge(&a, NOW, 0, true, 5ull * 1024 * 1024, &v);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
}

/* How many times `needle` is in `text`. */
static size_t count(const char* text, const char* needle)
{
    size_t n = 0;
    size_t len = strlen(needle);

    for (const char* p = strstr(text, needle); p != NULL; p = strstr(p + len, needle)) {
        n++;
    }
    return n;
}

/* The page's colored box as one word, or "" when there is not exactly one box. */
static const char* box_word(const char* html)
{
    static const char* const WORDS[] = {"green", "yellow", "red", "black", "unknown"};
    const char*              found = "";
    size_t                   boxes = 0;

    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        char needle[40];

        CHECK(snprintf(needle, sizeof needle, "class=\"box %s\"", WORDS[i]) > 0);
        if (has(html, needle)) {
            found = WORDS[i];
            boxes++;
        }
    }
    return boxes == 1 ? found : "";
}

/* The page shows the box `want` with its own words, and none of the other boxes' words. */
static void box_is(const page_view* v, const char* want)
{
    static const struct {
        const char* word;
        const char* label;
    } LABELS[] = {
        {"green", "No water needed"},
        {"yellow", "Water soon"},
        {"red", "Water now"},
        {"black", "Extremely dry"},
        {"unknown", "Cannot tell whether this plant needs water"},
    };
    char html[HTML_CAP];

    CHECK(page_render_html(html, sizeof html, v) > 0);
    CHECK(strcmp(box_word(html), want) == 0);
    for (size_t i = 0; i < sizeof LABELS / sizeof LABELS[0]; i++) {
        CHECK(has(html, LABELS[i].label) == (strcmp(LABELS[i].word, want) == 0));
    }
    CHECK(has(html, "class=\"warn\"") == (strcmp(want, "unknown") == 0));
}

/* Green only when the needs-water check says OK; the other colors by how dry the reading is. */
static void box_colors(void)
{
    static const struct {
        uint16_t    soil;
        const char* want;
    } T[] = {
        {65535, "green"}, {800, "green"}, {799, "green"}, {751, "green"},
        {750, "yellow"},  {700, "yellow"}, {651, "yellow"},
        {650, "red"},     {400, "red"},    {300, "red"},
        {299, "black"},   {100, "black"},  {1, "black"},
    };
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        one_record(&a, &v, T[i].soil, 0);
        CHECK(v.state == PAGE_OK);
        box_is(&v, T[i].want);
    }
    one_record(&a, &v, 299, 0);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "Check that the sensor is in the soil"));

    /* Above the line but inside the margin is still not green (the needs-water hysteresis). */
    page_acc_init(&a);
    add(&a, rec(NOW - 40000, 1, 700, 0));
    add(&a, rec(NOW - 30000, 2, 797, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "yellow");
    add(&a, rec(NOW - 20000, 3, 798, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "green");
    add(&a, rec(NOW - 10000, 4, 751, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "green");
    add(&a, rec(NOW - 5000, 5, 750, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "yellow");

    /* A dry line set by the caller moves the green line; the red and black lines stay. */
    page_acc_init(&a);
    page_acc_set_dry(&a, 800);
    add(&a, rec(NOW - 5000, 1, 700, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "yellow");
    page_acc_init(&a);
    page_acc_set_dry(&a, 800);
    add(&a, rec(NOW - 5000, 1, 860, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "green");
    page_acc_init(&a);
    page_acc_set_dry(&a, 300);
    add(&a, rec(NOW - 5000, 1, 400, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "green"); /* the check says OK, so the box does too */
    page_acc_init(&a);
    page_acc_set_dry(&a, 300);
    add(&a, rec(NOW - 5000, 1, 299, 0));
    judge_ok(&a, NOW, &v);
    box_is(&v, "black");
}

/* Whenever the needs-water state is unknown the page says so: a grey box and a warning, never green. */
static void unknown_box(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    page_acc_init(&a);
    judge_ok(&a, NOW, &v); /* nothing recorded yet */
    box_is(&v, "unknown");
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "Do not assume it is fine"));

    for (int dry = 0; dry < 2; dry++) {
        uint16_t soil = dry ? 400 : 900;

        one_record(&a, &v, soil, 0); /* too old to trust, dry or wet */
        judge_ok(&a, NOW + PAGE_STALE_MS, &v);
        CHECK(v.state == PAGE_DEGRADED);
        box_is(&v, "unknown");

        page_acc_init(&a); /* the newest record is bad: an older good one does not stand in */
        add(&a, rec(NOW - 10000, 1, soil, 0));
        add(&a, rec(NOW - 5000, 2, 0, GT_REC_SOIL_INVALID));
        judge_ok(&a, NOW, &v);
        box_is(&v, "unknown");

        page_acc_init(&a);
        add(&a, rec(NOW - 10000, 1, soil, 0));
        add(&a, rec(NOW - 5000, 2, soil, GT_REC_SOIL_STUCK));
        judge_ok(&a, NOW, &v);
        box_is(&v, "unknown");
    }

    /* Wet and fresh, but the page is degraded for another reason: unknown. */
    one_record(&a, &v, 900, GT_REC_UNDERVOLT);
    box_is(&v, "unknown");
    one_record(&a, &v, 900, 0);
    judge(&a, NOW, EIO, true, GB, &v);
    box_is(&v, "unknown");
    judge(&a, NOW, 0, true, 5ull * 1024 * 1024, &v);
    box_is(&v, "unknown");

    /* Dry and fresh on a degraded page: the alert still shows, in its own color. */
    one_record(&a, &v, 700, GT_REC_UNDERVOLT);
    box_is(&v, "yellow");
}

/* The range table lists every range and marks the row of the newest valid soil value. */
static void range_table(void)
{
    static const struct {
        uint16_t    soil;
        const char* row;
    } T[] = {
        {65535, "Wet"},  {900, "Wet"},    {800, "Wet"},       {799, "Moist"},    {775, "Moist"},
        {751, "Moist"},  {750, "Dry"},    {700, "Dry"},       {651, "Dry"},      {650, "Very dry"},
        {400, "Very dry"}, {300, "Very dry"}, {299, "Open air"}, {200, "Open air"}, {1, "Open air"},
    };
    static const char* const RANGES[] = {"800 and up", "751 to 799", "651 to 750", "300 to 650",
                                         "below 300"};
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        char needle[64];

        one_record(&a, &v, T[i].soil, 0);
        CHECK(page_render_html(html, sizeof html, &v) > 0);
        CHECK(snprintf(needle, sizeof needle, "<tr class=\"here\"><th scope=\"row\">%s</th>", T[i].row) > 0);
        CHECK(has(html, needle));
        CHECK(count(html, "class=\"here\"") == 1);
        CHECK(snprintf(needle, sizeof needle, "Your reading: %u", (unsigned)T[i].soil) > 0);
        CHECK(has(html, needle));
        for (size_t r = 0; r < sizeof RANGES / sizeof RANGES[0]; r++) {
            CHECK(has(html, RANGES[r]));
        }
    }

    /* No soil value at all: the table is there, nothing in it is marked. */
    page_acc_init(&a);
    judge_ok(&a, NOW, &v);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(count(html, "class=\"here\"") == 0 && has(html, "<table") && has(html, "800 and up"));
    CHECK(has(html, "a provisional best guess for this plant from two readings, not a measured calibration"));
    CHECK(!has(html, "Your reading:"));

    /* The newest soil value is invalid: the last valid one is still the number marked, with its age. */
    page_acc_init(&a);
    add(&a, rec(NOW - 90000, 1, 880, 0));
    add(&a, rec(NOW - 30000, 2, 0, GT_REC_SOIL_INVALID));
    judge_ok(&a, NOW, &v);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "<tr class=\"here\"><th scope=\"row\">Wet</th>"));
    CHECK(has(html, "Your reading: 880 (higher is wetter), read 1 min ago."));
    CHECK(strcmp(box_word(html), "unknown") == 0); /* ... while the box does not trust it */
}

static void plant_picture(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    one_record(&a, &v, 900, 0);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "<svg") && has(html, "role=\"img\"") && has(html, "aria-label=\"Potted plant\""));
    CHECK(strstr(html, "<svg") < strstr(html, "class=\"box")); /* the picture, then the box beside it */

    page_acc_init(&a);
    judge_ok(&a, NOW, &v);
    CHECK(page_render_html(html, sizeof html, &v) > 0 && has(html, "<svg"));
}

/* The sun icon shows exactly one of three looks: "on" (bright), "off" (dim, dark) or "unk" (dim and
 * dashed, the page cannot vouch for the reading), with its words; and the page has two pictures. */
static void light_is(const page_view* v, const char* look, const char* label)
{
    static const char* const LOOKS[] = {"on", "off", "unk"};
    char                     html[HTML_CAP];

    CHECK(page_render_html(html, sizeof html, v) > 0);
    for (size_t i = 0; i < sizeof LOOKS / sizeof LOOKS[0]; i++) {
        char needle[32];

        CHECK(snprintf(needle, sizeof needle, "class=\"light %s\"", LOOKS[i]) > 0);
        CHECK(has(html, needle) == (strcmp(LOOKS[i], look) == 0));
    }
    CHECK(has(html, label));
    CHECK(count(html, "aria-label=\"Sun\"") == 1); /* the sun is drawn once (the page has other pictures) */
}

static void light_icon(void)
{
    page_acc  a;
    page_view v;

    one_record(&a, &v, 900, GT_REC_LIGHT_BRIGHT);
    light_is(&v, "on", "Light: Bright");
    one_record(&a, &v, 900, 0);
    light_is(&v, "off", "Light: Dark");

    page_acc_init(&a); /* nothing recorded */
    judge_ok(&a, NOW, &v);
    light_is(&v, "unk", "Light: Unknown");

    page_acc_init(&a); /* the newest light value is invalid: an older bright one does not stand in */
    add(&a, rec(NOW - 10000, 1, 900, GT_REC_LIGHT_BRIGHT));
    add(&a, rec(NOW - 5000, 2, 900, GT_REC_LIGHT_INVALID));
    judge_ok(&a, NOW, &v);
    light_is(&v, "unk", "Light: Unknown");

    page_acc_init(&a); /* too old to say what the light is now, bright or dark */
    add(&a, rec(NOW - PAGE_STALE_MS, 1, 900, GT_REC_LIGHT_BRIGHT));
    judge_ok(&a, NOW, &v);
    light_is(&v, "on", "Light: Bright"); /* exactly 3 minutes is not yet stale */
    judge_ok(&a, NOW + 1, &v);
    light_is(&v, "unk", "Light: Unknown");
    page_acc_init(&a);
    add(&a, rec(NOW - PAGE_STALE_MS - 1, 1, 900, 0));
    judge_ok(&a, NOW, &v);
    light_is(&v, "unk", "Light: Unknown"); /* a dark reading goes unknown too, not just bright */

    /* The icon follows the light only: it does not move the water box. */
    one_record(&a, &v, 700, GT_REC_LIGHT_BRIGHT);
    box_is(&v, "yellow");
    light_is(&v, "on", "Light: Bright");
    one_record(&a, &v, 700, 0);
    box_is(&v, "yellow");
    light_is(&v, "off", "Light: Dark");
}

/* The one bucket that holds anything, found by searching: -1 when none does. */
static int bucket_of(const page_acc* a)
{
    int found = -1;

    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        if (a->hist[i].soil_n != 0 || a->hist[i].light_n != 0) {
            CHECK(found == -1); /* one record, one bucket */
            found = (int)i;
        }
    }
    return found;
}

/* One good record at time ts in a fresh accumulator whose window ends at now. */
static int lands_in(uint64_t now, uint64_t ts)
{
    page_acc a;

    page_acc_init(&a);
    page_acc_set_now(&a, now);
    add(&a, rec(ts, 1, 900, 0));
    return bucket_of(&a);
}

static void history_buckets(void)
{
    const uint64_t start = NOW - PAGE_WINDOW_MS;
    page_acc       a;
    page_view      v;
    page_bucket    empty;

    CHECK(PAGE_HIST_BUCKET_MS == 30ull * 60ull * 1000ull);

    /* Off unless asked for: the old callers see no history. */
    page_acc_init(&a);
    add(&a, rec(NOW - 1000, 1, 900, 0));
    CHECK(!a.hist_on && bucket_of(&a) == -1);

    /* Where a record lands, at every edge of the window and of a bucket. */
    CHECK(lands_in(NOW, start) == 0);
    CHECK(lands_in(NOW, start - 1) == -1); /* older than the window */
    CHECK(lands_in(NOW, start + PAGE_HIST_BUCKET_MS - 1) == 0);
    CHECK(lands_in(NOW, start + PAGE_HIST_BUCKET_MS) == 1);
    CHECK(lands_in(NOW, NOW - 1000) == (int)PAGE_HIST_N - 1);
    CHECK(lands_in(NOW, NOW) == (int)PAGE_HIST_N - 1); /* the end itself is in the last bucket */
    CHECK(lands_in(NOW, NOW + 1) == -1);               /* dated after now */
    CHECK(lands_in(1000, 0) == 0);                     /* a clock near 1970: the window starts at 0 */

    /* Several samples in one bucket: count, min, max, rounded mean. */
    page_acc_init(&a);
    page_acc_set_now(&a, NOW);
    add(&a, rec(NOW - 3000, 1, 600, 0));
    add(&a, rec(NOW - 2000, 2, 800, GT_REC_LIGHT_BRIGHT));
    add(&a, rec(NOW - 1000, 3, 701, 0));
    CHECK(a.hist[PAGE_HIST_N - 1].soil_n == 3 && a.hist[PAGE_HIST_N - 1].soil_sum == 2101);
    CHECK(a.hist[PAGE_HIST_N - 1].soil_min == 600 && a.hist[PAGE_HIST_N - 1].soil_max == 800);
    CHECK(page_bucket_soil_avg(&a.hist[PAGE_HIST_N - 1]) == 700); /* 700.33 */
    CHECK(a.hist[PAGE_HIST_N - 1].light_n == 3 && a.hist[PAGE_HIST_N - 1].bright_n == 1);

    /* The flags: bad soil leaves the light in, bad light leaves the soil in, an unset clock
     * leaves both out, and a damaged record moves nothing. */
    page_acc_init(&a);
    page_acc_set_now(&a, NOW);
    add(&a, rec(NOW - 5000, 1, 0, GT_REC_SOIL_INVALID | GT_REC_LIGHT_BRIGHT));
    add(&a, rec(NOW - 4000, 2, 500, GT_REC_SOIL_STUCK));
    add(&a, rec(NOW - 3000, 3, 600, GT_REC_LIGHT_INVALID));
    add(&a, rec(NOW - 2000, 4, 900, GT_REC_TIME_UNSET | GT_REC_LIGHT_BRIGHT));
    CHECK(page_acc_visit(&(gt_record){.ts_unix_ms = NOW - 1000, .soil_raw = 1}, GT_STORE_REC_CORRUPT,
                         &a) == 0);
    CHECK(a.hist[PAGE_HIST_N - 1].soil_n == 1 && a.hist[PAGE_HIST_N - 1].soil_sum == 600);
    CHECK(a.hist[PAGE_HIST_N - 1].light_n == 2 && a.hist[PAGE_HIST_N - 1].bright_n == 1);
    CHECK(a.hist[PAGE_HIST_N - 1].soil_bad == 2 && a.hist[PAGE_HIST_N - 1].light_bad == 1);

    /* The largest values do not overflow: a full day of 65535 in one bucket. */
    page_acc_init(&a);
    page_acc_set_now(&a, NOW);
    for (uint32_t i = 0; i < 1440; i++) {
        add(&a, rec(NOW - 1000, i, 65535, 0));
    }
    CHECK(a.hist[PAGE_HIST_N - 1].soil_n == 1440 && page_bucket_soil_avg(&a.hist[PAGE_HIST_N - 1]) == 65535);

    /* An empty bucket has no mean. */
    (void)memset(&empty, 0, sizeof empty);
    CHECK(page_bucket_soil_avg(&empty) == 0);

    /* The view carries a copy. */
    page_acc_init(&a);
    page_acc_set_now(&a, NOW);
    add(&a, rec(start + PAGE_HIST_BUCKET_MS * 5u, 1, 444, 0));
    judge_ok(&a, NOW, &v);
    CHECK(v.hist_on && v.hist_start_ms == start);
    CHECK(v.hist[5].soil_n == 1 && v.hist[5].soil_min == 444 && v.hist[4].soil_n == 0);
    page_acc_init(&a);
    judge_ok(&a, NOW, &v);
    CHECK(!v.hist_on && v.hist[PAGE_HIST_N - 1].soil_n == 0);
}

/* Every field at its largest, every reason set, the banner on: the longest page there can be. */
static void worst_case_view(page_view* v)
{
    (void)memset(v, 0, sizeof *v);
    v->state = PAGE_DEGRADED;
    v->source = PAGE_SRC_MIXED;
    v->reasons = 0x03FFu;
    v->store_errno = 2147483647;
    v->records = v->corrupt = v->synthetic = UINT64_MAX;
    v->soil_invalid = v->soil_stuck = v->light_invalid = UINT64_MAX;
    v->undervolt = v->time_unset = v->gap_events = v->missing = UINT64_MAX;
    v->have_last = true;
    v->age_ms = UINT64_MAX;
    v->soil_known = true;
    v->soil_raw = 65535;
    v->soil_age_ms = UINT64_MAX;
    v->light_known = true;
    v->light_bright = true;
    v->light_bright_ms = 65535;
    v->light_age_ms = UINT64_MAX;
    v->water = THIRST_NEEDS_WATER;
    v->water_level_raw = 65535;
    v->water_dry_raw = 65535;
    v->disk_known = true;
    v->disk_free_bytes = UINT64_MAX;
    /* A full history at the largest values, every bucket also with bad readings, and the soil
     * swinging so every chart point and as many updates as can be listed are drawn. */
    v->hist_on = true;
    v->hist_start_ms = UINT64_MAX - PAGE_WINDOW_MS;
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        page_bucket* b = &v->hist[i];

        b->soil_n = UINT32_MAX;
        b->soil_min = i % 2 == 0 ? 0 : 65535;
        b->soil_max = 65535;
        b->soil_sum = (uint64_t)b->soil_min * UINT32_MAX;
        b->light_n = UINT32_MAX;
        b->bright_n = UINT32_MAX;
        b->soil_bad = UINT32_MAX;
        b->light_bad = UINT32_MAX;
    }
}

/* A fresh, OK view (one record, soil 700) whose history is set by hand. */
static void hist_view(page_view* v)
{
    page_acc a;

    one_record(&a, v, 900, 0);
    v->hist_on = true;
    v->hist_start_ms = NOW - PAGE_WINDOW_MS;
    (void)memset(v->hist, 0, sizeof v->hist);
}

/* Bucket i holds one good reading of soil `soil` (light dark). */
static void put_soil(page_view* v, size_t i, uint16_t soil)
{
    page_bucket* b = &v->hist[i];

    b->soil_n = 1;
    b->soil_sum = soil;
    b->soil_min = soil;
    b->soil_max = soil;
    b->light_n = 1;
    b->bright_n = 0;
}

/* Recent updates: worked out from the buckets, newest first, at most the cap. */
static void recent_updates(void)
{
    page_view  v;
    page_event ev[PAGE_EVENTS_MAX];
    size_t     n;

    CHECK(PAGE_WATERED_RISE == 100u && PAGE_EVENTS_MAX == 5u);

    /* No history, or a steady one: nothing to say. */
    one_record(&(page_acc){0}, &v, 900, 0);
    CHECK(!v.hist_on && page_events(&v, ev, PAGE_EVENTS_MAX) == 0);
    hist_view(&v);
    CHECK(page_events(&v, ev, PAGE_EVENTS_MAX) == 0);
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        put_soil(&v, i, 900);
    }
    CHECK(page_events(&v, ev, PAGE_EVENTS_MAX) == 0);

    /* Watered: a rise of at least 100 from the last soil value within an hour. */
    hist_view(&v);
    put_soil(&v, 10, 400);
    put_soil(&v, 11, 500);
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 1 && ev[0].kind == PAGE_EV_WATERED && ev[0].bucket == 11);
    CHECK(ev[0].from_raw == 400 && ev[0].to_raw == 500);
    put_soil(&v, 11, 499); /* 99 is not enough */
    CHECK(page_events(&v, ev, PAGE_EVENTS_MAX) == 0);

    /* One empty half hour between: still watered, and the gap is reported after it. */
    hist_view(&v);
    put_soil(&v, 10, 400);
    put_soil(&v, 12, 520);
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 2 && ev[0].kind == PAGE_EV_WATERED && ev[0].bucket == 12);
    CHECK(ev[1].kind == PAGE_EV_GAP && ev[1].bucket == 12 && ev[1].count == 1);
    /* Two empty: more than an hour, so a rise is not called watering. */
    hist_view(&v);
    put_soil(&v, 10, 400);
    put_soil(&v, 13, 520);
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 1 && ev[0].kind == PAGE_EV_GAP && ev[0].count == 2);

    /* Went dry: from above the threshold to at or below it (750 is dry), even across a gap. */
    hist_view(&v);
    CHECK(v.water_dry_raw == 750);
    put_soil(&v, 20, 760);
    put_soil(&v, 21, 750);
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 1 && ev[0].kind == PAGE_EV_WENT_DRY && ev[0].bucket == 21);
    CHECK(ev[0].from_raw == 760 && ev[0].to_raw == 750);
    put_soil(&v, 21, 751);
    CHECK(page_events(&v, ev, PAGE_EVENTS_MAX) == 0);
    hist_view(&v);
    put_soil(&v, 20, 760);
    put_soil(&v, 30, 740);
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 2 && ev[0].kind == PAGE_EV_WENT_DRY && ev[1].kind == PAGE_EV_GAP && ev[1].count == 9);
    v.water_dry_raw = 730; /* the line the daemon was given */
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 1 && ev[0].kind == PAGE_EV_GAP);

    /* Bad readings, by sensor, with their count. */
    hist_view(&v);
    put_soil(&v, 40, 900);
    v.hist[40].soil_bad = 3;
    v.hist[40].light_bad = 2;
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 2 && ev[0].kind == PAGE_EV_SOIL_BAD && ev[0].count == 3);
    CHECK(ev[1].kind == PAGE_EV_LIGHT_BAD && ev[1].count == 2);
    /* A bucket with only bad readings still counts as readings for the gaps. */
    hist_view(&v);
    v.hist[5].soil_bad = 1;
    v.hist[5].light_bad = 1;
    put_soil(&v, 6, 900);
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == 2 && ev[0].bucket == 5 && ev[1].bucket == 5);

    /* More than fit: the newest ones, newest first, and never past the cap. */
    hist_view(&v);
    for (size_t i = 0; i < PAGE_HIST_N; i += 2) {
        put_soil(&v, i, 900);
    }
    n = page_events(&v, ev, PAGE_EVENTS_MAX);
    CHECK(n == PAGE_EVENTS_MAX && ev[0].bucket == PAGE_HIST_N - 2);
    for (size_t i = 1; i < n; i++) {
        CHECK(ev[i].kind == PAGE_EV_GAP && ev[i].bucket < ev[i - 1].bucket);
    }
    CHECK(page_events(&v, ev, 2) == 2 && page_events(&v, ev, 0) == 0);

    /* On the page: each kind in words, with when; and the empty cases. */
    {
        char html[HTML_CAP];

        hist_view(&v);
        put_soil(&v, 44, 760);
        put_soil(&v, 45, 740);
        put_soil(&v, 46, 940);
        v.hist[47].soil_bad = 4;
        put_soil(&v, 30, 900);
        CHECK(page_render_html(html, sizeof html, &v) > 0);
        CHECK(has(html, "<h2>Recent updates</h2>"));
        CHECK(has(html, "Watered</b> soil rose from 740 to 940"));
        CHECK(has(html, "Went dry</b> soil fell from 760 to 740"));
        CHECK(has(html, "Soil sensor</b> 4 bad readings"));
        CHECK(has(html, "No readings</b> for at least 6 h 30 min"));
        CHECK(has(html, "in the last 30 min")); /* bucket 47 */
        CHECK(has(html, "1 h 30 min to 1 h ago")); /* bucket 45 */
        CHECK(strstr(html, "Soil sensor</b>") < strstr(html, "Watered</b>")); /* newest first */
        hist_view(&v);
        put_soil(&v, 47, 900);
        CHECK(page_render_html(html, sizeof html, &v) > 0 && has(html, "Nothing new in the last 24 hours."));
        one_record(&(page_acc){0}, &v, 900, 0);
        CHECK(page_render_html(html, sizeof html, &v) > 0 && has(html, "No readings in the last 24 hours"));
    }
}

/* The d="..." of the first path with class `cls`, copied into out ("" if there is none). */
static void path_d(const char* html, const char* cls, char* out, size_t cap)
{
    char        needle[48];
    const char* p;
    const char* end;

    out[0] = '\0';
    CHECK(snprintf(needle, sizeof needle, "<path class=\"%s\" d=\"", cls) > 0);
    p = strstr(html, needle);
    if (p == NULL) {
        return;
    }
    p += strlen(needle);
    end = strchr(p, '"');
    CHECK(end != NULL && (size_t)(end - p) < cap);
    (void)memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
}

/* The soil graph: one line through the half-hour means, broken where there is no reading, with the
 * Dry and Very dry bands behind it, a dot on the newest point and a hover note per point. */
static void soil_chart(void)
{
    page_view v;
    char      html[HTML_CAP];
    char      d[2048];

    hist_view(&v);
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        put_soil(&v, i, (uint16_t)(400u + 10u * i));
    }
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "aria-label=\"Soil moisture, last 24 hours\""));
    CHECK(has(html, "class=\"bd1\"") && has(html, "class=\"bd2\"") && has(html, "class=\"bd3\""));
    CHECK(count(html, "<title>Soil ") == PAGE_HIST_N);
    CHECK(count(html, "class=\"now\"") == 1);
    CHECK(has(html, "24 h ago") && has(html, ">now<"));
    CHECK(has(html, "Low <b>400</b>") && has(html, "High <b>870</b>"));
    CHECK(has(html, "<title>Soil 24 h to 23 h 30 min ago: average 400, low 400, high 400</title>"));
    path_d(html, "ln", d, sizeof d);
    CHECK(count(d, "M") == 1 && count(d, "L") == PAGE_HIST_N - 1);
    path_d(html, "ar", d, sizeof d);
    CHECK(count(d, "M") == 1 && count(d, "Z") == 1);

    /* A hole breaks the line into two, and a point alone is a dot, not an invisible line. */
    hist_view(&v);
    put_soil(&v, 3, 600);
    put_soil(&v, 10, 600);
    put_soil(&v, 11, 610);
    put_soil(&v, 12, 620);
    put_soil(&v, 30, 600);
    put_soil(&v, 31, 600);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    path_d(html, "ln", d, sizeof d);
    CHECK(count(d, "M") == 2 && count(d, "L") == 3);
    CHECK(count(html, "class=\"pt\"") == 1);
    CHECK(count(html, "<title>Soil ") == 6);

    /* Extremes stay inside the plot: only digits, spaces and the path letters, no minus. */
    hist_view(&v);
    put_soil(&v, 0, 0);
    put_soil(&v, 1, 65535);
    put_soil(&v, 47, 65535);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    path_d(html, "ln", d, sizeof d);
    CHECK(d[0] == 'M' && strspn(d, "0123456789 MLZ") == strlen(d));
    path_d(html, "ar", d, sizeof d);
    CHECK(d[0] == 'M' && strspn(d, "0123456789 MLZ") == strlen(d));

    /* No soil in the window: words, not an empty graph. */
    hist_view(&v);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "No soil readings in the last 24 hours yet.") && !has(html, "class=\"ln\""));
}

/* The light strip: one cell per half hour, shaded by the share of bright readings (5 steps), a
 * dashed cell where there was no reading, and the total bright time. */
static void light_chart(void)
{
    static const struct {
        uint32_t bright, n;
        unsigned step;
    } STEPS[] = {{0, 10, 0}, {1, 10, 1}, {5, 10, 2}, {9, 10, 3}, {10, 10, 4},
                 {1, UINT32_MAX, 1}, {UINT32_MAX - 1u, UINT32_MAX, 3}, {UINT32_MAX, UINT32_MAX, 4}};
    page_view v;
    char      html[HTML_CAP];

    hist_view(&v);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "No light readings in the last 24 hours yet.") && !has(html, "class=\"ls\""));

    for (size_t s = 0; s < sizeof STEPS / sizeof STEPS[0]; s++) {
        char want[24];

        hist_view(&v);
        v.hist[20].light_n = STEPS[s].n;
        v.hist[20].bright_n = STEPS[s].bright;
        CHECK(page_render_html(html, sizeof html, &v) > 0);
        CHECK(snprintf(want, sizeof want, "class=\"b%u\"", STEPS[s].step) > 0);
        CHECK(count(html, want) == 1);
        CHECK(count(html, "class=\"none\"") == PAGE_HIST_N - 1);
    }

    hist_view(&v);
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        v.hist[i].light_n = 30;
        v.hist[i].bright_n = i < 3 ? 30 : 0;
    }
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "aria-label=\"Light, last 24 hours\""));
    CHECK(count(html, "<title>Light ") == PAGE_HIST_N);
    CHECK(count(html, "class=\"b4\"") == 3 && count(html, "class=\"b0\"") == PAGE_HIST_N - 3);
    CHECK(has(html, "Bright in 90 of 1440 readings, about 1 h 30 min"));
    CHECK(has(html, "<title>Light in the last 30 min: bright in 0 of 30 readings</title>"));
    CHECK(has(html, "<title>Light 24 h to 23 h 30 min ago: bright in 30 of 30 readings</title>"));
}

/* The top: the live dot and the banner's three tiles and strip. */
static void dashboard_top(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    hist_view(&v);
    put_soil(&v, 40, 520);
    put_soil(&v, 47, 900);
    v.hist[47].bright_n = 1;
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "<h1>Green Thumb</h1>"));
    CHECK(has(html, "class=\"pill live\"") && !has(html, "pill stale") && !has(html, "pill wait"));
    CHECK(has(html, "class=\"banner\"") && count(html, "class=\"tile\"") == 3);
    CHECK(has(html, "<small>Soil moisture</small><b>900</b><span>Wet</span>"));
    CHECK(has(html, "<small>Light</small><b>Dark</b>"));
    CHECK(has(html, "<small>Water</small><b>Good</b><span>No water needed</span>"));
    CHECK(has(html, "<small>Low to high, 24 h</small><b>520 to 900</b>"));
    CHECK(strstr(html, "class=\"banner\"") < strstr(html, "class=\"box"));
    CHECK(strstr(html, "class=\"scene\"") != NULL && has(html, "aria-hidden=\"true\""));

    one_record(&a, &v, 700, GT_REC_LIGHT_BRIGHT);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "<small>Soil moisture</small><b>700</b><span>Dry</span>"));
    CHECK(has(html, "<small>Light</small><b>Bright</b>"));
    CHECK(has(html, "<small>Water</small><b>Dry</b><span>Water soon</span>"));
    CHECK(has(html, "<small>Low to high, 24 h</small><b>none yet</b>"));

    page_acc_init(&a);
    add(&a, rec(NOW - PAGE_STALE_MS - 1, 1, 900, 0));
    judge_ok(&a, NOW, &v);
    CHECK(page_render_html(html, sizeof html, &v) > 0 && has(html, "class=\"pill stale\""));
    CHECK(has(html, "<small>Water</small><b>Unknown</b>"));
    page_acc_init(&a);
    judge_ok(&a, NOW, &v);
    CHECK(page_render_html(html, sizeof html, &v) > 0 && has(html, "class=\"pill wait\""));
    CHECK(has(html, "<small>Soil moisture</small><b>none</b>"));
}

/* The product makes no network calls, so the page loads nothing: no link, script, image, style
 * sheet or address of any kind, and it fits the daemon's buffer with room to spare. */
static void page_is_self_contained(void)
{
    static const char* const BANNED[] = {"http://", "https://", "<script", "<link", "<img", " src=",
                                         "url(",    "@import",  "<a ",     "<iframe", "<form",
                                         "<object", "<embed",   "xmlns"};
    page_acc  a;
    page_view views[5];
    char      html[HTML_CAP];

    page_acc_init(&a);
    judge_ok(&a, NOW, &views[0]);
    one_record(&a, &views[1], 900, 0);
    one_record(&a, &views[2], 400, GT_REC_SYNTHETIC);
    one_record(&a, &views[3], 200, GT_REC_UNDERVOLT);
    worst_case_view(&views[4]);

    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        int n = page_render_html(html, sizeof html, &views[i]);

        CHECK(n > 0 && has(html, "<svg") && has(html, "<style"));
        for (size_t b = 0; b < sizeof BANNED / sizeof BANNED[0]; b++) {
            CHECK(!has(html, BANNED[b]));
        }
        CHECK(has(html, "http-equiv=\"refresh\"")); /* still reloads itself */
        if (i == 4) {
            CHECK((size_t)n + 1024u <= sizeof html); /* the longest page leaves 1 KB spare */
        }
    }
}

/* Synthetic data must be labeled, and live data must never be (guardrail: replayed or fake data
 * is labeled on the page). */
static void source_label(void)
{
    page_acc  a;
    page_view v;
    gt_record bad;
    char      html[HTML_CAP];
    char      json[HTML_CAP];

    /* Nothing read yet: no source, no banner. */
    page_acc_init(&a);
    judge_ok(&a, NOW, &v);
    CHECK(v.source == PAGE_SRC_NONE && v.synthetic == 0);
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    CHECK(has(json, "\"source\":\"none\"") && has(json, "\"synthetic_records\":0"));
    CHECK(page_render_html(html, sizeof html, &v) > 0 && !has(html, "SYNTHETIC DATA"));

    /* Live records, even with every other flag set, are live: only the one bit counts. */
    page_acc_init(&a);
    add(&a, rec(NOW - 7000, 1, 900, 0));
    add(&a, rec(NOW - 6000, 2, 900, (uint16_t)(GT_REC_SOIL_INVALID | GT_REC_SOIL_STUCK |
                                              GT_REC_LIGHT_INVALID | GT_REC_LIGHT_BRIGHT |
                                              GT_REC_TIME_UNSET | GT_REC_UNDERVOLT)));
    judge_ok(&a, NOW, &v);
    CHECK(a.synthetic == 0 && v.source == PAGE_SRC_LIVE && v.synthetic == 0);
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    CHECK(has(json, "\"source\":\"live\"") && has(json, "\"synthetic_records\":0"));
    CHECK(page_render_html(html, sizeof html, &v) > 0 && !has(html, "SYNTHETIC DATA"));

    /* All synthetic. */
    page_acc_init(&a);
    add(&a, rec(NOW - 7000, 1, 900, GT_REC_SYNTHETIC));
    add(&a, rec(NOW - 6000, 2, 900, GT_REC_SYNTHETIC | GT_REC_LIGHT_BRIGHT));
    add(&a, rec(NOW - 5000, 3, 900, GT_REC_SYNTHETIC));
    judge_ok(&a, NOW, &v);
    CHECK(a.synthetic == 3 && v.source == PAGE_SRC_SYNTHETIC && v.synthetic == 3);
    CHECK(page_render_json(json, sizeof json, &v) > 0 && json_balanced(json));
    CHECK(has(json, "\"source\":\"synthetic\"") && has(json, "\"synthetic_records\":3"));
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "SYNTHETIC DATA: 3 of 3 readings") && has(html, "not from the sensors"));

    /* Labeling does not change the judgment of the same data. */
    CHECK(v.state == PAGE_OK && v.reasons == 0 && v.soil_known && v.light_known);

    /* Mixed: fake records followed by a live one. */
    page_acc_init(&a);
    add(&a, rec(NOW - 7000, 1, 900, GT_REC_SYNTHETIC));
    add(&a, rec(NOW - 6000, 2, 900, 0));
    add(&a, rec(NOW - 5000, 3, 900, 0));
    judge_ok(&a, NOW, &v);
    CHECK(v.source == PAGE_SRC_MIXED && v.synthetic == 1 && v.records == 3);
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    CHECK(has(json, "\"source\":\"mixed\"") && has(json, "\"synthetic_records\":1"));
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "SYNTHETIC DATA: 1 of 3 readings"));

    /* A damaged record is not counted, whatever its flags say. */
    page_acc_init(&a);
    add(&a, rec(NOW - 5000, 1, 900, 0));
    (void)memset(&bad, 0, sizeof bad);
    bad.flags = GT_REC_SYNTHETIC;
    CHECK(page_acc_visit(&bad, GT_STORE_REC_CORRUPT, &a) == 0);
    judge_ok(&a, NOW, &v);
    CHECK(a.synthetic == 0 && v.source == PAGE_SRC_LIVE);

    /* The banner is plain text: no link, script or address for the no-network guard to object to. */
    page_acc_init(&a);
    add(&a, rec(NOW - 5000, 1, 900, GT_REC_SYNTHETIC));
    judge_ok(&a, NOW, &v);
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(!has(html, "http://") && !has(html, "https://") && !has(html, "<script") &&
          !has(html, "<a ") && !has(html, "<link"));
}

static void names(void)
{
    CHECK(strcmp(page_state_name(PAGE_OK), "ok") == 0);
    CHECK(strcmp(page_state_name(PAGE_DEGRADED), "degraded") == 0);
    CHECK(strcmp(page_state_name(PAGE_NO_DATA), "no_data") == 0);
    CHECK(strcmp(page_source_name(PAGE_SRC_NONE), "none") == 0);
    CHECK(strcmp(page_source_name(PAGE_SRC_LIVE), "live") == 0);
    CHECK(strcmp(page_source_name(PAGE_SRC_SYNTHETIC), "synthetic") == 0);
    CHECK(strcmp(page_source_name(PAGE_SRC_MIXED), "mixed") == 0);
}

int main(void)
{
    empty_store();
    fresh_and_valid();
    stale_boundary();
    newest_record_flags();
    store_and_disk();
    clock_cases();
    history_notes();
    never_ok_when_not();
    buffer_limits();
    extreme_values();
    needs_water_states();
    needs_water_unknown();
    needs_water_on_a_degraded_page();
    box_colors();
    unknown_box();
    range_table();
    plant_picture();
    light_icon();
    history_buckets();
    recent_updates();
    soil_chart();
    light_chart();
    dashboard_top();
    page_is_self_contained();
    source_label();
    names();
    (void)puts("PASS");
    return 0;
}
