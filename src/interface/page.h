/* page.h: what the LAN page says, decided from the records in the store.  No sockets and no
 * I/O, so it is tested without a network.  The daemon (main.c) feeds records in through
 * page_acc_visit, adds the clock, the disk and the store's read result, and serves what
 * page_render_html / page_render_json produce.
 *
 * Rule from #43: an unknown state is never shown as OK.  PAGE_OK needs a fresh, valid
 * record with no flag set.  Prediction and needs-water are always "unknown" until analysis
 * (#27, #26, #43) exists. */
#ifndef GT_PAGE_H
#define GT_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "record.h"
#include "store.h"

/* Drafts, like the bracketed thresholds in DESIGN.md. */
#define PAGE_WINDOW_MS      (24ull * 3600ull * 1000ull) /* how much history the page reads */
#define PAGE_STALE_MS       (3ull * 60ull * 1000ull)    /* three 60 s cycles with no record */
#define PAGE_FUTURE_MS      (60ull * 1000ull)           /* a record this far ahead of now */
#define PAGE_LOW_DISK_BYTES (100ull * 1024ull * 1024ull)

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
} page_acc;

void page_acc_init(page_acc* a);

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

typedef struct page_view {
    page_state state;
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
    bool       disk_known;
    uint64_t   disk_free_bytes;
    /* History notes: facts about the window that do not change the state. */
    uint64_t   records, corrupt, soil_invalid, soil_stuck, light_invalid, undervolt, time_unset;
    uint64_t   gap_events, missing;
} page_view;

void page_judge(const page_input* in, page_view* v);

const char* page_state_name(page_state s); /* "ok", "degraded" or "no_data" */

/* Render into buf (cap bytes).  Returns the length without the terminating NUL, or -1 if
 * cap is too small, in which case buf holds an empty string (if cap > 0). */
int page_render_html(char* buf, size_t cap, const page_view* v);
int page_render_json(char* buf, size_t cap, const page_view* v);

#endif /* GT_PAGE_H */
