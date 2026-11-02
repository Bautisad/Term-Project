#include "page.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct reason {
    unsigned    bit;
    const char* id;   /* in the JSON */
    const char* text; /* on the page */
} reason;

/* The numbers in two of the texts must match the thresholds in page.h. */
_Static_assert(PAGE_STALE_MS == 180000ull, "update the stale text");
_Static_assert(PAGE_LOW_DISK_BYTES == 100ull * 1024ull * 1024ull, "update the low disk text");

static const reason REASONS[] = {
    {PAGE_R_STORE_UNREADABLE, "store_unreadable", "The reading history cannot be read."},
    {PAGE_R_STALE, "stale", "No new reading for more than 3 minutes."},
    {PAGE_R_SOIL_INVALID, "soil_invalid", "The latest soil reading is invalid."},
    {PAGE_R_SOIL_STUCK, "soil_stuck", "The soil sensor is repeating the same value."},
    {PAGE_R_LIGHT_INVALID, "light_invalid", "The latest light reading is invalid."},
    {PAGE_R_UNDERVOLT, "undervolt", "The Pi reported under-voltage."},
    {PAGE_R_TIME_UNSET, "time_unset", "The clock was not set for the latest reading."},
    {PAGE_R_CLOCK_AHEAD, "clock_ahead", "The latest reading is dated in the future."},
    {PAGE_R_NO_SOIL, "no_valid_soil", "No valid soil reading in the last 24 hours."},
    {PAGE_R_LOW_DISK, "low_disk", "Less than 100 MB of disk is free."},
};
#define N_REASONS (sizeof REASONS / sizeof REASONS[0])

void page_acc_init(page_acc* a)
{
    (void)memset(a, 0, sizeof *a);
}

int page_acc_visit(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    page_acc* a = ctx;

    if (status != GT_STORE_REC_OK) {
        a->corrupt++;
        return 0;
    }
    a->records++;
    if (a->have_prev && (uint64_t)rec->seq > (uint64_t)a->prev_seq + 1u) {
        a->gap_events++;
        a->missing += (uint64_t)rec->seq - (uint64_t)a->prev_seq - 1u;
    }
    a->have_prev = true;
    a->prev_seq = rec->seq;
    a->last = *rec;
    a->have_last = true;

    if ((rec->flags & GT_REC_SOIL_INVALID) != 0) {
        a->soil_invalid++;
    } else {
        a->have_soil = true;
        a->soil_raw = rec->soil_raw;
        a->soil_ts_ms = rec->ts_unix_ms;
    }
    if ((rec->flags & GT_REC_SOIL_STUCK) != 0) {
        a->soil_stuck++;
    }
    if ((rec->flags & GT_REC_LIGHT_INVALID) != 0) {
        a->light_invalid++;
    } else {
        a->have_light = true;
        a->light_bright = (rec->flags & GT_REC_LIGHT_BRIGHT) != 0;
        a->light_bright_ms = rec->light_bright_ms;
        a->light_ts_ms = rec->ts_unix_ms;
    }
    if ((rec->flags & GT_REC_UNDERVOLT) != 0) {
        a->undervolt++;
    }
    if ((rec->flags & GT_REC_TIME_UNSET) != 0) {
        a->time_unset++;
    }
    return 0;
}

static uint64_t age_of(uint64_t now_ms, uint64_t ts_ms)
{
    return now_ms > ts_ms ? now_ms - ts_ms : 0;
}

void page_judge(const page_input* in, page_view* v)
{
    const page_acc* a = in->acc;
    unsigned        r = 0;

    (void)memset(v, 0, sizeof *v);
    v->store_errno = in->store_errno;
    v->disk_known = in->disk_known;
    v->disk_free_bytes = in->disk_free_bytes;
    v->records = a->records;
    v->corrupt = a->corrupt;
    v->soil_invalid = a->soil_invalid;
    v->soil_stuck = a->soil_stuck;
    v->light_invalid = a->light_invalid;
    v->undervolt = a->undervolt;
    v->time_unset = a->time_unset;
    v->gap_events = a->gap_events;
    v->missing = a->missing;

    if (in->store_errno != 0) {
        r |= PAGE_R_STORE_UNREADABLE;
    }
    if (a->have_last) {
        const gt_record* last = &a->last;

        v->have_last = true;
        v->age_ms = age_of(in->now_ms, last->ts_unix_ms);
        if (last->ts_unix_ms > in->now_ms && last->ts_unix_ms - in->now_ms > PAGE_FUTURE_MS) {
            r |= PAGE_R_CLOCK_AHEAD;
        }
        if (v->age_ms > PAGE_STALE_MS) {
            r |= PAGE_R_STALE;
        }
        if ((last->flags & GT_REC_SOIL_INVALID) != 0) {
            r |= PAGE_R_SOIL_INVALID;
        }
        if ((last->flags & GT_REC_SOIL_STUCK) != 0) {
            r |= PAGE_R_SOIL_STUCK;
        }
        if ((last->flags & GT_REC_LIGHT_INVALID) != 0) {
            r |= PAGE_R_LIGHT_INVALID;
        }
        if ((last->flags & GT_REC_UNDERVOLT) != 0) {
            r |= PAGE_R_UNDERVOLT;
        }
        if ((last->flags & GT_REC_TIME_UNSET) != 0) {
            r |= PAGE_R_TIME_UNSET;
        }
        if (!a->have_soil) {
            r |= PAGE_R_NO_SOIL;
        }
    }
    if (a->have_soil) {
        v->soil_known = true;
        v->soil_raw = a->soil_raw;
        v->soil_age_ms = age_of(in->now_ms, a->soil_ts_ms);
    }
    if (a->have_light) {
        v->light_known = true;
        v->light_bright = a->light_bright;
        v->light_bright_ms = a->light_bright_ms;
        v->light_age_ms = age_of(in->now_ms, a->light_ts_ms);
    }
    if (in->disk_known && in->disk_free_bytes < PAGE_LOW_DISK_BYTES) {
        r |= PAGE_R_LOW_DISK;
    }

    v->reasons = r;
    if (r != 0) {
        v->state = PAGE_DEGRADED;
    } else if (!a->have_last) {
        v->state = PAGE_NO_DATA;
    } else {
        v->state = PAGE_OK;
    }
}

const char* page_state_name(page_state s)
{
    switch (s) {
    case PAGE_OK:
        return "ok";
    case PAGE_DEGRADED:
        return "degraded";
    case PAGE_NO_DATA:
        break;
    }
    return "no_data";
}

/* A bounded writer: once anything does not fit, bad stays set and nothing more is written. */
typedef struct out {
    char*  buf;
    size_t cap;
    size_t len;
    bool   bad;
} out;

__attribute__((format(printf, 2, 3)))
static void put(out* o, const char* fmt, ...)
{
    va_list ap;
    int     n;

    if (o->bad) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len) {
        o->bad = true;
        return;
    }
    o->len += (size_t)n;
}

static int finish(out* o)
{
    if (o->bad) {
        if (o->cap > 0) {
            o->buf[0] = '\0';
        }
        return -1;
    }
    return (int)o->len;
}

static void put_age(out* o, uint64_t ms)
{
    uint64_t s = ms / 1000u;

    if (s < 60u) {
        put(o, "%llu s", (unsigned long long)s);
    } else if (s < 3600u) {
        put(o, "%llu min", (unsigned long long)(s / 60u));
    } else {
        put(o, "%llu h %llu min", (unsigned long long)(s / 3600u),
            (unsigned long long)((s % 3600u) / 60u));
    }
}

static const char* state_title(page_state s)
{
    switch (s) {
    case PAGE_OK:
        return "Readings are fresh and valid";
    case PAGE_DEGRADED:
        return "Degraded";
    case PAGE_NO_DATA:
        break;
    }
    return "No data yet";
}

int page_render_html(char* buf, size_t cap, const page_view* v)
{
    out o = {buf, cap, 0, false};

    if (cap == 0) {
        return -1;
    }
    put(&o, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
            "<meta http-equiv=\"refresh\" content=\"30\"><title>Green Thumb</title>\n"
            "<style>body{font-family:sans-serif;max-width:32rem;margin:1rem auto;padding:0 1rem}"
            ".state{padding:.5rem .75rem;border-radius:.4rem;font-weight:bold}"
            ".ok{background:#d8f0d8}.degraded{background:#f6d6d6}.no_data{background:#e6e6e6}"
            "</style></head><body>\n<h1>Green Thumb</h1>\n");
    put(&o, "<p class=\"state %s\">%s</p>\n", page_state_name(v->state), state_title(v->state));
    if (v->reasons != 0) {
        put(&o, "<ul>\n");
        for (size_t i = 0; i < N_REASONS; i++) {
            if ((v->reasons & REASONS[i].bit) != 0) {
                put(&o, "<li>%s</li>\n", REASONS[i].text);
            }
        }
        if ((v->reasons & PAGE_R_STORE_UNREADABLE) != 0) {
            put(&o, "<li>Read error code %d.</li>\n", v->store_errno);
        }
        put(&o, "</ul>\n");
    }

    put(&o, "<h2>Needs water</h2>\n<p>Unknown: not enough data (the analysis is not built yet).</p>\n");
    put(&o, "<h2>Time until dry</h2>\n<p>Not enough data.</p>\n");

    put(&o, "<h2>Soil</h2>\n");
    if (v->soil_known) {
        put(&o, "<p>Raw value %u (higher is wetter), read ", (unsigned)v->soil_raw);
        put_age(&o, v->soil_age_ms);
        put(&o, " ago.</p>\n");
    } else {
        put(&o, "<p>No valid soil reading in the last 24 hours.</p>\n");
    }

    put(&o, "<h2>Light</h2>\n");
    if (v->light_known) {
        put(&o, "<p>%s at the end of the last cycle (%u ms bright), ",
            v->light_bright ? "Bright" : "Dark", (unsigned)v->light_bright_ms);
        put_age(&o, v->light_age_ms);
        put(&o, " ago.</p>\n");
    } else {
        put(&o, "<p>No valid light reading in the last 24 hours.</p>\n");
    }

    put(&o, "<h2>Last 24 hours</h2>\n<ul>\n<li>%llu readings", (unsigned long long)v->records);
    if (v->have_last) {
        put(&o, ", newest ");
        put_age(&o, v->age_ms);
        put(&o, " ago");
    }
    put(&o, "</li>\n<li>%llu invalid soil, %llu stuck soil, %llu invalid light</li>\n",
        (unsigned long long)v->soil_invalid, (unsigned long long)v->soil_stuck,
        (unsigned long long)v->light_invalid);
    put(&o, "<li>%llu under-voltage, %llu with the clock unset</li>\n",
        (unsigned long long)v->undervolt, (unsigned long long)v->time_unset);
    put(&o, "<li>%llu gaps (%llu readings missing), %llu damaged records</li>\n",
        (unsigned long long)v->gap_events, (unsigned long long)v->missing,
        (unsigned long long)v->corrupt);
    if (v->disk_known) {
        put(&o, "<li>%llu MB of disk free</li>\n", (unsigned long long)(v->disk_free_bytes >> 20));
    }
    put(&o, "</ul>\n</body></html>\n");
    return finish(&o);
}

int page_render_json(char* buf, size_t cap, const page_view* v)
{
    out  o = {buf, cap, 0, false};
    bool first = true;

    if (cap == 0) {
        return -1;
    }
    put(&o, "{\"state\":\"%s\",\"reasons\":[", page_state_name(v->state));
    for (size_t i = 0; i < N_REASONS; i++) {
        if ((v->reasons & REASONS[i].bit) != 0) {
            put(&o, "%s\"%s\"", first ? "" : ",", REASONS[i].id);
            first = false;
        }
    }
    put(&o, "],\"store_errno\":%d,", v->store_errno);
    if (v->have_last) {
        put(&o, "\"age_s\":%llu,", (unsigned long long)(v->age_ms / 1000u));
    } else {
        put(&o, "\"age_s\":null,");
    }
    if (v->soil_known) {
        put(&o, "\"soil\":{\"raw\":%u,\"age_s\":%llu},", (unsigned)v->soil_raw,
            (unsigned long long)(v->soil_age_ms / 1000u));
    } else {
        put(&o, "\"soil\":null,");
    }
    if (v->light_known) {
        put(&o, "\"light\":{\"bright\":%s,\"bright_ms\":%u,\"age_s\":%llu},",
            v->light_bright ? "true" : "false", (unsigned)v->light_bright_ms,
            (unsigned long long)(v->light_age_ms / 1000u));
    } else {
        put(&o, "\"light\":null,");
    }
    put(&o, "\"needs_water\":\"unknown\",\"time_until_dry\":null,");
    if (v->disk_known) {
        put(&o, "\"disk_free_mb\":%llu,", (unsigned long long)(v->disk_free_bytes >> 20));
    } else {
        put(&o, "\"disk_free_mb\":null,");
    }
    put(&o, "\"window\":{\"records\":%llu,\"soil_invalid\":%llu,\"soil_stuck\":%llu,"
            "\"light_invalid\":%llu,\"undervolt\":%llu,\"time_unset\":%llu,"
            "\"gap_events\":%llu,\"missing\":%llu,\"corrupt\":%llu}}\n",
        (unsigned long long)v->records, (unsigned long long)v->soil_invalid,
        (unsigned long long)v->soil_stuck, (unsigned long long)v->light_invalid,
        (unsigned long long)v->undervolt, (unsigned long long)v->time_unset,
        (unsigned long long)v->gap_events, (unsigned long long)v->missing,
        (unsigned long long)v->corrupt);
    return finish(&o);
}
