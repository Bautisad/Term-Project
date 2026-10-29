#!/bin/sh
# Soil sensor capture: reads the STEMMA soil sensor (I2C 0x36) and logs every read, for the
# S1-S5 tests (DESIGN Part 1).  Feed the file to soilstats.sh.
#
# usage: soilread.sh (--count N | --duration S) [--interval 1] [--label TEXT] [--out FILE]
#                    [--cmd PATH]
#   --count N      take N reads
#   --duration S   read for S seconds: ceil(S / interval) reads, so 172800 s at 60 s is 2880
#   --interval S   seconds between reads, default 1.  Reads are scheduled from the start time,
#                  so a slow read does not push the later ones back.
#   --label TEXT   tag put in every line (no commas), e.g. s2 or insert-3
#   --out FILE     append the CSV to FILE (the header only when the file is empty) and print
#                  the summary on stdout.  Without it the CSV goes to stdout, the summary to stderr.
#   --cmd PATH     run PATH instead of i2ctransfer (the tests use it)
#
# Each read runs `i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2`: two bytes, high byte first, higher
# is wetter.  Every read is logged exactly as the sensor returned it, 0xffff included: one line
# time_utc,elapsed_s,label,raw_hex,raw,error.  A read that fails (or prints something else)
# is logged with raw empty and the reason in error; it is never dropped.  Lines starting with
# # note vcgencmd get_throttled at the start and the end (under-voltage shifts readings).
# SIGTERM or SIGINT stops after the current read and prints the summary.
# Exit 0: every read worked.  1: a read failed or the output is unwritable.  2: bad arguments.

CMD=i2ctransfer
COUNT=
DURATION=
INTERVAL=1
LABEL=
OUT=

STOP=0
SLEEPPID=
ERRF=
READS=0
FAILED=0
FFFF=0
MIN=
MAX=

usage() {
    sed -n '2,/^# Exit 0/p' "$0" | sed 's/^# \{0,1\}//'
}

usage_error() {
    echo "soilread: $*" >&2
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

is_hexbyte() {
    case $1 in
    0x[0-9a-fA-F][0-9a-fA-F]) return 0 ;;
    esac
    return 1
}

NL=$(printf '\n_')
NL=${NL%_}

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help)
        usage
        exit 0
        ;;
    --count | --duration | --interval | --label | --out | --cmd) ;;
    *) usage_error "unknown argument: $1" ;;
    esac
    [ $# -ge 2 ] || usage_error "$1 needs a value"
    case $1 in
    --count)
        need_uint "$1" "$2"
        COUNT=$2
        ;;
    --duration)
        need_uint "$1" "$2"
        DURATION=$2
        ;;
    --interval)
        need_uint "$1" "$2"
        INTERVAL=$2
        ;;
    --label) LABEL=$2 ;;
    --out) OUT=$2 ;;
    --cmd) CMD=$2 ;;
    esac
    shift 2
done

if [ -n "$COUNT" ] && [ -n "$DURATION" ]; then
    usage_error "give --count or --duration, not both"
fi
[ -n "$COUNT" ] || [ -n "$DURATION" ] || usage_error "give --count or --duration"
[ "$INTERVAL" -ge 1 ] || usage_error "--interval must be at least 1"
if [ -n "$COUNT" ]; then
    [ "$COUNT" -ge 1 ] || usage_error "--count must be at least 1"
    N=$COUNT
else
    [ "$DURATION" -ge 1 ] || usage_error "--duration must be at least 1"
    N=$(((DURATION + INTERVAL - 1) / INTERVAL))
fi
case $LABEL in
*,* | *"$NL"*) usage_error "--label must not contain a comma or a newline" ;;
esac
command -v "$CMD" >/dev/null 2>&1 || usage_error "no $CMD found (sudo apt install i2c-tools, or use --cmd)"

if [ -n "$OUT" ]; then
    (: >>"$OUT") 2>/dev/null || usage_error "cannot append to $OUT"
    exec 3>>"$OUT"
    exec 4>&1
    [ -s "$OUT" ] && HEADER=0 || HEADER=1
else
    exec 3>&1
    exec 4>&2
    HEADER=1
fi
ERRF=$(mktemp) || usage_error "cannot create a temporary file"

cleanup() {
    [ -n "$ERRF" ] && rm -f "$ERRF"
}
trap cleanup EXIT
trap 'STOP=1; [ -z "$SLEEPPID" ] || kill "$SLEEPPID" 2>/dev/null' INT TERM

# I2C can hang; give each read a few seconds where timeout(1) exists.
TO=
if command -v timeout >/dev/null 2>&1; then
    TO="timeout 5"
fi

# emit LINE: one line to the output; a write failure (disk full) ends the run.
emit() {
    printf '%s\n' "$1" >&3 || {
        echo "soilread: cannot write the output" >&4
        exit 1
    }
}

throttled() {
    if command -v vcgencmd >/dev/null 2>&1; then
        vcgencmd get_throttled 2>&1 | head -n 1 | tr '\n,' '  '
    else
        echo "throttled=unavailable"
    fi
}

# sanitize TEXT: one line, no commas, so it fits in a CSV field.
sanitize() {
    printf '%s' "$1" | tr '\n,' '  '
}

read_once() {
    now=$(date +%s)
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    el=$((now - START))
    out=$($TO "$CMD" -y 1 w2@0x36 0x0F 0x10 r2 2>"$ERRF")
    rc=$?
    READS=$((READS + 1))
    if [ "$rc" -ne 0 ]; then
        FAILED=$((FAILED + 1))
        emit "$ts,$el,$LABEL,,,rc=$rc $(sanitize "$(cat "$ERRF")")"
        return
    fi
    set -f
    set -- $out
    set +f
    if [ $# -eq 2 ] && is_hexbyte "$1" && is_hexbyte "$2"; then
        hi=$(($1))
        lo=$(($2))
        raw=$((hi * 256 + lo))
        emit "$ts,$el,$LABEL,$(printf '0x%02x%02x' "$hi" "$lo"),$raw,"
        [ "$raw" -ne 65535 ] || FFFF=$((FFFF + 1))
        if [ -z "$MIN" ] || [ "$raw" -lt "$MIN" ]; then
            MIN=$raw
        fi
        if [ -z "$MAX" ] || [ "$raw" -gt "$MAX" ]; then
            MAX=$raw
        fi
    else
        FAILED=$((FAILED + 1))
        emit "$ts,$el,$LABEL,,,unexpected output: $(sanitize "$out")"
    fi
}

START=$(date +%s)
[ "$HEADER" = 0 ] || emit "time_utc,elapsed_s,label,raw_hex,raw,error"
emit "# start label=$LABEL interval=$INTERVAL reads=$N $(throttled)"

k=0
while [ "$k" -lt "$N" ] && [ "$STOP" = 0 ]; do
    due=$((START + k * INTERVAL))
    now=$(date +%s)
    if [ "$due" -gt "$now" ]; then
        sleep $((due - now)) &
        SLEEPPID=$!
        wait "$SLEEPPID" 2>/dev/null
        SLEEPPID=
        [ "$STOP" = 0 ] || break
    fi
    read_once
    k=$((k + 1))
done

emit "# end label=$LABEL $(throttled)"
EARLY=
[ "$STOP" = 0 ] || EARLY=" stopped_early=1"
printf 'soilread label=%s reads=%d failed=%d ffff=%d min=%s max=%s%s\n' "$LABEL" "$READS" \
    "$FAILED" "$FFFF" "${MIN:--}" "${MAX:--}" "$EARLY" >&4
[ "$FAILED" -eq 0 ] || exit 1
exit 0
