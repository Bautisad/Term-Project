/* interface_page_test: what the LAN page decides and says, with no network and no store on
 * disk.  Exits 1 at the first failed check. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "page.h"

#define CHECK(c)                                                                        \
    do {                                                                                \
        if (!(c)) {                                                                     \
            (void)fprintf(stderr, "interface_page_test: line %d: %s\n", __LINE__, #c); \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

#define NOW      1700000000000ull
#define GB       (1024ull * 1024ull * 1024ull)
#define HTML_CAP 8192u

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
    CHECK(has(html, "No. Soil reading 914, dry line 565 (higher is wetter)."));
    CHECK(!has(html, "Unknown: not enough data") && !has(html, "Yes, water this plant"));
    CHECK(has(html, "draft and has not been calibrated"));
    CHECK(!has(html, "Degraded"));
    CHECK(page_render_json(json, sizeof json, &v) > 0);
    CHECK(has(json, "\"state\":\"ok\"") && has(json, "\"reasons\":[]"));
    CHECK(has(json, "\"soil\":{\"raw\":914,\"age_s\":5}"));
    CHECK(has(json, "\"light\":{\"bright\":true,\"bright_ms\":1234,\"age_s\":5}"));
    CHECK(has(json, "\"needs_water\":\"no\"") && has(json, "\"dry_raw\":565,"));
    CHECK(has(json, "\"time_until_dry\":null"));
    CHECK(has(html, "<h2>Time until dry</h2>\n<p>Not enough data.</p>"));
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
    CHECK(has(html, "No. Soil reading") == (want == THIRST_OK));
    CHECK(has(html, "Unknown: not enough data") == (want == THIRST_UNKNOWN));
    CHECK(has(html, "class=\"state needs\"") == (want == THIRST_NEEDS_WATER));
}

static void needs_water_states(void)
{
    page_acc  a;
    page_view v;
    char      html[HTML_CAP];

    /* At or below the dry line is dry; one above is not. */
    one_record(&a, &v, 500, 0);
    CHECK(v.state == PAGE_OK && v.water_level_raw == 500 && v.water_dry_raw == THIRST_DRY_DEFAULT);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    CHECK(page_render_html(html, sizeof html, &v) > 0);
    CHECK(has(html, "Yes, water this plant. Soil reading 500, dry line 565 (higher is wetter)."));
    CHECK(has(html, "draft and has not been calibrated"));
    one_record(&a, &v, THIRST_DRY_DEFAULT, 0);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    one_record(&a, &v, THIRST_DRY_DEFAULT + 1u, 0);
    water_is(&v, THIRST_OK, "no");

    /* Hysteresis through the page: stays dry until the reading is 48 above the line. */
    page_acc_init(&a);
    add(&a, rec(NOW - 40000, 1, 500, 0));
    add(&a, rec(NOW - 30000, 2, 612, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    add(&a, rec(NOW - 20000, 3, 613, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_OK, "no");
    add(&a, rec(NOW - 10000, 4, 566, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_OK, "no");
    add(&a, rec(NOW - 5000, 5, 565, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");

    /* A threshold set by the caller. */
    page_acc_init(&a);
    page_acc_set_dry(&a, 700);
    add(&a, rec(NOW - 5000, 1, 650, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    CHECK(v.water_dry_raw == 700);
    CHECK(page_render_html(html, sizeof html, &v) > 0 && has(html, "dry line 700"));
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
    add(&a, rec(NOW - 2000, 2, 500, GT_REC_SOIL_STUCK));
    add(&a, rec(NOW - 1000, 3, 500, GT_REC_TIME_UNSET));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_UNKNOWN, "unknown");

    /* The newest record is bad: an older good one does not stand in, dry or wet. */
    for (int dry = 0; dry < 2; dry++) {
        uint16_t soil = dry ? 500 : 700;

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
        add(&a, rec(NOW - PAGE_STALE_MS, 1, dry ? 500 : 700, 0));
        judge_ok(&a, NOW, &v);
        water_is(&v, dry ? THIRST_NEEDS_WATER : THIRST_OK, dry ? "yes" : "no"); /* exactly 3 min */
        judge_ok(&a, NOW + 1, &v);
        CHECK(v.state == PAGE_DEGRADED);
        water_is(&v, THIRST_UNKNOWN, "unknown");
    }

    /* Dated in the future by more than the page allows. */
    page_acc_init(&a);
    add(&a, rec(NOW + PAGE_FUTURE_MS, 1, 500, 0));
    judge_ok(&a, NOW, &v);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    page_acc_init(&a);
    add(&a, rec(NOW + PAGE_FUTURE_MS + 1u, 1, 500, 0));
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
    one_record(&a, &v, 700, GT_REC_UNDERVOLT);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");
    one_record(&a, &v, 700, GT_REC_LIGHT_INVALID);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");
    one_record(&a, &v, 700, 0);
    judge(&a, NOW, 0, true, 5ull * 1024 * 1024, &v); /* low disk */
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");
    judge(&a, NOW, EIO, true, GB, &v); /* store unreadable */
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_UNKNOWN, "unknown");

    /* Dry and fresh on a degraded page: the alert still shows. */
    one_record(&a, &v, 500, GT_REC_UNDERVOLT);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
    one_record(&a, &v, 500, 0);
    judge(&a, NOW, 0, true, 5ull * 1024 * 1024, &v);
    CHECK(v.state == PAGE_DEGRADED);
    water_is(&v, THIRST_NEEDS_WATER, "yes");
}

static void names(void)
{
    CHECK(strcmp(page_state_name(PAGE_OK), "ok") == 0);
    CHECK(strcmp(page_state_name(PAGE_DEGRADED), "degraded") == 0);
    CHECK(strcmp(page_state_name(PAGE_NO_DATA), "no_data") == 0);
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
    names();
    (void)puts("PASS");
    return 0;
}
