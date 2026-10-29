#!/bin/sh
# Soil tool tests.  usage: soiltools_test.sh <soilread.sh> <soilstats.sh>
# soilread.sh runs against a fake i2ctransfer; soilstats.sh against canned files with known
# answers: a passing case, a failing case and a boundary case for each of S1-S5.

READ=$1
STATS=$2
WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "FAIL: $*"
    echo "--- output:"
    echo "$OUT"
    exit 1
}

contains() {
    case $OUT in
    *"$1"*) return 0 ;;
    esac
    return 1
}

# ---------------------------------------------------------------- soilread.sh

FAKES=0

# fake LINE...: a fake i2ctransfer in $FAKE that prints the Nth line of its script on the Nth
# call (the last line again after that).  A line "FAIL 1 boom" exits 1 with "boom" on stderr.
fake() {
    FAKES=$((FAKES + 1))
    FAKE=$WORK/fake$FAKES
    mkdir -p "$FAKE"
    : >"$FAKE/seq"
    for l in "$@"; do
        printf '%s\n' "$l" >>"$FAKE/seq"
    done
    cat >"$FAKE/i2ctransfer" <<'EOF'
#!/bin/sh
d=$(dirname "$0")
n=$(cat "$d/n" 2>/dev/null || echo 0)
n=$((n + 1))
echo "$n" >"$d/n"
line=$(sed -n "${n}p" "$d/seq")
[ -n "$line" ] || line=$(tail -n 1 "$d/seq")
case $line in
FAIL*)
    echo "${line#FAIL * }" >&2
    exit "$(echo "$line" | cut -d' ' -f2)"
    ;;
*) echo "$line" ;;
esac
EOF
    chmod +x "$FAKE/i2ctransfer"
}

# read_run ARGS...: runs soilread.sh against $FAKE; output in OUT, exit code in RC.
read_run() {
    OUT=$(sh "$READ" --cmd "$FAKE/i2ctransfer" "$@" 2>&1)
    RC=$?
}

# 1. Reads are logged as the sensor returned them, with a header and the summary.
fake "0x03 0x1f" "0x03 0x20" "0x03 0x1e"
F=$WORK/s2.csv
read_run --count 3 --interval 1 --label s2 --out "$F"
[ "$RC" = 0 ] || fail "normal run exited $RC"
contains "soilread label=s2 reads=3 failed=0 ffff=0 min=798 max=800" || fail "normal run: summary"
[ "$(sed -n 1p "$F")" = "time_utc,elapsed_s,label,raw_hex,raw,error" ] || fail "no CSV header"
grep -q ',0,s2,0x031f,799,$' "$F" || fail "first read not logged as 0x031f / 799"
grep -q ',2,s2,0x031e,798,$' "$F" || fail "third read not logged at elapsed 2"
[ "$(grep -c '^20' "$F")" = 3 ] || fail "expected 3 data lines"
grep -q '^# start label=s2 interval=1 reads=3 throttled=' "$F" || fail "no start comment"
grep -q '^# end label=s2 throttled=' "$F" || fail "no end comment"

# 2. A 0xffff read is kept as read and counted, and is not a failure by itself.
fake "0x02 0x91" "0xff 0xff" "0x02 0x92"
F=$WORK/ffff.csv
read_run --count 3 --interval 1 --label ff --out "$F"
[ "$RC" = 0 ] || fail "0xffff run exited $RC"
grep -q ',ff,0xffff,65535,$' "$F" || fail "0xffff read not logged as read"
contains "ffff=1 min=657 max=65535" || fail "0xffff run: summary"

# 3. A failed transfer and unexpected output are logged, never dropped; the exit status is 1.
fake "FAIL 1 boom, bad" "garbage here"
F=$WORK/bad.csv
read_run --count 2 --interval 1 --label bad --out "$F"
[ "$RC" = 1 ] || fail "failing reads: expected exit 1, got $RC"
contains "reads=2 failed=2 ffff=0 min=- max=-" || fail "failing reads: summary"
grep -q ',0,bad,,,rc=1 boom  bad$' "$F" || fail "failed transfer not logged (commas must go)"
grep -q ',1,bad,,,unexpected output: garbage here$' "$F" || fail "unexpected output not logged"

# 4. Without --out the CSV goes to stdout and the summary to stderr.
fake "0x03 0x00"
OUT=$(sh "$READ" --cmd "$FAKE/i2ctransfer" --count 1 --label so 2>"$WORK/so.err")
RC=$?
[ "$RC" = 0 ] || fail "stdout mode exited $RC"
contains ",so,0x0300,768," || fail "stdout mode: no CSV on stdout"
contains "soilread" && fail "stdout mode: summary leaked onto stdout"
grep -q 'soilread label=so reads=1' "$WORK/so.err" || fail "stdout mode: no summary on stderr"

# 5. --out appends, and the header is written only once.
fake "0x03 0x00"
F=$WORK/append.csv
read_run --count 1 --label a --out "$F"
read_run --count 1 --label b --out "$F"
[ "$(grep -c '^time_utc' "$F")" = 1 ] || fail "header repeated"
[ "$(grep -c '^20' "$F")" = 2 ] || fail "append: expected 2 data lines"

# 6. --duration is turned into ceil(duration / interval) reads.
fake "0x03 0x00"
read_run --duration 2 --interval 1 --label d
contains "reads=2 " || fail "--duration 2 --interval 1 should take 2 reads"

# 7. SIGTERM stops after the current read and still prints the summary.
fake "0x03 0x00"
F=$WORK/term.csv
sh "$READ" --cmd "$FAKE/i2ctransfer" --duration 60 --interval 1 --label term --out "$F" \
    >"$WORK/term.out" 2>&1 &
pid=$!
i=0
until grep -q ',term,0x' "$F" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -le 100 ] || fail "no read within 10 s"
    sleep 0.1
done
kill -TERM "$pid"
wait "$pid"
RC=$?
OUT=$(cat "$WORK/term.out")
[ "$RC" = 0 ] || fail "SIGTERM: expected exit 0, got $RC"
contains "stopped_early=1" || fail "SIGTERM: no stopped_early in the summary"
grep -q '^# end label=term' "$F" || fail "SIGTERM: no end comment"

# 8. The throttled word from vcgencmd goes into the start and end comments.
mkdir -p "$WORK/bin"
printf '#!/bin/sh\necho "throttled=0x50005"\n' >"$WORK/bin/vcgencmd"
chmod +x "$WORK/bin/vcgencmd"
fake "0x03 0x00"
F=$WORK/thr.csv
OUT=$(PATH="$WORK/bin:$PATH" sh "$READ" --cmd "$FAKE/i2ctransfer" --count 1 --label thr --out "$F" 2>&1)
grep -q '^# start .*throttled=0x50005' "$F" || fail "vcgencmd value missing from the start comment"
grep -q '^# end .*throttled=0x50005' "$F" || fail "vcgencmd value missing from the end comment"

# 9. Bad arguments and a missing i2ctransfer exit 2.
bad_read() {
    read_run "$@"
    [ "$RC" = 2 ] || fail "soilread $*: expected exit 2, got $RC"
}
fake "0x03 0x00"
bad_read
bad_read --count 3 --duration 10
bad_read --count abc
bad_read --count 0
bad_read --count 1 --interval 0
bad_read --count 1 --label a,b
bad_read --count 1 --bogus
bad_read --count
bad_read --count 1 --out "$WORK/no/such/dir/x.csv"
OUT=$(sh "$READ" --count 1 --cmd "$WORK/nowhere" 2>&1)
RC=$?
[ "$RC" = 2 ] || fail "missing i2ctransfer: expected exit 2, got $RC"

# ------------------------------------------------------------- soilstats.sh

stats() {
    OUT=$(sh "$STATS" "$@" 2>&1)
    RC=$?
}

# expect_pass NAME: the last stats run printed PASS and exited 0.
expect_pass() {
    [ "$RC" = 0 ] || fail "$1: expected exit 0, got $RC"
    contains PASS || fail "$1: no PASS"
}

# expect_fail NAME TEXT: exit 1 and a FAIL line holding TEXT.
expect_fail() {
    [ "$RC" = 1 ] || fail "$1: expected exit 1, got $RC"
    contains "FAIL $2" || fail "$1: no 'FAIL $2' line"
}

expect_usage() {
    [ "$RC" = 2 ] || fail "$1: expected exit 2, got $RC"
}

# csv FILE: a soilread.sh header.  row FILE T LABEL RAW: one read; RAW empty is a failed read.
csv() {
    echo "time_utc,elapsed_s,label,raw_hex,raw,error" >"$1"
}
row() {
    if [ -n "$4" ]; then
        echo "2026-01-01T00:00:00Z,$2,$3,0x0000,$4," >>"$1"
    else
        echo "2026-01-01T00:00:00Z,$2,$3,,,rc=1 boom" >>"$1"
    fi
}

# S2 noise.  Range 565..977 is 412 counts, so 1% is 4.12 counts.
# calm: every group of 5 has the median 800 (single reads vary by a few counts).
csv "$WORK/calm.csv"
i=0
while [ "$i" -lt 100 ]; do
    set -- 800 804 796 802 798
    shift $((i % 5))
    row "$WORK/calm.csv" "$i" s2 "$1"
    i=$((i + 1))
done
stats noise "$WORK/calm.csv" --range 565 977
expect_pass "noise: calm"
contains "median of 5: groups=20 stdev=0.00" || fail "noise: calm numbers"

# jumpy: the group medians alternate 800 and 830, so their stdev is about 15 (3.7%).
csv "$WORK/jumpy.csv"
i=0
while [ "$i" -lt 100 ]; do
    if [ $((i / 5 % 2)) = 0 ]; then v=800; else v=830; fi
    row "$WORK/jumpy.csv" "$i" s2 "$v"
    i=$((i + 1))
done
stats noise "$WORK/jumpy.csv" --range 565 977
expect_fail "noise: jumpy" "median-of-5 stdev is 3.7"

csv "$WORK/few.csv"
i=0
while [ "$i" -lt 7 ]; do
    row "$WORK/few.csv" "$i" s2 800
    i=$((i + 1))
done
stats noise "$WORK/few.csv" --range 565 977
expect_fail "noise: few reads" "not enough reads (7)"

# One 0xffff read among 99 steady ones: reported, included, but the group median ignores it.
csv "$WORK/ffff.csv"
i=0
while [ "$i" -lt 99 ]; do
    row "$WORK/ffff.csv" "$i" s2 800
    i=$((i + 1))
done
row "$WORK/ffff.csv" 99 s2 65535
stats noise "$WORK/ffff.csv" --range 565 977
contains "ffff=1" || fail "noise: ffff not counted"
contains "NOTE 1 read(s) were 0xffff and are included" || fail "noise: ffff not reported"

# Comments, the header and failed reads are skipped; failed reads are counted.
cp "$WORK/calm.csv" "$WORK/mixed.csv"
echo "# start label=s2 interval=1 reads=100 throttled=0x0" >>"$WORK/mixed.csv"
row "$WORK/mixed.csv" 100 s2 ""
row "$WORK/mixed.csv" 101 s2 ""
row "$WORK/mixed.csv" 102 s2 ""
stats noise "$WORK/mixed.csv" --range 565 977
expect_pass "noise: mixed"
contains "noise reads=100 errors=3 ffff=0" || fail "noise: errors not counted"

# --label picks one capture out of a file that holds two.
csv "$WORK/two.csv"
i=0
while [ "$i" -lt 100 ]; do
    set -- 800 804 796 802 798
    shift $((i % 5))
    row "$WORK/two.csv" "$i" good "$1"
    if [ $((i / 5 % 2)) = 0 ]; then v=800; else v=830; fi
    row "$WORK/two.csv" "$i" noisy "$v"
    i=$((i + 1))
done
stats noise "$WORK/two.csv" --range 565 977 --label good
expect_pass "noise: --label good"
stats noise "$WORK/two.csv" --range 565 977 --label noisy
expect_fail "noise: --label noisy" "median-of-5"

cp "$WORK/calm.csv" "$WORK/malformed.csv"
echo "this is not a read" >>"$WORK/malformed.csv"
stats noise "$WORK/malformed.csv" --range 565 977
expect_fail "noise: malformed" "1 malformed line(s)"

# S3 repeat.  Range 565..965 is 400 counts, so 5% is exactly 20.
spread_file() {
    csv "$1"
    i=0
    while [ "$i" -lt 10 ]; do
        v=800
        [ "$i" != 9 ] || v=$((800 + $2))
        row "$1" "$i" "insert-$i" "$v"
        i=$((i + 1))
    done
}
spread_file "$WORK/rep19.csv" 19
stats repeat "$WORK/rep19.csv" --range 565 965
expect_pass "repeat: spread 19 of 400"
spread_file "$WORK/rep20.csv" 20
stats repeat "$WORK/rep20.csv" --range 565 965
expect_fail "repeat: spread exactly 5%" "spread is 5.00%"
csv "$WORK/rep1.csv"
row "$WORK/rep1.csv" 0 insert-0 800
stats repeat "$WORK/rep1.csv" --range 565 965
expect_fail "repeat: one read" "not enough reads (1)"

# S4 response.  40 reads, one a second: 565 until second S, then 700 for D seconds, then 977.
# The step is seen at second S-1 and the last read outside 5% is second S+D-1, so the
# settle time is D+1 seconds.
resp_file() {
    csv "$1"
    t=0
    while [ "$t" -lt 40 ]; do
        if [ "$t" -lt "$2" ]; then v=565; elif [ "$t" -lt $(($2 + $3)) ]; then v=700; else v=977; fi
        row "$1" "$t" s4 "$v"
        t=$((t + 1))
    done
}
resp_file "$WORK/resp3.csv" 5 2
stats response "$WORK/resp3.csv"
expect_pass "response: 3 s"
contains "settle time 3s" || fail "response: settle time should be 3 s"
resp_file "$WORK/resp10.csv" 5 9
stats response "$WORK/resp10.csv"
expect_pass "response: exactly 10 s"
resp_file "$WORK/resp11.csv" 5 10
stats response "$WORK/resp11.csv"
expect_fail "response: 11 s" "settle time is 11 s"
resp_file "$WORK/resp_late.csv" 20 2
stats response "$WORK/resp_late.csv"
expect_pass "response: the capture started long before the step"
contains "settle time 3s" || fail "response: start time must not matter"

csv "$WORK/ramp.csv"
t=0
while [ "$t" -lt 40 ]; do
    row "$WORK/ramp.csv" "$t" s4 $((565 + 20 * t))
    t=$((t + 1))
done
stats response "$WORK/ramp.csv"
expect_fail "response: still rising" "never settled"
csv "$WORK/flat.csv"
t=0
while [ "$t" -lt 30 ]; do
    row "$WORK/flat.csv" "$t" s4 800
    t=$((t + 1))
done
stats response "$WORK/flat.csv"
expect_fail "response: no step" "no step found"

# S1 calib.  The dry pot weighs 1000 g.
# mkcalib FILE PAIR...: a calibration file with one "weight_g raw" line per PAIR.
mkcalib() {
    f=$1
    shift
    echo "# weight_g raw" >"$f"
    for pair in "$@"; do
        echo "$pair" >>"$f"
    done
}
mkcalib "$WORK/cal_ok.txt" "1050 600" "1100 700" "1150 800" "1200 900" "1250 1000"
stats calib "$WORK/cal_ok.txt" --dry-weight 1000
expect_pass "calib: linear"
contains "R^2 = 1.0000" || fail "calib: R^2"
contains "measured: dry=600 wet=1000" || fail "calib: measured range"
mkcalib "$WORK/cal_rev.txt" "1250 1000" "1200 900" "1150 800" "1100 700" "1050 600"
stats calib "$WORK/cal_rev.txt" --dry-weight 1000
expect_pass "calib: rows in any order"
mkcalib "$WORK/cal_dip.txt" "1050 600" "1100 700" "1150 650" "1200 900" "1250 1000"
stats calib "$WORK/cal_dip.txt" --dry-weight 1000
expect_fail "calib: dip" "raw does not rise with water at level 3"
mkcalib "$WORK/cal_curve.txt" "1050 600" "1100 605" "1150 610" "1200 615" "1250 1000"
stats calib "$WORK/cal_curve.txt" --dry-weight 1000
expect_fail "calib: monotonic but not linear" "R^2 is 0.53"
mkcalib "$WORK/cal_four.txt" "1050 600" "1100 700" "1150 800" "1200 900"
stats calib "$WORK/cal_four.txt" --dry-weight 1000
expect_fail "calib: four levels" "need 5 moisture levels, got 4"
mkcalib "$WORK/cal_same.txt" "1050 600" "1050 610" "1150 800" "1200 900" "1250 1000"
stats calib "$WORK/cal_same.txt" --dry-weight 1000
expect_fail "calib: same weight" "two levels have the same weight"
stats calib "$WORK/cal_ok.txt" --dry-weight 1100
expect_fail "calib: weight below the dry pot" "level 1 weighs less than the dry pot"
mkcalib "$WORK/cal_bad.txt" "1050 600" "1100 seven" "1150 800" "1200 900" "1250 1000"
stats calib "$WORK/cal_bad.txt" --dry-weight 1000
expect_fail "calib: malformed" "1 malformed line(s)"

# S5 drift.  Range 565..977 is 412 counts; 2% is 8.24 counts over 48 hours.
# drift_file FILE SLOPE_PER_HOUR HOURS: one read a minute, raw = 800 + slope * hours, rounded.
drift_file() {
    csv "$1"
    awk -v s="$2" -v hours="$3" 'BEGIN {
        for (t = 0; t < hours * 3600; t += 60)
            printf "2026-01-01T00:00:00Z,%d,s5,0x0000,%d,\n", t, int(800 + s * t / 3600 + 0.5)
    }' >>"$1"
}
drift_file "$WORK/flat48.csv" 0 48
stats drift "$WORK/flat48.csv" --range 565 977
expect_pass "drift: flat"
contains "drift: 48 hourly medians" || fail "drift: expected 48 hourly medians"
drift_file "$WORK/slow48.csv" 0.1 48
stats drift "$WORK/slow48.csv" --range 565 977
expect_pass "drift: 0.1 counts/hour"
drift_file "$WORK/fast48.csv" 0.25 48
stats drift "$WORK/fast48.csv" --range 565 977
expect_fail "drift: 0.25 counts/hour" "drift is 2."
drift_file "$WORK/short.csv" 0 10
stats drift "$WORK/short.csv" --range 565 977
expect_fail "drift: only 10 hours" "only 10 hourly medians"
cp "$WORK/flat48.csv" "$WORK/back.csv"
echo "2026-01-01T00:00:00Z,60,s5,0x0000,800," >>"$WORK/back.csv"
stats drift "$WORK/back.csv" --range 565 977
expect_fail "drift: time goes backwards" "elapsed_s goes backwards"

# Bad arguments for soilstats.sh exit 2.
stats
expect_usage "no arguments"
stats bogus "$WORK/calm.csv"
expect_usage "unknown subcommand"
stats noise
expect_usage "no file"
stats noise "$WORK/nowhere.csv" --range 565 977
expect_usage "missing file"
stats noise "$WORK/calm.csv"
expect_usage "noise without --range"
stats noise "$WORK/calm.csv" --range 565
expect_usage "--range with one value"
stats noise "$WORK/calm.csv" --range 977 565
expect_usage "--range with wet below dry"
stats noise "$WORK/calm.csv" --range abc 977
expect_usage "--range with text"
stats noise "$WORK/calm.csv" --range 565 977 --bogus
expect_usage "unknown option"
stats calib "$WORK/cal_ok.txt"
expect_usage "calib without --dry-weight"
stats response "$WORK/calm.csv" --label
expect_usage "--label without a value"

echo PASS
