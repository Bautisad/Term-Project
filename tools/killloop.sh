#!/bin/sh
# kill -9 loop: DESIGN test P1.  Starts gt-feeder, kills it with SIGKILL after a random
# delay, restarts it, and has gt-logcheck judge the store each time.
#
# usage: killloop.sh [-n 500] [--bin DIR] [--work DIR] [--rate 1000] [--max-delay-ms 1000]
#                    [--seed N] [--stop-on-fail] [--keep] [--checker PATH]
#   -n N              iterations (kills), default 500
#   --bin DIR         directory with gt-feeder and gt-logcheck, default build/tools
#   --work DIR        scratch directory; must be missing or empty, and is never deleted.
#                     Default: a fresh mktemp -d, removed when every iteration passed.
#   --rate N          feeder records per second, default 1000
#   --max-delay-ms N  the kill comes 1..N ms after the feeder is ready, default 1000
#   --seed N          fixes the sequence of delays (printed at the start; default: the clock)
#   --stop-on-fail    stop at the first failing iteration instead of carrying on
#   --keep            keep the scratch directory even when everything passed
#   --checker PATH    use this program instead of gt-logcheck (the tests use it)
#
# Each iteration: start the feeder; wait until it is ready; check the store while it runs
# (a "store recovered" entry for every start, no ERROR lines); wait a random delay; kill -9;
# check again with the writer stopped (no acknowledged record lost).  That second check has to
# come before the restart: a restarted feeder appends past the acknowledged seq in milliseconds.
# Prints one line per iteration, then a summary.  Results go to WORK/results.log, the checker
# output to WORK/checks.log, and the first failure leaves a copy of the store in WORK/failed-iter-N.
# Exit 0: every iteration passed.  1: some failed.  2: bad arguments or missing binaries.

N=500
BIN=build/tools
WORK=
RATE=1000
MAXDELAY=1000
SEED=
STOP=0
KEEP=0
CHECKER=

PID=
FAILED=0
PASSED=0
TORN=0
MAXD=0
RAN=0
FIRSTFAIL=0
WORK_OWNED=0

# A sanitizer finding in a build under test must not look like a normal exit status.
export ASAN_OPTIONS=exitcode=99:detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:exitcode=99

usage() {
    sed -n '2,/^# Exit 0/p' "$0" | sed 's/^# \{0,1\}//'
}

usage_error() {
    echo "killloop: $*" >&2
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

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help)
        usage
        exit 0
        ;;
    --stop-on-fail)
        STOP=1
        shift
        continue
        ;;
    --keep)
        KEEP=1
        shift
        continue
        ;;
    -n | --bin | --work | --rate | --max-delay-ms | --seed | --checker) ;;
    *) usage_error "unknown argument: $1" ;;
    esac
    [ $# -ge 2 ] || usage_error "$1 needs a value"
    case $1 in
    -n)
        need_uint "$1" "$2"
        N=$2
        ;;
    --bin) BIN=$2 ;;
    --work) WORK=$2 ;;
    --rate)
        need_uint "$1" "$2"
        RATE=$2
        ;;
    --max-delay-ms)
        need_uint "$1" "$2"
        MAXDELAY=$2
        ;;
    --seed)
        need_uint "$1" "$2"
        SEED=$2
        ;;
    --checker) CHECKER=$2 ;;
    esac
    shift 2
done

[ "$N" -ge 1 ] || usage_error "-n must be at least 1"
[ "$MAXDELAY" -ge 1 ] || usage_error "--max-delay-ms must be at least 1"
FEEDER=$BIN/gt-feeder
CHECK=${CHECKER:-$BIN/gt-logcheck}
[ -x "$FEEDER" ] || usage_error "no executable gt-feeder at $FEEDER (use --bin)"
[ -x "$CHECK" ] || usage_error "no executable checker at $CHECK (use --bin or --checker)"
[ -n "$SEED" ] || SEED=$(date +%s)

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
PROG=$WORK/progress
RESULTS=$WORK/results.log
CHECKS=$WORK/checks.log
: >"$LOG"
: >"$RESULTS"
: >"$CHECKS"

cleanup() {
    [ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null
    if [ "$WORK_OWNED" = 1 ] && [ "$KEEP" = 0 ] && [ "$FAILED" = 0 ]; then
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# True when the feeder has exited (a dead child stays a zombie until it is waited for).
feeder_exited() {
    kill -0 "$PID" 2>/dev/null || return 0
    grep -q '^State:[[:space:]]*Z' "/proc/$PID/status" 2>/dev/null
}

count_lines() {
    c=$(grep -c "$1" "$LOG" 2>/dev/null)
    echo "${c:-0}"
}

# wait_ready STARTS: the log holds STARTS "feeder start" lines and the first append is
# acknowledged (the progress file is not empty).  0 ready, 1 feeder exited, 2 timed out.
wait_ready() {
    t=0
    while [ "$t" -lt 2000 ]; do
        if [ "$(count_lines 'feeder start ')" -ge "$1" ] && [ -s "$PROG" ]; then
            return 0
        fi
        feeder_exited && return 1
        sleep 0.01
        t=$((t + 1))
    done
    return 2
}

# check LABEL ARGS...: runs the checker, logs its output, returns its exit code.
check() {
    label=$1
    shift
    out=$("$CHECK" "$@" 2>&1)
    rc=$?
    printf '== iter %s %s rc=%s\n%s\n' "$I" "$label" "$rc" "$out" >>"$CHECKS"
    return $rc
}

# A copy of the store at the first failure, taken before anything restarts.
snapshot() {
    [ "$FIRSTFAIL" = 0 ] || return 0
    FIRSTFAIL=$I
    snap=$WORK/failed-iter-$I
    mkdir -p "$snap" && cp -R "$STORE" "$snap/" && cp "$LOG" "$snap/"
    [ -f "$PROG" ] && cp "$PROG" "$snap/"
}

echo "killloop n=$N rate=$RATE max_delay_ms=$MAXDELAY seed=$SEED work=$WORK"
I=0
while [ "$I" -lt "$N" ]; do
    I=$((I + 1))
    RAN=$I
    ok=1
    note=
    res_b=PASS
    res_a=PASS
    delay_ms=0
    dropped=0

    starts=$(count_lines 'feeder start ')
    "$FEEDER" --dir "$STORE" --rate "$RATE" --progress "$PROG" 2>>"$LOG" &
    PID=$!
    wait_ready $((starts + 1))
    rdy=$?
    if [ "$rdy" -ne 0 ]; then
        ok=0
        note=" feeder_not_ready=$rdy"
        res_b=FAIL
        res_a=SKIPPED
        snapshot
        if ! feeder_exited; then
            kill -KILL "$PID" 2>/dev/null
        fi
        wait "$PID" 2>/dev/null
        PID=
    else
        last=$(grep 'store recovered' "$LOG" | tail -n 1 | sed -n 's/.*dropped_bytes=\([0-9]*\).*/\1/p')
        dropped=${last:-0}
        if [ "$dropped" -gt 0 ]; then
            TORN=$((TORN + 1))
        fi
        [ "$dropped" -le "$MAXD" ] || MAXD=$dropped

        # The writer is running: its restart must have logged a recovery.
        if ! check restart --dir "$STORE" --writer-log "$LOG" --restarts $((I - 1)); then
            ok=0
            res_b=FAIL
            snapshot
        fi

        delay_ms=$(awk -v s="$SEED" -v i="$I" -v m="$MAXDELAY" \
            'BEGIN { srand(s + i); rand(); print 1 + int(rand() * m) }')
        sleep "$(awk -v d="$delay_ms" 'BEGIN { printf "%.3f", d / 1000 }')"

        kill -KILL "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
        status=$?
        PID=
        if [ "$status" -ne 137 ]; then
            ok=0
            note=" kill_status=$status"
        fi

        # The writer is stopped: no acknowledged record may be missing.
        if ! check kill --dir "$STORE" --progress "$PROG"; then
            ok=0
            res_a=FAIL
            snapshot
        fi
    fi

    if [ "$ok" = 1 ]; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
    line="iter=$I/$N delay_ms=$delay_ms dropped=$dropped check_restart=$res_b check_kill=$res_a$note"
    echo "$line"
    echo "$line" >>"$RESULTS"
    if [ "$ok" = 0 ] && [ "$STOP" = 1 ]; then
        break
    fi
done

echo "killloop iterations=$RAN passed=$PASSED failed=$FAILED torn_restarts=$TORN max_dropped=$MAXD seed=$SEED"
if [ "$FAILED" -gt 0 ]; then
    echo "FAIL $FAILED of $RAN iterations failed; first failure: iter $FIRSTFAIL"
    echo "work directory kept: $WORK (results.log, checks.log, writer.log, failed-iter-$FIRSTFAIL)"
    exit 1
fi
[ "$KEEP" = 0 ] || echo "work directory kept: $WORK"
echo PASS
exit 0
