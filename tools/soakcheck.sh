#!/bin/sh
# Soak check: a 1-hour miniature of DESIGN test P4 (the 48-hour soak).  Runs gt-feeder (the
# hub and sensors are not written yet) for a fixed time, samples its memory (RSS) and open
# file count at a fixed interval, then has gt-logcheck judge the store.
#
# usage: soakcheck.sh [BUILD_DIR] [--duration 3600] [--sample 60] [--rate 10] [--work DIR]
#                     [--keep] [--feeder PATH] [--checker PATH]
#   BUILD_DIR         the CMake build directory, default build; the programs are taken from
#                     BUILD_DIR/tools
#   --duration S      seconds to run, default 3600
#   --sample S        seconds between samples, default 60; the first sample is the baseline
#   --rate N          feeder records per second, default 10 (the real hub writes one a minute)
#   --work DIR        scratch directory; must be missing or empty, and is never deleted.
#                     Default: a fresh mktemp -d, removed when everything passed.
#   --keep            keep the scratch directory even when everything passed
#   --feeder PATH     use this program instead of gt-feeder (the tests use it)
#   --checker PATH    use this program instead of gt-logcheck (the tests use it)
#
# Fails on: the feeder exiting early or with a bad status, RSS growth over 1 MB, an open file
# count that changes, a store the checker rejects or that has any gap in its sequence numbers,
# fewer records than the rate promises, or an ERROR line in the feeder's log.
# Results go to WORK/samples.log (elapsed_s name pid rss_kb fds), WORK/writer.log and
# WORK/check.log.
# Exit 0: pass.  1: some check failed.  2: bad arguments or missing binaries.

BUILD=build
DURATION=3600
SAMPLE=60
RATE=10
WORK=
KEEP=0
FEEDER=
CHECK=

# DESIGN P4: RSS growth under 1 MB per process.
MAX_RSS_GROWTH_KB=1024
# The feeder is paced, but not to the microsecond: allow some shortfall before calling it a gap.
MIN_RECORDS_PCT=80

PID=
WORK_OWNED=0

# A sanitizer finding in a build under test must not look like a normal exit status.
export ASAN_OPTIONS=exitcode=99:detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:exitcode=99

usage() {
    sed -n '2,/^# Exit 0/p' "$0" | sed 's/^# \{0,1\}//'
}

usage_error() {
    echo "soakcheck: $*" >&2
    usage >&2
    exit 2
}

is_uint() {
    case $1 in
    '' | *[!0-9]*) return 1 ;;
    esac
    return 0
}

# need_uint FLAG VALUE: the value must be a non-negative integer.
need_uint() {
    is_uint "$2" || usage_error "$1 needs a number, got '$2'"
}

SEEN_BUILD=0
while [ $# -gt 0 ]; do
    case $1 in
    -h | --help)
        usage
        exit 0
        ;;
    --keep)
        KEEP=1
        shift
        continue
        ;;
    --duration | --sample | --rate | --work | --feeder | --checker) ;;
    -*) usage_error "unknown argument: $1" ;;
    *)
        [ "$SEEN_BUILD" = 0 ] || usage_error "unexpected argument: $1"
        SEEN_BUILD=1
        BUILD=$1
        shift
        continue
        ;;
    esac
    [ $# -ge 2 ] || usage_error "$1 needs a value"
    case $1 in
    --duration)
        need_uint "$1" "$2"
        DURATION=$2
        ;;
    --sample)
        need_uint "$1" "$2"
        SAMPLE=$2
        ;;
    --rate)
        need_uint "$1" "$2"
        RATE=$2
        ;;
    --work) WORK=$2 ;;
    --feeder) FEEDER=$2 ;;
    --checker) CHECK=$2 ;;
    esac
    shift 2
done

[ "$SAMPLE" -ge 1 ] || usage_error "--sample must be at least 1"
[ "$RATE" -ge 1 ] || usage_error "--rate must be at least 1"
[ "$DURATION" -ge $((SAMPLE * 2)) ] || usage_error "--duration must cover at least two samples"
[ -n "$FEEDER" ] || FEEDER=$BUILD/tools/gt-feeder
[ -n "$CHECK" ] || CHECK=$BUILD/tools/gt-logcheck
[ -x "$FEEDER" ] || usage_error "no executable feeder at $FEEDER (build first, or use --feeder)"
[ -x "$CHECK" ] || usage_error "no executable checker at $CHECK (build first, or use --checker)"

if [ -z "$WORK" ]; then
    WORK=$(mktemp -d) || usage_error "cannot create a scratch directory"
    WORK_OWNED=1
elif [ -e "$WORK" ]; then
    [ -d "$WORK" ] || usage_error "--work $WORK exists and is not a directory"
    [ -z "$(ls -A "$WORK")" ] || usage_error "--work $WORK is not empty"
else
    mkdir -p "$WORK" || usage_error "cannot create $WORK"
fi

STORE=$WORK/store
LOG=$WORK/writer.log
SAMPLES=$WORK/samples.log
CHECKLOG=$WORK/check.log
FAILURES=$WORK/failures
: >"$LOG"
: >"$SAMPLES"
: >"$FAILURES"

cleanup() {
    [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null
    if [ "$WORK_OWNED" = 1 ] && [ "$KEEP" = 0 ] && [ ! -s "$FAILURES" ]; then
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# fail MESSAGE: records one failed check.
fail() {
    echo "FAIL $*" >>"$FAILURES"
}

# True when the process has exited (a dead child stays a zombie until it is waited for).
exited() {
    kill -0 "$1" 2>/dev/null || return 0
    grep -q '^State:[[:space:]]*Z' "/proc/$1/status" 2>/dev/null
}

# sample ELAPSED NAME PID: appends one line to the samples file.  False if the process is gone.
sample() {
    exited "$3" && return 1
    rss=$(awk '/^VmRSS:/ { print $2 }' "/proc/$3/status" 2>/dev/null)
    fds=$(ls -1 "/proc/$3/fd" 2>/dev/null | wc -l)
    [ -n "$rss" ] || return 1
    echo "$1 $2 $3 $rss $fds" >>"$SAMPLES"
    return 0
}

echo "soakcheck duration=$DURATION sample=$SAMPLE rate=$RATE work=$WORK"
"$FEEDER" --dir "$STORE" --rate "$RATE" 2>>"$LOG" &
PID=$!

elapsed=0
alive=1
while [ "$elapsed" -lt "$DURATION" ]; do
    sleep "$SAMPLE"
    elapsed=$((elapsed + SAMPLE))
    if ! sample "$elapsed" feeder "$PID"; then
        fail "feeder: exited after about ${elapsed}s, an unplanned restart"
        alive=0
        break
    fi
done

if [ "$alive" = 1 ]; then
    kill -TERM "$PID" 2>/dev/null
    wait "$PID"
    status=$?
    [ "$status" = 0 ] || fail "feeder: exit status $status after SIGTERM"
else
    wait "$PID" 2>/dev/null
    status=$?
    echo "feeder exit status $status" >>"$LOG"
fi
PID=

# Memory and file counts, one row per watched process.
awk '
    { if (!($2 in first)) { first[$2] = $4; lo[$2] = $5; hi[$2] = $5; order[++n] = $2 }
      last[$2] = $4
      if ($5 < lo[$2]) lo[$2] = $5
      if ($5 > hi[$2]) hi[$2] = $5 }
    END { for (i = 1; i <= n; i++) {
              p = order[i]
              printf "%s %d %d %d %d %d\n", p, first[p], last[p], last[p] - first[p], lo[p], hi[p] } }
' "$SAMPLES" >"$WORK/summary"

echo "process rss_first_kb rss_last_kb rss_growth_kb fds_min fds_max"
cat "$WORK/summary"
while read -r name first last growth lo hi; do
    if [ "$growth" -gt "$MAX_RSS_GROWTH_KB" ]; then
        fail "$name: RSS grew ${growth} KB (limit ${MAX_RSS_GROWTH_KB} KB)"
    fi
    if [ "$lo" != "$hi" ]; then
        fail "$name: open file count changed, $lo to $hi"
    fi
done <"$WORK/summary"
[ -s "$WORK/summary" ] || fail "no samples were taken"

errors=$(grep -c ' ERROR ' "$LOG" 2>/dev/null)
[ "${errors:-0}" = 0 ] || fail "feeder: ${errors} ERROR lines in $LOG"

# A soak has no kills, so unlike the kill -9 loop even a single skipped number is unexplained.
"$CHECK" --dir "$STORE" --writer-log "$LOG" --restarts 0 >"$CHECKLOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
    fail "checker: exit $rc: $(grep '^FAIL' "$CHECKLOG" | head -n 1)"
fi
summary=$(grep '^logcheck ' "$CHECKLOG" | head -n 1)
records=$(echo "$summary" | sed -n 's/.* records=\([0-9]*\).*/\1/p')
lost=$(echo "$summary" | sed -n 's/.* lost=\([0-9]*\).*/\1/p')
echo "${summary:-logcheck: no summary line}"
if [ "${lost:-0}" != 0 ]; then
    fail "checker: $lost records lost, a gap in the sequence numbers"
fi
expected=$((DURATION * RATE))
if [ "${records:-0}" -lt $((expected * MIN_RECORDS_PCT / 100)) ]; then
    fail "records: ${records:-0} in the store, expected about $expected"
fi

if [ -s "$FAILURES" ]; then
    cat "$FAILURES"
    echo "work directory kept: $WORK (samples.log, writer.log, check.log)"
    exit 1
fi
[ "$KEEP" = 0 ] || echo "work directory kept: $WORK"
echo PASS
exit 0
