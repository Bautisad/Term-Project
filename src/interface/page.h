/* page.h: what the LAN page says, decided from the records in the store.  No sockets and no
 * I/O, so it is tested without a network.  The daemon (main.c) feeds records in through
 * page_acc_visit, adds the clock, the disk and the store's read result, and serves what
 * page_render_html / page_render_json produce.
 *
 * Rule from #43: an unknown state is never shown as OK.  PAGE_OK needs a fresh, valid
 * record with no flag set.  Needs-water comes from thirst.h (a hysteresis around the dry
 * threshold); "no" is only shown while the page itself is PAGE_OK, otherwise it is "unknown".
 * "Yes" is shown whenever thirst.h says so.  The dry threshold is a draft, not calibrated.
 * The page does not show time-until-dry (the JSON still carries the field, always null for now:
 * it needs a long history read), and shows no thresholds, only the status. */
#ifndef GT_PAGE_H
#define GT_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "record.h"
#include "store.h"
#include "thirst.h"

/* Drafts, like the bracketed thresholds in DESIGN.md. */
#define PAGE_WINDOW_MS      (24ull * 3600ull * 1000ull) /* how much history the page reads */
#define PAGE_STALE_MS       (3ull * 60ull * 1000ull)    /* three 60 s cycles with no record */
#define PAGE_FUTURE_MS      (60ull * 1000ull)           /* a record this far ahead of now */
#define PAGE_LOW_DISK_BYTES (100ull * 1024ull * 1024ull)

/* History for the graphs: the window split into equal buckets, oldest first. */
#define PAGE_HIST_N         48u
#define PAGE_HIST_BUCKET_MS (PAGE_WINDOW_MS / PAGE_HIST_N) /* 30 minutes */

/* One bucket of history.  Soil counts only good samples (not INVALID, STUCK or TIME_UNSET, the
 * set dry.c skips); light counts only samples that are not INVALID or TIME_UNSET. */
typedef struct page_bucket {
    uint32_t soil_n;
    uint16_t soil_min;
    uint16_t soil_max;
    uint64_t soil_sum;
    uint32_t light_n;
    uint32_t bright_n; /* light samples that were bright at the end of their cycle */
    uint32_t soil_bad;  /* records with the soil INVALID or STUCK (clock set) */
    uint32_t light_bad; /* records with the light INVALID (clock set) */
} page_bucket;

/* The rounded mean soil value of a bucket, or 0 when it has no soil sample. */
uint16_t page_bucket_soil_avg(const page_bucket* b);

/* The records of the window, boiled down.  Fill it with store_read_range(..., page_acc_visit,
 * &acc) after page_acc_init. */
typedef struct page_acc {
    uint64_t  records;       /* readable records seen */
    uint64_t  corrupt;       /* records reported GT_STORE_REC_CORRUPT */
    uint64_t  soil_invalid;  /* records with GT_REC_SOIL_INVALID */
    uint64_t  soil_stuck;
    uint64_t  light_invalid;
    uint64_t  undervolt;
    uint64_t  time_unset;
    uint64_t  synthetic;     /* readable records with GT_REC_SYNTHETIC (fake feeder, not a sensor) */
    uint64_t  gap_events;    /* places where seq jumped forward by more than one */
    uint64_t  missing;       /* records those jumps skipped */
    bool      have_prev;
    uint32_t  prev_seq;
    bool      have_last;
    gt_record last;          /* the newest readable record */
    bool      have_soil;     /* the newest record whose soil value is valid */
    uint16_t  soil_raw;
    uint64_t  soil_ts_ms;
    bool      have_light;    /* the newest record whose light value is valid */
    bool      light_bright;
    uint16_t  light_bright_ms;
    uint64_t  light_ts_ms;
    thirst_acc thirst;       /* needs-water: fed every readable record, oldest first */
    bool        hist_on;       /* page_acc_set_now was called */
    uint64_t    hist_start_ms; /* the window is [hist_start_ms, hist_start_ms + PAGE_WINDOW_MS] */
    page_bucket hist[PAGE_HIST_N];
} page_acc;

/* Starts empty, with the draft dry threshold THIRST_DRY_DEFAULT. */
void page_acc_init(page_acc* a);

/* Sets the dry threshold.  Call it right after page_acc_init, before any record is visited
 * (it clears what the needs-water state has seen). */
void page_acc_set_dry(page_acc* a, uint16_t dry_raw);

/* Turns the history on, for the window ending at now_ms (wall clock).  Call it before any record
 * is visited.  Without it the history stays empty.  A record outside the window (older, or dated
 * after now_ms) is left out of the history; it still counts everywhere else. */
void page_acc_set_now(page_acc* a, uint64_t now_ms);

/* A gt_store_visit_fn: ctx is a page_acc*.  Always returns 0 (keep reading). */
int page_acc_visit(const gt_record* rec, gt_store_rec_status status, void* ctx);

/* What the page needs besides the records. */
typedef struct page_input {
    const page_acc* acc;
    uint64_t        now_ms;          /* wall clock, ms since the epoch */
    int             store_errno;     /* 0 when the last read of the store worked */
    bool            disk_known;      /* false when statvfs failed */
    uint64_t        disk_free_bytes;
} page_input;

typedef enum page_state {
    PAGE_NO_DATA  = 0, /* the store is readable and empty */
    PAGE_OK       = 1, /* fresh, valid, nothing wrong */
    PAGE_DEGRADED = 2  /* something is wrong now; see reasons */
} page_state;

/* Conditions that make the state PAGE_DEGRADED. */
#define PAGE_R_STORE_UNREADABLE 0x0001u
#define PAGE_R_STALE            0x0002u
#define PAGE_R_SOIL_INVALID     0x0004u /* newest record */
#define PAGE_R_SOIL_STUCK       0x0008u /* newest record */
#define PAGE_R_LIGHT_INVALID    0x0010u /* newest record */
#define PAGE_R_UNDERVOLT        0x0020u /* newest record */
#define PAGE_R_TIME_UNSET       0x0040u /* newest record */
#define PAGE_R_CLOCK_AHEAD      0x0080u
#define PAGE_R_NO_SOIL          0x0100u /* records exist but none has a valid soil value */
#define PAGE_R_LOW_DISK         0x0200u

/* Where the readable records of the window came from.  This labels the data only: it never
 * changes the page state or needs-water. */
typedef enum page_source {
    PAGE_SRC_NONE      = 0, /* no readable records */
    PAGE_SRC_LIVE      = 1, /* none carries GT_REC_SYNTHETIC */
    PAGE_SRC_SYNTHETIC = 2, /* all carry it */
    PAGE_SRC_MIXED     = 3  /* some do */
} page_source;

typedef struct page_view {
    page_state state;
    page_source source;
    uint64_t   synthetic;     /* records of the window with GT_REC_SYNTHETIC */
    unsigned   reasons;       /* PAGE_R_* */
    int        store_errno;
    bool       have_last;
    uint64_t   age_ms;        /* of the newest record; 0 if it is dated ahead of now */
    bool       soil_known;    /* newest valid soil value */
    uint16_t   soil_raw;
    uint64_t   soil_age_ms;
    bool       light_known;   /* newest valid light state */
    bool       light_bright;
    uint16_t   light_bright_ms;
    uint64_t   light_age_ms;
    thirst_state water;       /* needs water: THIRST_UNKNOWN unless known (and "ok" needs PAGE_OK) */
    uint16_t   water_level_raw; /* the soil value the state rests on (0 if there is none) */
    uint16_t   water_dry_raw;   /* the dry threshold in use */
    bool       disk_known;
    uint64_t   disk_free_bytes;
    /* History notes: facts about the window that do not change the state. */
    uint64_t   records, corrupt, soil_invalid, soil_stuck, light_invalid, undervolt, time_unset;
    uint64_t   gap_events, missing;
    /* The graphs: a copy of the accumulator's history (all zero when it was off). */
    bool        hist_on;
    uint64_t    hist_start_ms;
    page_bucket hist[PAGE_HIST_N];
} page_view;

void page_judge(const page_input* in, page_view* v);

/* Recent updates, worked out from the history buckets.  Drafts, like the thresholds above. */
#define PAGE_WATERED_RISE 100u /* soil mean up this much within an hour (two buckets) = watered */
#define PAGE_EVENTS_MAX   5u   /* how many the page lists */

typedef enum page_event_kind {
    PAGE_EV_WATERED,   /* from_raw -> to_raw, a rise of at least PAGE_WATERED_RISE */
    PAGE_EV_WENT_DRY,  /* from above water_dry_raw to at or below it */
    PAGE_EV_SOIL_BAD,  /* count = soil readings invalid or stuck in the bucket */
    PAGE_EV_LIGHT_BAD, /* count = light readings invalid in the bucket */
    PAGE_EV_GAP        /* count = empty buckets just before this one */
} page_event_kind;

typedef struct page_event {
    page_event_kind kind;
    unsigned        bucket; /* index into page_view.hist */
    uint16_t        from_raw;
    uint16_t        to_raw;
    uint32_t        count;
} page_event;

/* Fills ev (up to cap) newest bucket first; within a bucket in the order of page_event_kind.
 * Returns how many.  None when the history is off. */
size_t page_events(const page_view* v, page_event* ev, size_t cap);

const char* page_state_name(page_state s); /* "ok", "degraded" or "no_data" */
const char* page_source_name(page_source s); /* "none", "live", "synthetic" or "mixed" */

/* Render into buf (cap bytes).  Returns the length without the terminating NUL, or -1 if
 * cap is too small, in which case buf holds an empty string (if cap > 0). */
int page_render_html(char* buf, size_t cap, const page_view* v);
int page_render_json(char* buf, size_t cap, const page_view* v);

#endif /* GT_PAGE_H */
