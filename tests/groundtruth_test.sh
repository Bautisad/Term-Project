#!/bin/sh
# Ground-truth join tests.  usage: groundtruth_test.sh <groundtruth.sh>
# Each case writes a synthetic gt-csv export ($WORK/store.csv) and a weights file, runs the tool,
# and checks the exit code and the exact output lines.  No store, no Pi, no clock: the records
# start at 2026-11-14T17:00:00Z (Unix 1794675600) and are placed by their offset in seconds.

GT=$1
WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT

BASE_MS=1794675600000
CSVHEAD='time_utc,unix_ms,seq,soil_raw,soil_valid,soil_stuck,light,light_valid,light_bright_ms,light_changes,clock_unset,undervoltage,synthetic'
WHEAD='utc_time,grams,finger'
SEQ=0

fail() {
    echo "FAIL: $*"
    echo "--- stdout:"
    cat "$WORK/out" 2>/dev/null
    echo "--- stderr:"
    cat "$WORK/err" 2>/dev/null
    exit 1
}

# newstore / newweights: start a fresh fixture file with its header.
newstore() {
    printf '%s\n' "$CSVHEAD" >"$WORK/store.csv"
    SEQ=0
}
newweights() {
    printf '%s\n' "$WHEAD" >"$WORK/weights.csv"
}

# rec OFFSET SOIL [valid stuck unset synthetic]: one record OFFSET seconds after BASE (under 7 h),
# the flags are yes or no and default to a clean reading.
rec() {
    off=$1
    soil=$2
    valid=${3:-yes}
    stuck=${4:-no}
    unset_=${5:-no}
    synth=${6:-no}
    SEQ=$((SEQ + 1))
    printf '2026-11-14T%02d:%02d:%02d.000Z,%s,%s,%s,%s,%s,dark,yes,0,0,%s,no,%s\n' \
        $((17 + off / 3600)) $((off % 3600 / 60)) $((off % 60)) \
        $((BASE_MS + off * 1000)) "$SEQ" "$soil" "$valid" "$stuck" "$unset_" "$synth" >>"$WORK/store.csv"
}

# wt OFFSET GRAMS [FINGER]: one weights line at OFFSET seconds after BASE.
wt() {
    printf '2026-11-14T%02d:%02d:%02dZ,%s,%s\n' \
        $((17 + $1 / 3600)) $(($1 % 3600 / 60)) $(($1 % 60)) "$2" "$3" >>"$WORK/weights.csv"
}

# wtn COUNT OFFSET GRAMS FINGER: COUNT identical weights lines.
wtn() {
    n=$1
    while [ "$n" -gt 0 ]; do
        wt "$2" "$3" "$4"
        n=$((n - 1))
    done
}

# run ARGS...: the tool over the fixture files plus ARGS.  stdout in $WORK/out, stderr in
# $WORK/err.  (The redirections are inside run on purpose: it is the only function that has them,
# and `make memcheck` runs this script under Valgrind.)
run() {
    sh "$GT" --csv "$WORK/store.csv" --weights "$WORK/weights.csv" "$@" >"$WORK/out" 2>"$WORK/err"
    RC=$?
}

# has LINE: stdout has a line that is exactly LINE.
has() {
    grep -qxF -- "$1" "$WORK/out"
}

expect_rc() {
    [ "$RC" = "$2" ] || fail "$1: expected exit $2, got $RC"
    return 0
}

expect_line() {
    has "$2" || fail "$1: no line: $2"
    return 0
}

expect_no_line() {
    has "$2" && fail "$1: unexpected line: $2"
    return 0
}

HEADER='utc_time,grams,finger,soil_raw,soil_time,gap_s,flag'

echo "a clean join: nearest usable record, gap in seconds, flag, summary"
newstore
rec 0 700
rec 600 640
rec 1200 420
rec 1800 520
newweights
wt 610 800 moist
wt 1200 640 dry
run
expect_rc clean 0
expect_line clean "$HEADER"
expect_line clean "2026-11-14T17:10:10Z,800,moist,640,2026-11-14T17:10:00.000Z,10,no"
expect_line clean "2026-11-14T17:20:00Z,640,dry,420,2026-11-14T17:20:00.000Z,0,yes"
expect_line clean "matched=2 unmatched=0"
expect_line clean "c4 checks=2 agree=2 pct=100.0 need=20 INCOMPLETE"

echo "the nearest record wins, a tie goes to the earlier one"
newstore
rec 0 700
rec 120 650
newweights
wt 50 1 dry
wt 60 2 dry
wt 70 3 dry
run
expect_rc nearest 0
expect_line nearest "2026-11-14T17:00:50Z,1,dry,700,2026-11-14T17:00:00.000Z,50,no"
expect_line nearest "2026-11-14T17:01:00Z,2,dry,700,2026-11-14T17:00:00.000Z,60,no"
expect_line nearest "2026-11-14T17:01:10Z,3,dry,650,2026-11-14T17:02:00.000Z,50,no"

echo "the flag follows the page: yes at or below the line, back to no only 48 above it"
newstore
rec 0 700
rec 600 440
rec 1200 480
rec 1800 498
rec 2400 451
rec 3000 450
newweights
wt 0 1
wt 600 2
wt 1200 3
wt 1800 4
wt 2400 5
wt 3000 6
run
expect_rc hysteresis 0
expect_line hysteresis "2026-11-14T17:00:00Z,1,,700,2026-11-14T17:00:00.000Z,0,no"
expect_line hysteresis "2026-11-14T17:10:00Z,2,,440,2026-11-14T17:10:00.000Z,0,yes"
expect_line hysteresis "2026-11-14T17:20:00Z,3,,480,2026-11-14T17:20:00.000Z,0,yes"
expect_line hysteresis "2026-11-14T17:30:00Z,4,,498,2026-11-14T17:30:00.000Z,0,no"
expect_line hysteresis "2026-11-14T17:40:00Z,5,,451,2026-11-14T17:40:00.000Z,0,no"
expect_line hysteresis "2026-11-14T17:50:00Z,6,,450,2026-11-14T17:50:00.000Z,0,yes"

echo "--dry and --band move the line"
run --dry 500 --band 10
expect_rc dry_option 0
expect_line dry_option "2026-11-14T17:00:00Z,1,,700,2026-11-14T17:00:00.000Z,0,no"
expect_line dry_option "2026-11-14T17:10:00Z,2,,440,2026-11-14T17:10:00.000Z,0,yes"
expect_line dry_option "2026-11-14T17:20:00Z,3,,480,2026-11-14T17:20:00.000Z,0,yes"
expect_line dry_option "2026-11-14T17:30:00Z,4,,498,2026-11-14T17:30:00.000Z,0,yes"
expect_line dry_option "2026-11-14T17:40:00Z,5,,451,2026-11-14T17:40:00.000Z,0,yes"
run --dry 500 --band 0
expect_rc band_zero 0
expect_line band_zero "2026-11-14T17:30:00Z,4,,498,2026-11-14T17:30:00.000Z,0,yes"

echo "a weight with no usable record inside the window is listed as unmatched, not dropped"
newstore
rec 0 700
rec 600 640
newweights
wt 1800 800 moist
wt 700 801 moist
run
expect_rc window 0
expect_line window "2026-11-14T17:30:00Z,800,moist,,,,unmatched"
expect_line window "2026-11-14T17:11:40Z,801,moist,640,2026-11-14T17:10:00.000Z,100,no"
expect_line window "matched=1 unmatched=1"
run --window 1200
expect_rc window_wide 0
expect_line window_wide "2026-11-14T17:30:00Z,800,moist,640,2026-11-14T17:10:00.000Z,1200,no"
expect_line window_wide "matched=2 unmatched=0"
run --window 100
expect_line window_edge "2026-11-14T17:11:40Z,801,moist,640,2026-11-14T17:10:00.000Z,100,no"
run --window 99
expect_line window_edge "2026-11-14T17:11:40Z,801,moist,,,,unmatched"

echo "invalid, stuck, clock-unset and synthetic records are never matched"
newstore
rec 0 700
rec 30 0 no
rec 40 300 yes yes
rec 50 310 yes no yes
rec 55 200 yes no no yes
newweights
wt 45 900
run
expect_rc skipped 0
expect_line skipped "2026-11-14T17:00:45Z,900,,700,2026-11-14T17:00:00.000Z,45,no"
newstore
rec 30 0 no
rec 55 200 yes no no yes
run
expect_line only_bad "2026-11-14T17:00:45Z,900,,,,,unmatched"
expect_line only_bad "matched=0 unmatched=1"

echo "skipped records do not move the flag"
newstore
rec 0 400
rec 100 900 no
rec 200 900 yes yes
rec 300 900 yes no yes
rec 400 900 yes no no yes
newweights
wt 250 1
run
expect_rc skipped_flag 0
expect_line skipped_flag "2026-11-14T17:04:10Z,1,,400,2026-11-14T17:00:00.000Z,250,yes"

echo "C4: 18 of 20 agree is 90.0 and passes"
newstore
rec 0 400
rec 3000 600
newweights
wtn 10 0 640 dry
wtn 8 3000 700 moist
wtn 2 3000 701 dry
run
expect_rc c4_pass 0
expect_line c4_pass "c4 checks=20 agree=18 pct=90.0 PASS"
expect_no_line c4_pass "c4 checks=20 agree=18 pct=90.0 INCOMPLETE"

echo "C4: 17 of 20 agree is 85.0 and fails"
newweights
wtn 10 0 640 dry
wtn 7 3000 700 wet
wtn 3 3000 701 dry
run
expect_rc c4_fail 0
expect_line c4_fail "c4 checks=20 agree=17 pct=85.0 FAIL"

echo "C4: fewer than 20 checks is incomplete, however good; blank fingers and unmatched weights are not checks"
newweights
wtn 19 0 640 dry
wtn 5 3000 700
wtn 2 9000 5 dry
run
expect_rc c4_short 0
expect_line c4_short "c4 checks=19 agree=19 pct=100.0 need=20 INCOMPLETE"
expect_line c4_short "matched=24 unmatched=2"

echo "C4: no checks at all"
newweights
wt 0 640
run
expect_rc c4_none 0
expect_line c4_none "c4 checks=0 agree=0 pct=0.0 need=20 INCOMPLETE"

echo "an empty weights file (header only) is a result, not an error"
newweights
run
expect_rc empty 0
expect_line empty "matched=0 unmatched=0"

echo "--calib-out writes weight and raw per matched weight, in the order of the weights file"
newstore
rec 0 700
rec 600 640
rec 1200 420
newweights
wt 1200 640 dry
wt 0 800.5 moist
wt 9000 5 dry
run --calib-out "$WORK/calib.txt"
expect_rc calib 0
printf '640 420\n800.5 700\n' >"$WORK/calib.want"
cmp -s "$WORK/calib.txt" "$WORK/calib.want" || fail "calib: wrong --calib-out file: $(cat "$WORK/calib.txt")"

echo "a CRLF weights file works"
printf '%s\r\n2026-11-14T17:00:00Z,800,moist\r\n' "$WHEAD" >"$WORK/weights.csv"
run
expect_rc crlf 0
expect_line crlf "2026-11-14T17:00:00Z,800,moist,700,2026-11-14T17:00:00.000Z,0,no"

echo "the inputs are never modified"
cp "$WORK/store.csv" "$WORK/store.before"
cp "$WORK/weights.csv" "$WORK/weights.before"
run
cmp -s "$WORK/store.csv" "$WORK/store.before" || fail "readonly: store file changed"
cmp -s "$WORK/weights.csv" "$WORK/weights.before" || fail "readonly: weights file changed"

echo "--dir runs gt-csv on the directory (a stand-in tool here) and uses what it prints"
cat >"$WORK/fakecsv" <<EOF
#!/bin/sh
echo "\$@" >"$WORK/fakecsv.args"
cat "$WORK/store.csv"
EOF
chmod +x "$WORK/fakecsv"
newweights
wt 0 800 moist
sh "$GT" --dir /some/where --csv-tool "$WORK/fakecsv" --weights "$WORK/weights.csv" >"$WORK/out" 2>"$WORK/err"
RC=$?
expect_rc dir 0
expect_line dir "2026-11-14T17:00:00Z,800,moist,700,2026-11-14T17:00:00.000Z,0,no"
[ "$(cat "$WORK/fakecsv.args")" = "--dir /some/where" ] || fail "dir: gt-csv called with '$(cat "$WORK/fakecsv.args")'"
printf '#!/bin/sh\necho "gt-csv: store unreadable" >&2\nexit 1\n' >"$WORK/badcsv"
chmod +x "$WORK/badcsv"
sh "$GT" --dir /some/where --csv-tool "$WORK/badcsv" --weights "$WORK/weights.csv" >"$WORK/out" 2>"$WORK/err"
RC=$?
expect_rc dir_fails 1
grep -q 'gt-csv failed' "$WORK/err" || fail "dir_fails: no 'gt-csv failed' message"

echo "bad input exits 1 with the line number"
newstore
rec 0 700
for bad in '2026-13-14T17:00:00Z,800,moist' '2026-11-31T17:00:00Z,800,moist' '2026-11-14 17:00:00,800,moist' \
    '2026-11-14T25:00:00Z,800,moist' 'noon,800,moist' '2026-11-14T17:00:00Z,abc,moist' \
    '2026-11-14T17:00:00Z,-5,moist' '2026-11-14T17:00:00Z,,moist' '2026-11-14T17:00:00Z,800,maybe' \
    '2026-11-14T17:00:00Z,800'; do
    newweights
    printf '2026-11-14T17:00:00Z,800,dry\n%s\n' "$bad" >>"$WORK/weights.csv"
    run
    expect_rc "bad_weight($bad)" 1
    grep -q 'weights line 3' "$WORK/err" || fail "bad_weight($bad): no 'weights line 3' message"
done
printf '2026-02-29T17:00:00Z,800,moist\n' >"$WORK/leap.tail"
newweights
cat "$WORK/leap.tail" >>"$WORK/weights.csv"
run
expect_rc not_leap 1
printf '2028-02-29T17:00:00Z,800,moist\n' >"$WORK/leap.tail"
newweights
cat "$WORK/leap.tail" >>"$WORK/weights.csv"
run
expect_rc leap 0
echo 'time,g,f' >"$WORK/weights.csv"
run
expect_rc bad_wheader 1
grep -q 'weights line 1' "$WORK/err" || fail "bad_wheader: no 'weights line 1' message"
printf '%s\n' 'a,b,c' >"$WORK/store.csv"
newweights
run
expect_rc bad_csvheader 1
grep -q 'csv' "$WORK/err" || fail "bad_csvheader: no message about the csv"
newstore
rm -f "$WORK/weights.csv"
run
expect_rc no_weights 1
newweights
rm -f "$WORK/store.csv"
run
expect_rc no_csv 1

echo "bad arguments exit 2"
newstore
newweights
sh "$GT" >"$WORK/out" 2>"$WORK/err"; RC=$?;                                                        expect_rc no_args 2
sh "$GT" --csv "$WORK/store.csv" >"$WORK/out" 2>"$WORK/err"; RC=$?;                                expect_rc no_weights_arg 2
sh "$GT" --weights "$WORK/weights.csv" >"$WORK/out" 2>"$WORK/err"; RC=$?;                          expect_rc no_source 2
sh "$GT" --weights "$WORK/weights.csv" --csv "$WORK/store.csv" --dir x >"$WORK/out" 2>"$WORK/err"; RC=$?; expect_rc both_sources 2
run --bogus;                                                                                      expect_rc unknown_option 2
run --window abc;                                                                                 expect_rc bad_window 2
run --window -1;                                                                                  expect_rc neg_window 2
run --dry x;                                                                                      expect_rc bad_dry 2
run --dry 70000;                                                                                  expect_rc big_dry 2
run --band x;                                                                                     expect_rc bad_band 2
run --window;                                                                                     expect_rc missing_value 2
run extra;                                                                                        expect_rc extra_arg 2
sh "$GT" --help >"$WORK/out" 2>"$WORK/err"; RC=$?;                                                 expect_rc help 0
grep -q '^usage: groundtruth.sh' "$WORK/out" || fail "help: no usage line"

echo "PASS"
