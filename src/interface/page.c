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

void page_acc_set_now(page_acc* a, uint64_t now_ms)
{
    (void)memset(a->hist, 0, sizeof a->hist);
    a->hist_on = true;
    a->hist_start_ms = now_ms > PAGE_WINDOW_MS ? now_ms - PAGE_WINDOW_MS : 0;
}

uint16_t page_bucket_soil_avg(const page_bucket* b)
{
    if (b->soil_n == 0) {
        return 0;
    }
    return (uint16_t)((b->soil_sum + b->soil_n / 2u) / b->soil_n);
}

/* Adds a readable record to its bucket, if it falls inside the window. */
static void hist_add(page_acc* a, const gt_record* rec)
{
    page_bucket* b;
    uint64_t     i;

    if (!a->hist_on || rec->ts_unix_ms < a->hist_start_ms ||
        rec->ts_unix_ms - a->hist_start_ms > PAGE_WINDOW_MS || (rec->flags & GT_REC_TIME_UNSET) != 0) {
        return;
    }
    i = (rec->ts_unix_ms - a->hist_start_ms) / PAGE_HIST_BUCKET_MS;
    b = &a->hist[i < PAGE_HIST_N ? i : PAGE_HIST_N - 1u]; /* the end of the window itself */
    if ((rec->flags & (GT_REC_SOIL_INVALID | GT_REC_SOIL_STUCK)) == 0) {
        if (b->soil_n == 0 || rec->soil_raw < b->soil_min) {
            b->soil_min = rec->soil_raw;
        }
        if (b->soil_n == 0 || rec->soil_raw > b->soil_max) {
            b->soil_max = rec->soil_raw;
        }
        b->soil_n++;
        b->soil_sum += rec->soil_raw;
    } else {
        b->soil_bad++;
    }
    if ((rec->flags & GT_REC_LIGHT_INVALID) == 0) {
        b->light_n++;
        b->bright_n += (rec->flags & GT_REC_LIGHT_BRIGHT) != 0 ? 1u : 0u;
    } else {
        b->light_bad++;
    }
}

int page_acc_visit(const gt_record* rec, gt_store_rec_status status, void* ctx)
{
    page_acc* a = ctx;

    if (status != GT_STORE_REC_OK) {
        a->corrupt++;
        return 0;
    }
    hist_add(a, rec);
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
    v->hist_on = a->hist_on;
    v->hist_start_ms = a->hist_start_ms;
    (void)memcpy(v->hist, a->hist, sizeof v->hist);
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

/* A bucket that saw any record with the clock set, good or bad. */
static bool bucket_any(const page_bucket* b)
{
    return b->soil_n != 0 || b->light_n != 0 || b->soil_bad != 0 || b->light_bad != 0;
}

static void ev_add(page_event* ev, size_t cap, size_t* n, page_event e)
{
    if (*n < cap) {
        ev[(*n)++] = e;
    }
}

size_t page_events(const page_view* v, page_event* ev, size_t cap)
{
    size_t n = 0;

    if (!v->hist_on) {
        return 0;
    }
    for (size_t k = PAGE_HIST_N; k-- > 0 && n < cap;) {
        const page_bucket* b = &v->hist[k];
        size_t             p = 0; /* the newest older bucket with any record */
        size_t             q = 0; /* the newest older bucket with a soil value */
        bool               have_p = false;
        bool               have_q = false;

        if (!bucket_any(b)) {
            continue;
        }
        for (size_t j = k; j-- > 0 && !(have_p && have_q);) {
            if (!have_p && bucket_any(&v->hist[j])) {
                p = j;
                have_p = true;
            }
            if (!have_q && v->hist[j].soil_n != 0) {
                q = j;
                have_q = true;
            }
        }
        if (b->soil_n != 0 && have_q) {
            uint16_t from = page_bucket_soil_avg(&v->hist[q]);
            uint16_t to = page_bucket_soil_avg(b);

            if (k - q <= 2u && (uint32_t)to >= (uint32_t)from + PAGE_WATERED_RISE) {
                ev_add(ev, cap, &n, (page_event){PAGE_EV_WATERED, (unsigned)k, from, to, 0});
            }
            if (from > v->water_dry_raw && to <= v->water_dry_raw) {
                ev_add(ev, cap, &n, (page_event){PAGE_EV_WENT_DRY, (unsigned)k, from, to, 0});
            }
        }
        if (b->soil_bad != 0) {
            ev_add(ev, cap, &n, (page_event){PAGE_EV_SOIL_BAD, (unsigned)k, 0, 0, b->soil_bad});
        }
        if (b->light_bad != 0) {
            ev_add(ev, cap, &n, (page_event){PAGE_EV_LIGHT_BAD, (unsigned)k, 0, 0, b->light_bad});
        }
        if (have_p && k - p >= 2u) {
            ev_add(ev, cap, &n, (page_event){PAGE_EV_GAP, (unsigned)k, 0, 0, (uint32_t)(k - p - 1u)});
        }
    }
    return n;
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

/* The page's look: one inline style sheet and inline pictures, so nothing is fetched from
 * anywhere (the product makes no network calls).  The pictures have no xmlns attribute (HTML does
 * not need it, and the no-network check refuses the address in it) and no gradients (those need
 * url(#id), which the same check refuses); the sky is a CSS gradient behind the banner. */
static const char HTML_HEAD[] =
    "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<meta http-equiv=\"refresh\" content=\"30\"><title>Green Thumb</title>\n"
    "<style>"
    ":root{--bg:#eef2ec;--card:#fff;--ink:#1c2a21;--mut:#5d6e63;--line:#dfe6dc;--hi:#e4f2e6;"
    "--g:#2e9d57;--y:#f0b400;--r:#d4372a;--k:#101010;--n:#79837c;--wt:#2f7fc1;--bk:#101010;"
    "--l0:#e3e0d3;--l1:#f6e8a8;--l2:#f3d464;--l3:#ecbb22;--l4:#d49500;"
    "--sky:linear-gradient(180deg,#86bfe0 0%,#c9e3ec 55%,#e4f0dc 100%)}"
    "@media(prefers-color-scheme:dark){:root{--bg:#0f1511;--card:#19211c;--ink:#e5eee7;"
    "--mut:#9db0a2;--line:#2a352d;--hi:#24382a;--bk:#d5ddd7;"
    "--l0:#2d332e;--l1:#53492a;--l2:#7e6721;--l3:#b58b12;--l4:#f0b400;"
    "--sky:linear-gradient(180deg,#14293a 0%,#223f43 60%,#263b2a 100%)}.scene{filter:brightness(.6)}}"
    "*{box-sizing:border-box}"
    "body{font:16px/1.5 system-ui,-apple-system,\"Segoe UI\",Roboto,sans-serif;background:var(--bg);"
    "color:var(--ink);max-width:76rem;margin:0 auto;padding:1rem}"
    "h1{font-size:1.6rem;margin:0;letter-spacing:-.02em;color:var(--g)}"
    "h2{font-size:1.05rem;margin:0 0 .4rem}p{margin:.4rem 0}"
    ".top{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:.6rem;"
    "margin:0 0 1rem}.top h1::before{content:\"\";display:inline-block;width:.75em;height:.75em;"
    "margin-right:.4em;background:var(--g);border-radius:0 80% 0 80%}"
    ".pill{display:inline-flex;align-items:center;gap:.5rem;margin:0;padding:.3rem .9rem;"
    "border-radius:2rem;background:var(--card);border:1px solid var(--line);font-size:.9rem}"
    ".pill i{width:.6rem;height:.6rem;border-radius:50%;background:var(--n)}"
    ".pill.live i{background:var(--g);box-shadow:0 0 0 .2rem var(--hi)}.pill.stale i{background:var(--y)}"
    ".banner{position:relative;overflow:hidden;border-radius:1.25rem;min-height:17rem;padding:1rem;"
    "margin:0 0 1rem;color:#fff;background:var(--sky);display:flex;flex-direction:column;"
    "justify-content:space-between;gap:1rem;text-shadow:0 1px 2px rgba(0,0,0,.4)}"
    ".scene{position:absolute;inset:0;width:100%;height:100%}.tiles,.strip{position:relative}"
    ".tiles{display:flex;flex-wrap:wrap;gap:.75rem}"
    ".tile,.strip{background:rgba(16,38,22,.45);border:1px solid rgba(255,255,255,.4);"
    "border-radius:1rem;-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px)}"
    ".tile{flex:1 1 9rem;max-width:14rem;padding:.7rem .9rem}"
    ".tile small,.strip small{display:block;font-size:.85rem;opacity:.92}"
    ".tile b{display:block;font-size:1.9rem;line-height:1.15}.tile span{font-size:.9rem}"
    ".strip{display:flex;flex-wrap:wrap;gap:.4rem 2rem;padding:.7rem 1rem}.strip b{font-size:1.2rem}"
    ".grid{display:grid;gap:1rem;grid-template-columns:minmax(0,1fr);grid-auto-flow:dense}"
    "@media(min-width:46rem){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.w2{grid-column:span 2}}"
    "@media(min-width:70rem){.grid{grid-template-columns:repeat(3,minmax(0,1fr))}}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:1rem;"
    "padding:1rem 1.1rem;margin:0;box-shadow:0 1px 3px rgba(0,0,0,.06);min-width:0}"
    ".hero{display:flex;gap:1rem;align-items:center}.plant{width:7rem;flex:none}"
    ".light{display:flex;align-items:center;gap:.8rem;margin:1rem 0;padding:.8rem 0;"
    "border-top:1px solid var(--line);border-bottom:1px solid var(--line)}.sun{width:3rem;flex:none}"
    ".ring{fill:none;stroke:none}"
    ".light.on .core{filter:drop-shadow(0 0 .4rem #f5b700)}"
    ".light.off .core,.light.unk .core{opacity:.45;filter:grayscale(1)}"
    ".light.unk .ring{stroke:var(--mut);stroke-width:2;stroke-dasharray:4 4}"
    ".box{flex:1;border-radius:1rem;padding:1rem;color:#fff;display:flex;flex-direction:column}"
    ".box b{font-size:1.5rem;line-height:1.25}.ico{font-size:1.6rem;line-height:1}"
    ".green{background:var(--g)}.yellow{background:var(--y);color:#2a2100}.red{background:var(--r)}"
    ".black{background:var(--k);border:2px solid #6b6b6b}.unknown{background:var(--n)}"
    ".warn{background:#fff4d6;color:#4a3500;border-left:.35rem solid var(--y);"
    "padding:.6rem .8rem;border-radius:.5rem;margin:0 0 1rem}"
    ".state{padding:.5rem .75rem;border-radius:.4rem;font-weight:bold;margin:0}"
    ".ok{background:#d8f0d8;color:#12331a}.degraded{background:#f6d6d6;color:#4a1010}"
    ".no_data{background:#e6e6e6;color:#222}.needs{background:#f6e0a8;color:#5a3a00}"
    ".fake{background:#222;color:#fff;padding:.5rem .75rem;border-radius:.4rem;margin:0 0 1rem}"
    ".gauge{position:relative;height:.8rem;border-radius:.4rem;margin:1rem 0 .4rem;"
    "outline:1px solid var(--mut);"
    "background:linear-gradient(90deg,var(--k) 0 14.3%,var(--r) 0 21.4%,var(--y) 0 35.7%,var(--g) 0)}"
    ".gauge i{position:absolute;top:-.3rem;width:.3rem;height:1.4rem;margin-left:-.15rem;"
    "background:var(--ink);border:2px solid var(--card);border-radius:.2rem}"
    ".hd p{margin:0}.chart{display:block;width:100%;height:auto;margin:.6rem 0 .2rem}"
    ".chart text{font-size:9px;fill:var(--mut)}@media(max-width:46rem){.chart text{font-size:15px}}"
    ".gl{stroke:var(--line)}"
    ".bd1{fill:var(--y);opacity:.2}.bd2{fill:var(--r);opacity:.16}.bd3{fill:var(--bk);opacity:.1}"
    ".ar{fill:var(--g);opacity:.16}.ln{fill:none;stroke:var(--g);stroke-width:2;"
    "stroke-linejoin:round;stroke-linecap:round}.pt{fill:var(--g)}"
    ".now{fill:var(--g);stroke:var(--card);stroke-width:3}"
    ".hit rect{fill:transparent}.hit rect:hover{fill:var(--ink);fill-opacity:.07}"
    ".b0{fill:var(--l0)}.b1{fill:var(--l1)}.b2{fill:var(--l2)}.b3{fill:var(--l3)}.b4{fill:var(--l4)}"
    ".none{fill:none;stroke:var(--mut);stroke-dasharray:2 2}.ls rect:hover{stroke:var(--ink)}"
    ".sum{display:flex;flex-wrap:wrap;gap:.2rem 1.4rem;color:var(--mut);font-size:.9rem}"
    ".sum b{color:var(--ink);font-size:1.05rem}"
    ".lg{display:flex;flex-wrap:wrap;align-items:center;gap:.35rem;color:var(--mut);font-size:.85rem}"
    ".sw{display:inline-block;width:.8rem;height:.8rem;border-radius:.2rem}"
    ".evs{list-style:none;padding:0;margin:0}"
    ".ev{display:flex;gap:.6rem;padding:.5rem 0;border-bottom:1px solid var(--line)}"
    ".ev:last-child{border:0}.ev i{flex:none;width:.7rem;height:.7rem;border-radius:50%;"
    "margin-top:.45rem;background:var(--n)}.ev.water i{background:var(--wt)}"
    ".ev.dry i{background:var(--y)}.ev.bad i{background:var(--r)}"
    ".ev small{display:block;color:var(--mut)}"
    "table{width:100%;border-collapse:collapse}"
    "th,td{text-align:left;padding:.4rem .5rem;border-bottom:1px solid var(--line)}"
    "tr.here{background:var(--hi);font-weight:bold}.mut{color:var(--mut);font-size:.9rem}"
    "ul{padding-left:1.2rem;margin:.3rem 0}.foot{text-align:center;margin:1rem 0 0}"
    "</style></head><body>\n<header class=\"top\"><h1>Green Thumb</h1>";

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

/* The banner's landscape: a sun, two hills, soil and four sprouts.  Decoration only. */
static const char SCENE_SVG[] =
    "<svg class=\"scene\" viewBox=\"0 0 800 280\" preserveAspectRatio=\"xMidYMax slice\" "
    "aria-hidden=\"true\">"
    "<circle cx=\"700\" cy=\"132\" r=\"30\" fill=\"#fff6c2\" opacity=\".9\"/>"
    "<path d=\"M0 180C110 130 230 160 340 140S560 100 800 150V280H0Z\" fill=\"#a9d29f\" opacity=\".9\"/>"
    "<path d=\"M0 212C150 172 300 200 430 186S660 168 800 196V280H0Z\" fill=\"#64a45d\"/>"
    "<path d=\"M0 240C220 226 560 226 800 242V280H0Z\" fill=\"#6e4b31\"/>"
    "<defs><g id=\"sp\"><path d=\"M0 0V-46\" stroke=\"#3b8a3a\" stroke-width=\"3\" "
    "stroke-linecap=\"round\"/><path d=\"M0-30C-20-30-30-46-30-56C-10-56 0-46 0-30Z\" fill=\"#58b45a\"/>"
    "<path d=\"M0-40C18-40 28-56 28-66C8-66 0-56 0-40Z\" fill=\"#7fd06f\"/></g></defs>"
    "<use href=\"#sp\" transform=\"translate(470 238) scale(.8)\"/>"
    "<use href=\"#sp\" transform=\"translate(560 240) scale(1.3)\"/>"
    "<use href=\"#sp\" transform=\"translate(660 238)\"/>"
    "<use href=\"#sp\" transform=\"translate(740 241) scale(1.6)\"/></svg>\n";

/* The sun icon: bright when it is light, dim when it is dark, and dim with a dashed ring when the
 * page cannot say (no valid reading, the newest one invalid, or older than the page's stale limit),
 * so a missing reading is never drawn as darkness. */
static const char SUN_SVG[] =
    "<svg class=\"sun\" viewBox=\"0 0 64 64\" role=\"img\" aria-label=\"Sun\">"
    "<circle class=\"ring\" cx=\"32\" cy=\"32\" r=\"30\"/>"
    "<g class=\"core\"><path stroke=\"#f5b700\" stroke-width=\"4\" stroke-linecap=\"round\" "
    "d=\"M32 6V14M32 50V58M6 32H14M50 32H58M13.6 13.6L19.3 19.3M44.7 44.7L50.4 50.4"
    "M13.6 50.4L19.3 44.7M44.7 19.3L50.4 13.6\"/>"
    "<circle cx=\"32\" cy=\"32\" r=\"11\" fill=\"#ffd23f\" stroke=\"#f5b700\" stroke-width=\"2\"/>"
    "</g></svg>";

/* Whether the page vouches for the light value: valid, newest, and fresh. */
static bool light_fresh(const page_view* v)
{
    return v->light_known && v->light_age_ms <= PAGE_STALE_MS && (v->reasons & PAGE_R_LIGHT_INVALID) == 0;
}

static void put_light(out* o, const page_view* v)
{
    bool        known = light_fresh(v);
    const char* look = !known ? "unk" : v->light_bright ? "on" : "off";
    const char* text = !known ? "Unknown" : v->light_bright ? "Bright" : "Dark";

    put(o, "<div class=\"light %s\">%s<b>Light: %s</b></div>\n", look, SUN_SVG, text);
}

/* The words of each box, in box_kind order. */
static const char* const BOX_WORD[] = {"unknown", "green", "yellow", "red", "black"};
static const struct {
    const char* icon;
    const char* title;
    const char* line;
} BOX_TEXT[] = {
    {"?", "Unknown", "Cannot tell whether this plant needs water"},
    {"&#10004;", "Good", "No water needed"},
    {"&#9888;", "Dry", "Water soon"},
    {"&#9888;", "Very dry", "Water now"},
    {"&#9888;", "Extremely dry", "Check that the sensor is in the soil"},
};

static const char* const ROW_NAME[N_ROWS] = {"Wet", "Moist", "Dry", "Very dry", "Open air"};

/* The lowest and highest good soil value of the history; false when it has none. */
static bool hist_range(const page_view* v, uint16_t* lo, uint16_t* hi)
{
    bool any = false;

    if (!v->hist_on) {
        return false;
    }
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        const page_bucket* b = &v->hist[i];

        if (b->soil_n == 0) {
            continue;
        }
        if (!any || b->soil_min < *lo) {
            *lo = b->soil_min;
        }
        if (!any || b->soil_max > *hi) {
            *hi = b->soil_max;
        }
        any = true;
    }
    return any;
}

/* The title line: a dot that says whether the readings are live. */
static void put_top(out* o, const page_view* v)
{
    if (!v->have_last) {
        put(o, "<p class=\"pill wait\"><i></i>Waiting for the first reading</p></header>\n");
        return;
    }
    put(o, "<p class=\"pill %s\"><i></i>%s, newest reading ", (v->reasons & PAGE_R_STALE) != 0 ? "stale" : "live",
        (v->reasons & PAGE_R_STALE) != 0 ? "Stale" : "Live");
    put_age(o, v->age_ms);
    put(o, " ago</p></header>\n");
}

/* The banner: a landscape with three glass tiles (soil, light, water) and a strip of the day's
 * numbers.  It repeats what the cards below say; it decides nothing of its own. */
static void put_banner(out* o, const page_view* v)
{
    box_kind k = box_of(v);
    uint16_t lo = 0;
    uint16_t hi = 0;

    put(o, "<section class=\"banner\">\n%s<div class=\"tiles\">\n", SCENE_SVG);
    if (v->soil_known) {
        put(o, "<div class=\"tile\"><small>Soil moisture</small><b>%u</b><span>%s</span></div>\n",
            (unsigned)v->soil_raw, ROW_NAME[soil_row(v->soil_raw)]);
    } else {
        put(o, "<div class=\"tile\"><small>Soil moisture</small><b>none</b><span>no valid reading</span>"
               "</div>\n");
    }
    if (light_fresh(v)) {
        put(o, "<div class=\"tile\"><small>Light</small><b>%s</b><span>read ",
            v->light_bright ? "Bright" : "Dark");
        put_age(o, v->light_age_ms);
        put(o, " ago</span></div>\n");
    } else {
        put(o, "<div class=\"tile\"><small>Light</small><b>Unknown</b><span>no fresh reading</span></div>\n");
    }
    put(o, "<div class=\"tile\"><small>Water</small><b>%s</b><span>%s</span></div>\n</div>\n",
        BOX_TEXT[k].title, BOX_TEXT[k].line);

    put(o, "<div class=\"strip\">\n<div><small>Readings, 24 h</small><b>%llu</b></div>\n",
        (unsigned long long)v->records);
    if (hist_range(v, &lo, &hi)) {
        put(o, "<div><small>Low to high, 24 h</small><b>%u to %u</b></div>\n", (unsigned)lo, (unsigned)hi);
    } else {
        put(o, "<div><small>Low to high, 24 h</small><b>none yet</b></div>\n");
    }
    put(o, "<div><small>Gaps, 24 h</small><b>%llu</b></div>\n", (unsigned long long)v->gap_events);
    if (v->disk_known) {
        put(o, "<div><small>Disk free</small><b>%llu MB</b></div>\n",
            (unsigned long long)(v->disk_free_bytes >> 20));
    } else {
        put(o, "<div><small>Disk free</small><b>unknown</b></div>\n");
    }
    put(o, "</div>\n</section>\n");
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

/* The plant card: the picture with the colored box beside it, the light, and needs-water. */
static void put_hero(out* o, const page_view* v)
{
    box_kind k = box_of(v);

    put(o, "<section class=\"card\">\n<div class=\"hero\">\n%s", PLANT_SVG);
    put(o, "<div class=\"box %s\" role=\"status\"><span class=\"ico\">%s</span><b>%s</b>"
           "<span>%s</span></div>\n</div>\n",
        BOX_WORD[k], BOX_TEXT[k].icon, BOX_TEXT[k].title, BOX_TEXT[k].line);
    put_light(o, v);
    put(o, "<h2>Needs water</h2>\n");
    switch (v->water) {
    case THIRST_NEEDS_WATER:
        put(o, "<p class=\"state needs\">Yes, water this plant.</p>\n");
        break;
    case THIRST_OK:
        put(o, "<p>No, this plant does not need water.</p>\n");
        break;
    case THIRST_UNKNOWN:
        put(o, "<p>Unknown: not enough data.</p>\n");
        break;
    }
    if (v->soil_known) {
        put_gauge(o, v->soil_raw);
    }
    put(o, "</section>\n");
}

/* A duration of `halves` half hours: "30 min", "2 h", "2 h 30 min". */
static void put_halves(out* o, uint64_t halves)
{
    if (halves < 2u) {
        put(o, "%u min", halves == 1u ? 30u : 0u);
    } else if (halves % 2u == 0u) {
        put(o, "%llu h", (unsigned long long)(halves / 2u));
    } else {
        put(o, "%llu h 30 min", (unsigned long long)(halves / 2u));
    }
}

/* When bucket k was, counted back from now: "2 h to 1 h 30 min ago", or "in the last 30 min". */
static void put_span(out* o, size_t k)
{
    if (k + 1u >= PAGE_HIST_N) {
        put(o, "in the last 30 min");
        return;
    }
    put_halves(o, PAGE_HIST_N - k);
    put(o, " to ");
    put_halves(o, PAGE_HIST_N - 1u - k);
    put(o, " ago");
}

/* The graphs share one x axis: 48 slots of 9 units from x = 40, in a 480-wide picture. */
#define CH_X0  40u
#define CH_BW  9u
#define CH_TOP 10u
#define CH_BOT 110u
#define CH_MIN 200u /* soil scale: the open-air floor ... */
#define CH_MAX 1000u /* ... to wetter than the Wet row starts */
_Static_assert(CH_X0 + CH_BW * PAGE_HIST_N == 472u, "the graphs are 480 wide");
_Static_assert((CH_MAX - CH_MIN) % (CH_BOT - CH_TOP) == 0u, "whole units per step");

static unsigned ch_x(size_t i)
{
    return CH_X0 + CH_BW * (unsigned)i + CH_BW / 2u;
}

static unsigned ch_y(unsigned raw)
{
    unsigned r = raw < CH_MIN ? CH_MIN : raw > CH_MAX ? CH_MAX : raw;

    return CH_BOT - (r - CH_MIN) / ((CH_MAX - CH_MIN) / (CH_BOT - CH_TOP));
}

static void put_x_axis(out* o, unsigned y)
{
    put(o, "<text x=\"%u\" y=\"%u\">24 h ago</text><text x=\"%u\" y=\"%u\" text-anchor=\"middle\">"
           "12 h ago</text><text x=\"%u\" y=\"%u\" text-anchor=\"end\">now</text>",
        CH_X0, y, CH_X0 + CH_BW * PAGE_HIST_N / 2u, y, CH_X0 + CH_BW * PAGE_HIST_N, y);
}

/* The line (area false) or the filled area under it (area true): one piece per run of slots that
 * have a soil value, so a slot with no reading breaks it.  A run of one is left to put_dots. */
static void put_runs(out* o, const page_view* v, bool area)
{
    size_t i = 0;

    while (i < PAGE_HIST_N) {
        size_t j = i;

        if (v->hist[i].soil_n == 0) {
            i++;
            continue;
        }
        while (j + 1u < PAGE_HIST_N && v->hist[j + 1u].soil_n != 0) {
            j++;
        }
        if (j > i) {
            if (area) {
                put(o, "M%u %uL", ch_x(i), CH_BOT);
            }
            for (size_t k = i; k <= j; k++) {
                put(o, "%s%u %u", k == i && !area ? "M" : k == i ? "" : "L", ch_x(k),
                    ch_y(page_bucket_soil_avg(&v->hist[k])));
            }
            if (area) {
                put(o, "L%u %uZ", ch_x(j), CH_BOT);
            }
        }
        i = j + 1u;
    }
}

static void put_soil_chart(out* o, const page_view* v)
{
    uint16_t lo = 0;
    uint16_t hi = 0;
    size_t   newest = PAGE_HIST_N;

    put(o, "<section class=\"card w2\">\n<div class=\"hd\"><h2>Soil moisture</h2><p class=\"mut\">"
           "Last 24 hours, half-hour averages (higher is wetter)</p></div>\n");
    if (!hist_range(v, &lo, &hi)) {
        put(o, "<p>No soil readings in the last 24 hours yet.</p>\n</section>\n");
        return;
    }
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        if (v->hist[i].soil_n != 0) {
            newest = i;
        }
    }
    put(o, "<svg class=\"chart\" viewBox=\"0 0 480 136\" role=\"img\" aria-label=\"Soil moisture, last "
           "24 hours\">");
    put(o, "<rect class=\"bd1\" x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\"/>", CH_X0,
        ch_y(ROW_MOIST_MIN - 1u), CH_BW * PAGE_HIST_N, ch_y(ROW_DRY_MIN) - ch_y(ROW_MOIST_MIN - 1u));
    put(o, "<rect class=\"bd2\" x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\"/>", CH_X0,
        ch_y(ROW_DRY_MIN), CH_BW * PAGE_HIST_N, ch_y(ROW_VERY_DRY_MIN) - ch_y(ROW_DRY_MIN));
    put(o, "<rect class=\"bd3\" x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\"/>", CH_X0,
        ch_y(ROW_VERY_DRY_MIN), CH_BW * PAGE_HIST_N, CH_BOT - ch_y(ROW_VERY_DRY_MIN));
    {
        static const unsigned TICKS[] = {ROW_VERY_DRY_MIN, ROW_MOIST_MIN - 1u, ROW_WET_MIN};

        for (size_t t = 0; t < sizeof TICKS / sizeof TICKS[0]; t++) {
            put(o, "<path class=\"gl\" d=\"M%u %uH%u\"/><text x=\"%u\" y=\"%u\" text-anchor=\"end\">%u</text>",
                CH_X0, ch_y(TICKS[t]), CH_X0 + CH_BW * PAGE_HIST_N, CH_X0 - 6u, ch_y(TICKS[t]) + 4u,
                TICKS[t]);
        }
    }
    put(o, "<path class=\"ar\" d=\"");
    put_runs(o, v, true);
    put(o, "\"/><path class=\"ln\" d=\"");
    put_runs(o, v, false);
    put(o, "\"/>");
    for (size_t i = 0; i < PAGE_HIST_N; i++) { /* a run of one, other than the newest */
        bool alone = v->hist[i].soil_n != 0 && (i == 0 || v->hist[i - 1u].soil_n == 0) &&
                     (i + 1u == PAGE_HIST_N || v->hist[i + 1u].soil_n == 0);

        if (alone && i != newest) {
            put(o, "<circle class=\"pt\" cx=\"%u\" cy=\"%u\" r=\"3\"/>", ch_x(i),
                ch_y(page_bucket_soil_avg(&v->hist[i])));
        }
    }
    put(o, "<circle class=\"now\" cx=\"%u\" cy=\"%u\" r=\"5\"/>", ch_x(newest),
        ch_y(page_bucket_soil_avg(&v->hist[newest])));
    put_x_axis(o, 130u);
    put(o, "<g class=\"hit\">");
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        const page_bucket* b = &v->hist[i];

        if (b->soil_n == 0) {
            continue;
        }
        put(o, "<rect x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\"><title>Soil ", CH_X0 + CH_BW * (unsigned)i,
            CH_TOP, CH_BW, CH_BOT - CH_TOP);
        put_span(o, i);
        put(o, ": average %u, low %u, high %u</title></rect>", (unsigned)page_bucket_soil_avg(b),
            (unsigned)b->soil_min, (unsigned)b->soil_max);
    }
    put(o, "</g></svg>\n<p class=\"sum\">");
    if (v->soil_known) {
        put(o, "<span>Now <b>%u</b></span>", (unsigned)v->soil_raw);
    }
    put(o, "<span>Low <b>%u</b></span><span>High <b>%u</b></span></p>\n", (unsigned)lo, (unsigned)hi);
    put(o, "<p class=\"lg\"><i class=\"sw\" style=\"background:var(--y);opacity:.5\"></i>Dry "
           "<i class=\"sw\" style=\"background:var(--r);opacity:.45\"></i>Very dry "
           "<i class=\"sw\" style=\"background:var(--bk);opacity:.3\"></i>Open air</p>\n</section>\n");
}

/* The share of bright readings in a slot, in five steps: 0 none, 4 all, 1 to 3 in between. */
static unsigned light_step(const page_bucket* b)
{
    if (b->bright_n == 0) {
        return 0;
    }
    if (b->bright_n >= b->light_n) {
        return 4;
    }
    return 1u + (unsigned)((uint64_t)b->bright_n * 3u / b->light_n);
}

static void put_light_chart(out* o, const page_view* v)
{
    uint64_t n = 0;
    uint64_t bright = 0;

    put(o, "<section class=\"card w2\">\n<div class=\"hd\"><h2>Light</h2><p class=\"mut\">Last 24 hours, "
           "share of bright readings per half hour</p></div>\n");
    for (size_t i = 0; v->hist_on && i < PAGE_HIST_N; i++) {
        n += v->hist[i].light_n;
        bright += v->hist[i].bright_n;
    }
    if (n == 0) {
        put(o, "<p>No light readings in the last 24 hours yet.</p>\n</section>\n");
        return;
    }
    put(o, "<svg class=\"chart ls\" viewBox=\"0 0 480 60\" role=\"img\" aria-label=\"Light, last 24 hours\">");
    for (size_t i = 0; i < PAGE_HIST_N; i++) {
        const page_bucket* b = &v->hist[i];

        if (b->light_n == 0) {
            put(o, "<rect class=\"none\" x=\"%u\" y=\"8\" width=\"7\" height=\"28\" rx=\"2\"><title>Light ",
                CH_X0 + CH_BW * (unsigned)i);
            put_span(o, i);
            put(o, ": no reading</title></rect>");
            continue;
        }
        put(o, "<rect class=\"b%u\" x=\"%u\" y=\"8\" width=\"7\" height=\"28\" rx=\"2\"><title>Light ",
            light_step(b), CH_X0 + CH_BW * (unsigned)i);
        put_span(o, i);
        put(o, ": bright in %u of %u readings</title></rect>", (unsigned)b->bright_n, (unsigned)b->light_n);
    }
    put_x_axis(o, 54u);
    put(o, "</svg>\n<p class=\"sum\">Bright in %llu of %llu readings, about ", (unsigned long long)bright,
        (unsigned long long)n);
    put_age(o, bright * 60000u);
    put(o, " (one reading a minute).</p>\n<p class=\"lg\">Dark");
    for (unsigned s = 0; s <= 4u; s++) {
        put(o, "<i class=\"sw\" style=\"background:var(--l%u)\"></i>", s);
    }
    put(o, "Bright &nbsp;<i class=\"sw\" style=\"border:1px dashed var(--mut)\"></i>No reading</p>\n"
           "</section>\n");
}

static void put_updates(out* o, const page_view* v)
{
    page_event ev[PAGE_EVENTS_MAX];
    size_t     n = page_events(v, ev, PAGE_EVENTS_MAX);
    bool       any = false;

    put(o, "<section class=\"card\">\n<h2>Recent updates</h2>\n");
    for (size_t i = 0; v->hist_on && i < PAGE_HIST_N; i++) {
        any = any || bucket_any(&v->hist[i]);
    }
    if (!any) {
        put(o, "<p>No readings in the last 24 hours.</p>\n</section>\n");
        return;
    }
    if (n == 0) {
        put(o, "<p>Nothing new in the last 24 hours.</p>\n</section>\n");
        return;
    }
    put(o, "<ul class=\"evs\">\n");
    for (size_t i = 0; i < n; i++) {
        const page_event* e = &ev[i];

        switch (e->kind) {
        case PAGE_EV_WATERED:
            put(o, "<li class=\"ev water\"><i></i><div><b>Watered</b> soil rose from %u to %u",
                (unsigned)e->from_raw, (unsigned)e->to_raw);
            break;
        case PAGE_EV_WENT_DRY:
            put(o, "<li class=\"ev dry\"><i></i><div><b>Went dry</b> soil fell from %u to %u",
                (unsigned)e->from_raw, (unsigned)e->to_raw);
            break;
        case PAGE_EV_SOIL_BAD:
        case PAGE_EV_LIGHT_BAD:
            put(o, "<li class=\"ev bad\"><i></i><div><b>%s sensor</b> %u bad reading%s",
                e->kind == PAGE_EV_SOIL_BAD ? "Soil" : "Light", (unsigned)e->count, e->count == 1u ? "" : "s");
            break;
        case PAGE_EV_GAP:
            put(o, "<li class=\"ev gap\"><i></i><div><b>No readings</b> for at least ");
            put_halves(o, e->count);
            break;
        }
        put(o, "<small>");
        put_span(o, e->bucket);
        put(o, "</small></div></li>\n");
    }
    put(o, "</ul>\n</section>\n");
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
    put(o, "<section class=\"card w2\">\n<h2>Soil moisture ranges</h2>\n");
    if (v->soil_known) {
        put(o, "<p>Your reading: %u (higher is wetter), read ", (unsigned)v->soil_raw);
        put_age(o, v->soil_age_ms);
        put(o, " ago.</p>\n");
    }
    put(o, "<table>\n<tr><th scope=\"col\">Level</th><th scope=\"col\">Reading</th>"
           "<th scope=\"col\">Box</th></tr>\n");
    put_row(o, here == 0, ROW_NAME[0], r[0], "Green");
    put_row(o, here == 1, ROW_NAME[1], r[1], "Green");
    put_row(o, here == 2, ROW_NAME[2], r[2], "Yellow");
    put_row(o, here == 3, ROW_NAME[3], r[3], "Red");
    (void)snprintf(r[0], sizeof r[0], "below %u", ROW_VERY_DRY_MIN);
    put_row(o, here == 4, ROW_NAME[4], r[0], "Black");
    put(o, "</table>\n<p class=\"mut\">The box is green only when the needs-water check says the "
           "soil is OK. After a dry spell it stays yellow until the reading is clearly above the "
           "Dry row, so it can lag this table. Below %u is what the sensor reads in open air. The "
           "ranges are typical values for this sensor type, not measured for your plant.</p>\n"
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
    put_top(&o, v);
    if (v->source == PAGE_SRC_SYNTHETIC || v->source == PAGE_SRC_MIXED) {
        put(&o, "<p class=\"fake\">SYNTHETIC DATA: %llu of %llu readings in this window came from "
                "the test feeder (gt-feeder), not from the sensors. Do not treat them as live.</p>\n",
            (unsigned long long)v->synthetic, (unsigned long long)v->records);
    }
    put_banner(&o, v);
    if (box_of(v) == BOX_UNKNOWN) {
        put(&o, "<p class=\"warn\">&#9888; Warning: this page cannot tell whether the plant needs "
                "water. Do not assume it is fine. The notes below say why.</p>\n");
    }
    put(&o, "<main class=\"grid\">\n");
    put_hero(&o, v);
    put_soil_chart(&o, v);
    put_light_chart(&o, v);
    put_updates(&o, v);
    put_table(&o, v);

    put(&o, "<section class=\"card\">\n<h2>Status</h2>\n");
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
    put(&o, "</ul>\n</section>\n</main>\n<p class=\"foot mut\">Served by the plant monitor on your own "
            "network. This page reloads every 30 seconds.</p>\n</body></html>\n");
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
