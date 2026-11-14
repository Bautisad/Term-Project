#!/bin/sh
# Ground truth: joins hand-made observations (the pot's weight, and what a finger says about the
# soil) to the soil records in the store, and judges the "needs water" flag against them (DESIGN
# test C4).  Also writes the "weight_g raw" lines that `soilstats.sh calib` reads for S1.
#
# usage: groundtruth.sh --weights FILE (--dir DIR | --csv FILE) [--dry N] [--band N] [--window S]
#                       [--calib-out FILE] [--csv-tool PATH]
#   --weights FILE    the observations, one per line, with the header line
#                         utc_time,grams,finger
#                     utc_time is like 2026-11-14T17:30:00Z; grams is the pot's weight; finger is
#                     dry, moist or wet, or empty when only the weight was taken.
#   --dir DIR         the store to read, through gt-csv (read-only, safe while the hub writes)
#   --csv FILE        a gt-csv export instead of a store (used by the tests)
#   --dry N           the flag is yes at or below N, default 450 (THIRST_DRY_DEFAULT)
#   --band N          the flag goes back to no only at N + band, default 48 (the page's band)
#   --window S        a weight is matched to the nearest usable record within S seconds,
#                     default 300; otherwise it is listed as unmatched
#   --calib-out FILE  write "weight_g raw", one line per matched weight, for soilstats.sh calib
#   --csv-tool PATH   gt-csv, default: next to this script
#
# Output, on stdout: a CSV header and one row per weight, in the order of the weights file:
#     utc_time,grams,finger,soil_raw,soil_time,gap_s,flag
# flag is what the page would have said at that record (yes or no), worked out the way
# src/analysis/thirst.c does it, from the start of the data.  A weight with no usable record in the
# window gets empty soil columns and the flag "unmatched"; it is counted, never dropped.  Then:
#     matched=N unmatched=M
#     c4 checks=K agree=A pct=P PASS|FAIL        (or "need=20 INCOMPLETE" below 20 checks)
# A check is a matched weight with a finger value.  dry agrees with flag yes; moist and wet agree
# with flag no.  The target is 90% over at least 20 checks.
#
# A record is usable when its soil reading is valid, not stuck, not clock-unset and not
# synthetic: a synthetic reading is not ground truth.  (The page also shows synthetic readings;
# this tool does not use them.)
#
# Reads only.  The weights file and the store are never written.
# Exit 0: done (a C4 FAIL is a result, not an error).  1: an input could not be read or is
# malformed (the message gives the line).  2: bad arguments.

DRY=450
BAND=48
WINDOW=300
WEIGHTS=
DIR=
CSV=
CALIB=
TOOL=

usage() {
    cat <<'EOF'
usage: groundtruth.sh --weights FILE (--dir DIR | --csv FILE) [--dry N] [--band N] [--window S]
                      [--calib-out FILE] [--csv-tool PATH]
  --weights FILE    utc_time,grams,finger lines (finger: dry, moist, wet or empty)
  --dir DIR         the store to read through gt-csv
  --csv FILE        a gt-csv export instead of a store
  --dry N           flag yes at or below N (default 450)
  --band N          flag back to no at N + band (default 48)
  --window S        match a weight to a record within S seconds (default 300)
  --calib-out FILE  write "weight_g raw" lines for soilstats.sh calib
  --csv-tool PATH   gt-csv (default: next to this script)
Exit 0 done, 1 an input is unreadable or malformed, 2 bad arguments.
EOF
}

usage_error() {
    echo "groundtruth: $1" >&2
    usage >&2
    exit 2
}

input_error() {
    echo "groundtruth: $1" >&2
    exit 1
}

# need_number NAME VALUE MAXDIGITS: VALUE is a whole number of at most MAXDIGITS digits.
need_number() {
    case $2 in
    '' | *[!0-9]*) usage_error "$1 needs a whole number, got '$2'" ;;
    esac
    [ ${#2} -le "$3" ] || usage_error "$1 is too large: '$2'"
}

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help)
        usage
        exit 0
        ;;
    --weights | --dir | --csv | --dry | --band | --window | --calib-out | --csv-tool)
        [ $# -ge 2 ] || usage_error "$1 needs a value"
        case $1 in
        --weights) WEIGHTS=$2 ;;
        --dir) DIR=$2 ;;
        --csv) CSV=$2 ;;
        --dry)
            need_number --dry "$2" 5
            [ "$2" -le 65535 ] || usage_error "--dry is at most 65535, got '$2'"
            DRY=$2
            ;;
        --band)
            need_number --band "$2" 5
            [ "$2" -le 65535 ] || usage_error "--band is at most 65535, got '$2'"
            BAND=$2
            ;;
        --window)
            need_number --window "$2" 9
            WINDOW=$2
            ;;
        --calib-out) CALIB=$2 ;;
        --csv-tool) TOOL=$2 ;;
        esac
        shift 2
        ;;
    *) usage_error "unknown argument: $1" ;;
    esac
done

[ -n "$WEIGHTS" ] || usage_error "--weights is required"
if [ -n "$DIR" ] && [ -n "$CSV" ]; then
    usage_error "give --dir or --csv, not both"
fi
if [ -z "$DIR" ] && [ -z "$CSV" ]; then
    usage_error "give --dir or --csv"
fi

[ -r "$WEIGHTS" ] || input_error "cannot read the weights file: $WEIGHTS"
[ -s "$WEIGHTS" ] || input_error "weights line 1: the file is empty (it needs the header utc_time,grams,finger)"

TMP=$(mktemp -d) || input_error "cannot make a scratch directory"
trap 'rm -rf "$TMP"' EXIT

if [ -n "$DIR" ]; then
    [ -n "$TOOL" ] || TOOL=$(dirname "$0")/gt-csv
    if "$TOOL" --dir "$DIR" >"$TMP/store.csv" 2>"$TMP/csv.err"; then
        # gt-csv reports skipped corrupt records on stderr; pass that on.
        cat "$TMP/csv.err" >&2
    else
        rc=$?
        cat "$TMP/csv.err" >&2
        input_error "gt-csv failed (exit $rc) on $DIR"
    fi
    CSV=$TMP/store.csv
fi
[ -r "$CSV" ] || input_error "cannot read the csv file: $CSV"
[ -s "$CSV" ] || input_error "csv line 1: the file is empty"

if [ -n "$CALIB" ]; then
    : >"$CALIB" || input_error "cannot write $CALIB"
fi

GT_DRY=$DRY GT_BAND=$BAND GT_WINDOW=$WINDOW GT_CALIB=$CALIB awk '
function leap(y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 }

# epoch(s): Unix seconds of a time like 2026-11-14T17:30:00Z, or -1 when it is not one.  Done by
# hand: the Pi has mawk, which has no mktime.
function epoch(s,    y, mo, d, h, mi, sec, ml, md, days, i) {
    if (s !~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z$/) return -1
    y = substr(s, 1, 4) + 0
    mo = substr(s, 6, 2) + 0
    d = substr(s, 9, 2) + 0
    h = substr(s, 12, 2) + 0
    mi = substr(s, 15, 2) + 0
    sec = substr(s, 18, 2) + 0
    if (y < 1970 || mo < 1 || mo > 12 || h > 23 || mi > 59 || sec > 59) return -1
    split("31 28 31 30 31 30 31 31 30 31 30 31", ml, " ")
    md = ml[mo]
    if (mo == 2 && leap(y)) md = 29
    if (d < 1 || d > md) return -1
    days = 0
    for (i = 1970; i < y; i++) days += 365 + leap(i)
    for (i = 1; i < mo; i++) {
        days += ml[i]
        if (i == 2 && leap(y)) days++
    }
    days += d - 1
    return days * 86400 + h * 3600 + mi * 60 + sec
}

function bad(where, msg) {
    print "groundtruth: " where ": " msg > "/dev/stderr"
    errflag = 1
    exit 1
}

BEGIN {
    dry = ENVIRON["GT_DRY"] + 0
    band = ENVIRON["GT_BAND"] + 0
    window = ENVIRON["GT_WINDOW"] + 0
    calib = ENVIRON["GT_CALIB"]
    CSVHEAD = "time_utc,unix_ms,seq,soil_raw,soil_valid,soil_stuck,light,light_valid,light_bright_ms,light_changes,clock_unset,undervoltage,synthetic"
}

# First file: the weights.
NR == FNR {
    line = $0
    sub(/\r$/, "", line)
    where = "weights line " FNR
    if (FNR == 1) {
        if (line != "utc_time,grams,finger") bad(where, "expected the header utc_time,grams,finger")
        next
    }
    if (line == "") next
    if (split(line, f, ",") != 3) bad(where, "expected 3 fields: utc_time,grams,finger")
    t = epoch(f[1])
    if (t < 0) bad(where, "bad time " f[1] " (want like 2026-11-14T17:30:00Z)")
    if (f[2] !~ /^[0-9]+(\.[0-9]+)?$/) bad(where, "bad grams " f[2] " (want a number, not negative)")
    if (f[3] != "" && f[3] != "dry" && f[3] != "moist" && f[3] != "wet") bad(where, "bad finger " f[3] " (want dry, moist, wet or empty)")
    wn++
    wtime[wn] = f[1]
    wsec[wn] = t
    wgrams[wn] = f[2]
    wfinger[wn] = f[3]
    next
}

# Second file: the gt-csv export, oldest first.
{
    line = $0
    sub(/\r$/, "", line)
    where = "csv line " FNR
    if (FNR == 1) {
        if (line != CSVHEAD) bad(where, "unexpected csv header (is this a gt-csv export?)")
        next
    }
    if (line == "") next
    if (split(line, c, ",") != 13) bad(where, "expected 13 fields")
    if (c[2] !~ /^[0-9]+$/ || c[4] !~ /^[0-9]+$/) bad(where, "bad unix_ms or soil_raw")
    # usable: valid, not stuck, clock set, not synthetic
    if (!(c[5] == "yes" && c[6] == "no" && c[11] == "no" && c[13] == "no")) next
    soil = c[4] + 0
    # the flag, as thirst.c walks it
    if (!have) needs = (soil <= dry)
    else if (needs) { if (soil >= dry + (band > 0 ? band : 1)) needs = 0 }
    else if (soil <= dry) needs = 1
    have = 1
    rn++
    rms[rn] = c[2] + 0
    rsoil[rn] = soil
    rtime[rn] = c[1]
    rflag[rn] = needs ? "yes" : "no"
}

END {
    if (errflag) exit 1
    print "utc_time,grams,finger,soil_raw,soil_time,gap_s,flag"
    for (i = 1; i <= wn; i++) {
        best = 0
        bestgap = -1
        for (j = 1; j <= rn; j++) {
            g = rms[j] - wsec[i] * 1000
            if (g < 0) g = -g
            if (bestgap < 0 || g < bestgap) {    # strictly less: a tie keeps the earlier record
                bestgap = g
                best = j
            }
        }
        if (best == 0 || bestgap > window * 1000) {
            unmatched++
            printf "%s,%s,%s,,,,unmatched\n", wtime[i], wgrams[i], wfinger[i]
            continue
        }
        matched++
        printf "%s,%s,%s,%s,%s,%d,%s\n", wtime[i], wgrams[i], wfinger[i], rsoil[best], rtime[best], int(bestgap / 1000), rflag[best]
        if (calib != "") print wgrams[i], rsoil[best] > calib
        if (wfinger[i] != "") {
            checks++
            if ((wfinger[i] == "dry" && rflag[best] == "yes") || (wfinger[i] != "dry" && rflag[best] == "no")) agree++
        }
    }
    printf "matched=%d unmatched=%d\n", matched, unmatched
    pct = checks > 0 ? 100 * agree / checks : 0
    if (checks < 20) printf "c4 checks=%d agree=%d pct=%.1f need=20 INCOMPLETE\n", checks, agree, pct
    else printf "c4 checks=%d agree=%d pct=%.1f %s\n", checks, agree, pct, (agree * 100 >= 90 * checks ? "PASS" : "FAIL")
}
' "$WEIGHTS" "$CSV"
