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

/* The needs-water state judges freshness and a future-dated record by the page's own limits. */
_Static_assert(PAGE_STALE_MS > 0, "thirst_acc_init refuses a zero freshness limit");

void page_acc_set_dry(page_acc* a, uint16_t dry_raw)
{
    thirst_params p;

    thirst_params_default(&p, dry_raw);
    p.fresh_ms = PAGE_STALE_MS;
    p.ahead_ms = PAGE_FUTURE_MS;
    /* Cannot fail: the only refusal is fresh_ms == 0 (asserted above). */
    (void)thirst_acc_init(&a->thirst, &p);
}

void page_acc_init(page_acc* a)
{
    (void)memset(a, 0, sizeof *a);
    page_acc_set_dry(a, THIRST_DRY_DEFAULT);
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
    thirst_acc_feed(&a->thirst, rec);

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
    if ((rec->flags & GT_REC_SYNTHETIC) != 0) {
        a->synthetic++;
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
    v->synthetic = a->synthetic;
    if (a->records == 0) {
        v->source = PAGE_SRC_NONE;
    } else if (a->synthetic == 0) {
        v->source = PAGE_SRC_LIVE;
    } else if (a->synthetic >= a->records) {
        v->source = PAGE_SRC_SYNTHETIC;
    } else {
        v->source = PAGE_SRC_MIXED;
    }

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

    /* Needs water.  "Yes" is an alert, so it may show on a degraded page.  "No" is a claim that
     * all is well, so it needs the page to be OK too; otherwise it is unknown, never OK. */
    {
        thirst_result t;

        thirst_judge(&a->thirst, in->now_ms, &t);
        v->water = t.state;
        v->water_level_raw = t.level_raw;
        v->water_dry_raw = t.dry_raw;
        if (v->water == THIRST_OK && v->state != PAGE_OK) {
            v->water = THIRST_UNKNOWN;
        }
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

const char* page_source_name(page_source s)
{
    switch (s) {
    case PAGE_SRC_LIVE:
        return "live";
    case PAGE_SRC_SYNTHETIC:
        return "synthetic";
    case PAGE_SRC_MIXED:
        return "mixed";
    case PAGE_SRC_NONE:
        break;
    }
    return "none";
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

/* The JSON word for needs-water. */
static const char* water_word(thirst_state s)
{
    switch (s) {
    case THIRST_NEEDS_WATER:
        return "yes";
    case THIRST_OK:
        return "no";
    case THIRST_UNKNOWN:
        break;
    }
    return "unknown";
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

/* The soil ranges of the table and the colors of the box.  Drafts, from the typical readings of
 * the Adafruit STEMMA sensor (open air 200-300, very dry 300-350, dry 350-450, moist 450-750,
 * wet 750 and up).  A reading of exactly 450 counts as dry, because the needs-water check treats
 * "at or below the dry line" as dry. */
#define ROW_WET_MIN      750u
#define ROW_MOIST_MIN    451u
#define ROW_DRY_MIN      350u
#define ROW_VERY_DRY_MIN 300u
#define N_ROWS           5u

_Static_assert(ROW_MOIST_MIN == THIRST_DRY_DEFAULT + 1u, "the Dry row ends at the default dry line");
/* The gauge below draws these lines on a 200 to 900 scale (14.3%, 21.4%, 35.7%). */
_Static_assert(ROW_VERY_DRY_MIN == 300u && ROW_DRY_MIN == 350u && ROW_MOIST_MIN == 451u,
               "update the gauge colors");
#define GAUGE_MIN 200u
#define GAUGE_MAX 900u

/* Which row of the table a soil reading falls in: 0 wet, 1 moist, 2 dry, 3 very dry, 4 open air. */
static size_t soil_row(uint16_t raw)
{
    if (raw >= ROW_WET_MIN) {
        return 0;
    }
    if (raw >= ROW_MOIST_MIN) {
        return 1;
    }
    if (raw >= ROW_DRY_MIN) {
        return 2;
    }
    return raw >= ROW_VERY_DRY_MIN ? 3 : 4;
}

/* The colored box.  Green needs the needs-water check to say OK, so a state the page cannot
 * vouch for is never green.  Yellow, red and black only appear while the check says the soil
 * is dry (including the margin above the dry line), and say how dry by the reading it rests on. */
typedef enum box_kind { BOX_UNKNOWN, BOX_GREEN, BOX_YELLOW, BOX_RED, BOX_BLACK } box_kind;

static box_kind box_of(const page_view* v)
{
    if (v->water == THIRST_UNKNOWN) {
        return BOX_UNKNOWN;
    }
    if (v->water == THIRST_OK) {
        return BOX_GREEN;
    }
    if (v->water_level_raw < ROW_VERY_DRY_MIN) {
        return BOX_BLACK;
    }
    return v->water_level_raw < ROW_DRY_MIN ? BOX_RED : BOX_YELLOW;
}

/* The page's look: one inline style sheet and one inline picture, so nothing is fetched from
 * anywhere (the product makes no network calls).  The picture has no xmlns attribute: HTML does
 * not need it, and the no-network check refuses the address in it. */
static const char HTML_HEAD[] =
    "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<meta http-equiv=\"refresh\" content=\"30\"><title>Green Thumb</title>\n"
    "<style>"
    ":root{--bg:#f3f6f1;--card:#fff;--ink:#1c2a21;--mut:#5d6e63;--line:#dfe6dc;--hi:#e4f2e6;"
    "--g:#2e9d57;--y:#f0b400;--r:#d4372a;--k:#101010;--n:#79837c}"
    "@media(prefers-color-scheme:dark){:root{--bg:#0f1511;--card:#19211c;--ink:#e5eee7;"
    "--mut:#9db0a2;--line:#2a352d;--hi:#24382a}}"
    "*{box-sizing:border-box}"
    "body{font:16px/1.5 system-ui,-apple-system,\"Segoe UI\",Roboto,sans-serif;background:var(--bg);"
    "color:var(--ink);max-width:34rem;margin:0 auto;padding:1rem}"
    "h1{font-size:1.7rem;margin:.2rem 0 1rem;letter-spacing:-.02em}"
    "h2{font-size:1.05rem;margin:0 0 .4rem}p{margin:.4rem 0}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:1rem;"
    "padding:1rem;margin:0 0 1rem;box-shadow:0 1px 3px rgba(0,0,0,.06)}"
    ".hero{display:flex;gap:1rem;align-items:center}.plant{width:7.5rem;flex:none}"
    ".box{flex:1;border-radius:1rem;padding:1rem;color:#fff;display:flex;flex-direction:column}"
    ".box b{font-size:1.5rem;line-height:1.25}.ico{font-size:1.6rem;line-height:1}"
    ".green{background:var(--g)}.yellow{background:var(--y);color:#2a2100}.red{background:var(--r)}"
    ".black{background:var(--k);border:2px solid #6b6b6b}.unknown{background:var(--n)}"
    ".warn{background:#fff4d6;color:#4a3500;border-left:.35rem solid var(--y);"
    "padding:.6rem .8rem;border-radius:.5rem;margin:0 0 1rem}"
    ".state{padding:.5rem .75rem;border-radius:.4rem;font-weight:bold;margin:0}"
    ".ok{background:#d8f0d8;color:#12331a}.degraded{background:#f6d6d6;color:#4a1010}"
    ".no_data{background:#e6e6e6;color:#222}.needs{background:#f6e0a8;color:#5a3a00}"
    ".fake{background:#222;color:#fff;padding:.5rem .75rem;border-radius:.4rem}"
    ".gauge{position:relative;height:.8rem;border-radius:.4rem;margin:1rem 0 .4rem;"
    "outline:1px solid var(--mut);"
    "background:linear-gradient(90deg,var(--k) 0 14.3%,var(--r) 0 21.4%,var(--y) 0 35.7%,var(--g) 0)}"
    ".gauge i{position:absolute;top:-.3rem;width:.3rem;height:1.4rem;margin-left:-.15rem;"
    "background:var(--ink);border:2px solid var(--card);border-radius:.2rem}"
    "table{width:100%;border-collapse:collapse}"
    "th,td{text-align:left;padding:.4rem .5rem;border-bottom:1px solid var(--line)}"
    "tr.here{background:var(--hi);font-weight:bold}.mut{color:var(--mut);font-size:.9rem}"
    "ul{padding-left:1.2rem;margin:.3rem 0}"
    "</style></head><body>\n<h1>Green Thumb</h1>\n";

static const char PLANT_SVG[] =
    "<svg class=\"plant\" viewBox=\"0 0 120 150\" role=\"img\" aria-label=\"Potted plant\">"
    "<path d=\"M60 96C60 74 58 54 60 30\" stroke=\"#2f7d3c\" stroke-width=\"4\" fill=\"none\" "
    "stroke-linecap=\"round\"/>"
    "<path d=\"M60 82C40 80 22 68 20 46C42 46 58 60 60 82Z\" fill=\"#43a35a\"/>"
    "<path d=\"M60 68C80 66 98 54 100 32C78 32 62 46 60 68Z\" fill=\"#58b86d\"/>"
    "<path d=\"M60 48C48 44 40 32 42 16C56 20 62 34 60 48Z\" fill=\"#3d9b52\"/>"
    "<path d=\"M60 32C70 26 78 16 76 4C64 8 58 20 60 32Z\" fill=\"#6cc880\"/>"
    "<path d=\"M30 100H90L82 144H38Z\" fill=\"#c9693c\"/>"
    "<rect x=\"26\" y=\"90\" width=\"68\" height=\"14\" rx=\"4\" fill=\"#d9794b\"/>"
    "<ellipse cx=\"60\" cy=\"91\" rx=\"30\" ry=\"3\" fill=\"#5a3b26\"/></svg>\n";

/* The picture with the colored box beside it; an unknown state also gets a warning. */
static void put_hero(out* o, const page_view* v)
{
    static const char* const WORD[] = {"unknown", "green", "yellow", "red", "black"};
    static const struct {
        const char* icon;
        const char* title;
        const char* line;
    } TEXT[] = {
        {"?", "Unknown", "Cannot tell whether this plant needs water"},
        {"&#10004;", "Good", "No water needed"},
        {"&#9888;", "Dry", "Water soon"},
        {"&#9888;", "Very dry", "Water now"},
        {"&#9888;", "Extremely dry", "Check that the sensor is in the soil"},
    };
    box_kind k = box_of(v);

    put(o, "<section class=\"card hero\">\n%s", PLANT_SVG);
    put(o, "<div class=\"box %s\" role=\"status\"><span class=\"ico\">%s</span><b>%s</b>"
           "<span>%s</span></div>\n</section>\n",
        WORD[k], TEXT[k].icon, TEXT[k].title, TEXT[k].line);
    if (k == BOX_UNKNOWN) {
        put(o, "<p class=\"warn\">&#9888; Warning: this page cannot tell whether the plant needs "
               "water. Do not assume it is fine. The notes below say why.</p>\n");
    }
}

/* A bar from very dry to wet with a marker at the reading, drawn on a 200 to 900 scale. */
static void put_gauge(out* o, uint16_t raw)
{
    unsigned r = raw < GAUGE_MIN ? GAUGE_MIN : raw;

    r = r > GAUGE_MAX ? GAUGE_MAX : r;
    put(o, "<div class=\"gauge\" role=\"img\" aria-label=\"Soil reading %u, from dry on the left to "
           "wet on the right\"><i style=\"left:%u%%\"></i></div>\n",
        (unsigned)raw, (r - GAUGE_MIN) * 100u / (GAUGE_MAX - GAUGE_MIN));
}

static void put_row(out* o, bool here, const char* name, const char* range, const char* box)
{
    put(o, "<tr%s><th scope=\"row\">%s</th><td>%s</td><td>%s</td></tr>\n",
        here ? " class=\"here\"" : "", name, range, box);
}

/* The ranges, with the row of the newest valid soil value marked. */
static void put_table(out* o, const page_view* v)
{
    size_t here = v->soil_known ? soil_row(v->soil_raw) : N_ROWS;
    char   r[4][24];

    (void)snprintf(r[0], sizeof r[0], "%u and up", ROW_WET_MIN);
    (void)snprintf(r[1], sizeof r[1], "%u to %u", ROW_MOIST_MIN, ROW_WET_MIN - 1u);
    (void)snprintf(r[2], sizeof r[2], "%u to %u", ROW_DRY_MIN, ROW_MOIST_MIN - 1u);
    (void)snprintf(r[3], sizeof r[3], "%u to %u", ROW_VERY_DRY_MIN, ROW_DRY_MIN - 1u);
    put(o, "<section class=\"card\">\n<h2>Soil moisture ranges</h2>\n");
    if (v->soil_known) {
        put(o, "<p>Your reading: %u (higher is wetter), read ", (unsigned)v->soil_raw);
        put_age(o, v->soil_age_ms);
        put(o, " ago.</p>\n");
    }
    put(o, "<table>\n<tr><th scope=\"col\">Level</th><th scope=\"col\">Reading</th>"
           "<th scope=\"col\">Box</th></tr>\n");
    put_row(o, here == 0, "Wet", r[0], "Green");
    put_row(o, here == 1, "Moist", r[1], "Green");
    put_row(o, here == 2, "Dry", r[2], "Yellow");
    put_row(o, here == 3, "Very dry", r[3], "Red");
    (void)snprintf(r[0], sizeof r[0], "below %u", ROW_VERY_DRY_MIN);
    put_row(o, here == 4, "Open air", r[0], "Black");
    put(o, "</table>\n<p class=\"mut\">The box is green only when the needs-water check says the "
           "soil is OK. After a dry spell it stays yellow until the reading is clearly above the "
           "dry line, so it can lag this table. Below %u is what the sensor reads in open air.</p>\n"
           "</section>\n",
        ROW_VERY_DRY_MIN);
}

int page_render_html(char* buf, size_t cap, const page_view* v)
{
    out o = {buf, cap, 0, false};

    if (cap == 0) {
        return -1;
    }
    put(&o, "%s", HTML_HEAD);
    if (v->source == PAGE_SRC_SYNTHETIC || v->source == PAGE_SRC_MIXED) {
        put(&o, "<p class=\"fake\">SYNTHETIC DATA: %llu of %llu readings in this window came from "
                "the test feeder (gt-feeder), not from the sensors. Do not treat them as live.</p>\n",
            (unsigned long long)v->synthetic, (unsigned long long)v->records);
    }
    put_hero(&o, v);
    put(&o, "<section class=\"card\">\n");
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
    put(&o, "</section>\n");

    put(&o, "<section class=\"card\">\n<h2>Needs water</h2>\n");
    switch (v->water) {
    case THIRST_NEEDS_WATER:
        put(&o, "<p class=\"state needs\">Yes, water this plant. Soil reading %u, dry line %u "
                "(higher is wetter).</p>\n",
            (unsigned)v->water_level_raw, (unsigned)v->water_dry_raw);
        break;
    case THIRST_OK:
        put(&o, "<p>No. Soil reading %u, dry line %u (higher is wetter).</p>\n",
            (unsigned)v->water_level_raw, (unsigned)v->water_dry_raw);
        break;
    case THIRST_UNKNOWN:
        put(&o, "<p>Unknown: not enough data.</p>\n");
        break;
    }
    if (v->water != THIRST_UNKNOWN) {
        put(&o, "<p>The dry line is a draft and has not been calibrated.</p>\n");
    }
    if (v->soil_known) {
        put_gauge(&o, v->soil_raw);
    }
    put(&o, "</section>\n");
    put_table(&o, v);

    put(&o, "<section class=\"card\">\n");
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

    put(&o, "</section>\n<section class=\"card\">\n");
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
    put(&o, "</ul>\n</section>\n</body></html>\n");
    return finish(&o);
}

int page_render_json(char* buf, size_t cap, const page_view* v)
{
    out  o = {buf, cap, 0, false};
    bool first = true;

    if (cap == 0) {
        return -1;
    }
    put(&o, "{\"state\":\"%s\",\"source\":\"%s\",\"synthetic_records\":%llu,\"reasons\":[",
        page_state_name(v->state), page_source_name(v->source),
        (unsigned long long)v->synthetic);
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
    put(&o, "\"needs_water\":\"%s\",\"dry_raw\":%u,\"time_until_dry\":null,", water_word(v->water),
        (unsigned)v->water_dry_raw);
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
