#!/bin/sh
# Soak report tests.  usage: soakreport_test.sh <soakreport.sh>
# Each case writes a synthetic journal (30 status lines per child, a minute apart, starting at
# 23:41 so the run crosses midnight, in the layout of `journalctl -o short-iso`) with one fault
# put in by gen(), and checks the exit code and the FAIL line.  No processes, no clock.

REPORT=$1
WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "FAIL: $*"
    echo "--- output:"
    echo "$OUT"
    exit 1
}

# gen [-v NAME=VALUE ...]: writes the journal to $WORK/journal (the redirection is inside the
# function: bash leaks a little each time a function is called with a redirection, and `make
# memcheck` runs this script under Valgrind).  The -v settings choose one fault:
#   leak=CHILD bump=KB          RSS of CHILD grows by KB at the last sample
#   fdchild=CHILD fdat=K        CHILD has one more open file from sample K on
#   skipchild=CHILD skipfrom=A skipto=B   no lines for CHILD at samples A..B
#   vanish=CHILD vanishafter=K  CHILD stops reporting after sample K
#   dead=CHILD                  CHILD is not alive at its last sample
#   restartchild=CHILD restartat=K   CHILD restarts at sample K (a line with alive=0, then a new
#                               process with different RSS and fds)
#   malformed=CHILD malat=K     CHILD's line K has no fds= field
gen() {
    awk "$@" '
    function iso(k,    t, day) {
        t = 23 * 60 + 40 + k
        day = 4 + int(t / 1440)
        t = t % 1440
        return sprintf("2026-11-%02dT%02d:%02d:00Z", day, int(t / 60), t % 60)
    }
    BEGIN {
        n = split("gt-hub gt-sensor-soil gt-sensor-light gt-interface", name, " ")
        split("1616 1184 1200 1760", rss0, " ")
        split("10 7 7 6", fds0, " ")
        for (k = 1; k <= 30; k++) {
            ts = iso(k)
            if (k == 1)   # lines the report must ignore
                printf "%s host gt-supervisor[812]: %s INFO supervisor start child=gt-hub pid=900\n", ts, ts
            if (k == 2 && !noerr)   # noerr=1: no ERROR line (a run with marks would count it)
                printf "%s host gt-hub[900]: %s ERROR hub listen_failed errno=2\n", ts, ts
            for (i = 1; i <= n; i++) {
                c = name[i]; rss = rss0[i]; fds = fds0[i]; alive = 1; restarts = 0
                if (c == skipchild && k >= skipfrom && k <= skipto) continue
                if (c == vanish && k > vanishafter) continue
                if (c == leak && k == 30) rss += bump
                if (c == fdchild && k >= fdat) fds += 1
                if (c == restartchild && k == restartat) { alive = 0; rss = -1; fds = -1; restarts = 1 }
                if (c == restartchild && k > restartat) { restarts = 1; rss -= 100; fds += 2 }
                if (c == dead && k == 30) { alive = 0; rss = -1; fds = -1 }
                line = sprintf("%s host gt-supervisor[812]: %s INFO supervisor status child=%s alive=%d state=running degraded=0 rss_kb=%d", ts, ts, c, alive, rss)
                if (!(c == malformed && k == malat)) line = line sprintf(" fds=%d", fds)
                printf "%s restarts=%d faults=%d\n", line, restarts, restarts
            }
        }
    }' >"$WORK/journal"
}

# run ARGS...: the report over $WORK/journal with the rehearsal-sized settings plus ARGS.
run() {
    OUT=$(sh "$REPORT" --journal "$WORK/journal" --hours 0.5 --interval 60 "$@" 2>&1)
    RC=$?
}

contains() {
    case $OUT in
    *"$1"*) return 0 ;;
    esac
    return 1
}

# pass_line: the output has a line that is exactly PASS (the scratch path in the header could
# hold any letters).
pass_line() {
    printf '%s\n' "$OUT" | grep -qx PASS
}

expect_pass() {
    [ "$RC" = 0 ] || fail "$1: expected exit 0, got $RC"
    pass_line || fail "$1: no PASS line"
    printf '%s\n' "$OUT" | grep -q '^FAIL ' && fail "$1: printed a FAIL line"
    return 0
}

# expect_fail NAME TEXT: exit 1, a FAIL line holding TEXT, and no PASS.
expect_fail() {
    [ "$RC" = 1 ] || fail "$1: expected exit 1, got $RC"
    contains "FAIL $2" || fail "$1: no 'FAIL $2' line"
    pass_line && fail "$1: printed PASS"
    return 0
}

expect_usage() {
    [ "$RC" = 2 ] || fail "$1: expected exit 2, got $RC"
    return 0
}

echo "a clean run passes, across midnight, with other log lines mixed in"
gen
run
expect_pass clean
contains "gt-hub 1616 1616 0 10 10 0 0 30" || fail "clean: wrong table row for gt-hub"
contains "gt-interface 1760 1760 0 6 6 0 0 30" || fail "clean: wrong table row for gt-interface"

echo "RSS growth: exactly 1 MB passes, one kB more fails"
gen -v leak=gt-hub -v bump=1024
run
expect_pass rss_edge
gen -v leak=gt-hub -v bump=1025
run
expect_fail rss_leak "gt-hub RSS grew 1025 kB"

echo "a changed open file count fails, unless it falls inside the warm-up"
gen -v fdchild=gt-sensor-soil -v fdat=15
run
expect_fail fd_change "gt-sensor-soil open files changed from 7 to 8 at 2026-11-04T23:55:00Z"
gen -v fdchild=gt-sensor-soil -v fdat=2
run                       # the change is at the first sample after the one warm-up sample
expect_pass fd_in_warmup
gen -v fdchild=gt-sensor-soil -v fdat=3
run
expect_fail fd_after_warmup "gt-sensor-soil open files changed from 7 to 8"
run --warmup 2            # now sample 2 is warm-up too, and the first counted sample already has 8
expect_pass fd_warmup_option

echo "a restart starts a new run: new RSS and fds are not growth, and the dead line is not a sample"
gen -v restartchild=gt-hub -v restartat=15
run
expect_pass restart
contains "gt-hub 1616 1516 0 10 12 1 1 29" || fail "restart: wrong table row for gt-hub"

echo "a missing hour is a gap"
gen -v skipchild=gt-interface -v skipfrom=10 -v skipto=12
run
expect_fail gap "gt-interface gap: no status line between 2026-11-04T23:49:00Z and 2026-11-04T23:53:00Z"

echo "a child that stops reporting is caught, and so is one that has no lines at all"
gen -v vanish=gt-sensor-light -v vanishafter=20
run
expect_fail vanish "gt-sensor-light stopped reporting at 2026-11-05T00:00:00Z"
grep -v "child=gt-interface" "$WORK/journal" >"$WORK/journal2"
mv "$WORK/journal2" "$WORK/journal"
run
expect_fail missing "no status lines for gt-interface"

echo "a child that is not alive at its last line fails"
gen -v dead=gt-sensor-soil
run
expect_fail dead "gt-sensor-soil was not alive at its last status line"

echo "too few lines for the run length fails"
gen
run --hours 2
expect_fail short "gt-hub has 30 status lines, wanted at least 119"

echo "a malformed status line fails"
gen -v malformed=gt-hub -v malat=10
run
expect_fail malformed "malformed status line"

echo "a journal with no status lines fails"
printf '2026-11-04T23:31:00Z INFO supervisor start child=gt-hub pid=1\n' >"$WORK/journal"
run
expect_fail empty "no status lines for gt-hub"

echo "the supervisor's own stderr (no journal prefix) is read too"
gen
sed 's/^[^ ]* host [^ ]* //' "$WORK/journal" >"$WORK/bare" && mv "$WORK/bare" "$WORK/journal"
run
expect_pass bare

echo "bad arguments exit 2"
gen
OUT=$(sh "$REPORT" 2>&1); RC=$?;                                          expect_usage no_args
OUT=$(sh "$REPORT" --journal "$WORK/none" 2>&1); RC=$?;                   expect_usage no_file
OUT=$(sh "$REPORT" --journal "$WORK/journal" --hours 0 2>&1); RC=$?;      expect_usage hours_zero
OUT=$(sh "$REPORT" --journal "$WORK/journal" --hours 1.2.3 2>&1); RC=$?;  expect_usage hours_junk
OUT=$(sh "$REPORT" --journal "$WORK/journal" --interval 0 2>&1); RC=$?;   expect_usage interval_zero
OUT=$(sh "$REPORT" --journal "$WORK/journal" --interval abc 2>&1); RC=$?; expect_usage interval_junk
OUT=$(sh "$REPORT" --journal "$WORK/journal" --warmup -1 2>&1); RC=$?;    expect_usage warmup_junk
OUT=$(sh "$REPORT" --journal "$WORK/journal" --bogus 1 2>&1); RC=$?;      expect_usage unknown
OUT=$(sh "$REPORT" --journal 2>&1); RC=$?;                                expect_usage no_value

# --- With --marks: planned injections, the orderly stop, the run length, the unit result and
# --- the store.  The status lines come from gen -v noerr=1; the events are appended by e().

# e ID TIME LEVEL COMPONENT TEXT: appends one line to the journal, unless ID is in $OMIT.
e() {
    case " $OMIT " in
    *" $1 "*) return 0 ;;
    esac
    printf '%s host gt-supervisor[812]: %s %s %s %s\n' "$2" "$2" "$3" "$4" "$5" >>"$WORK/journal"
}

# The events of a good rehearsal: a 3-minute soil unplug at 23:50, kill -9 of the soil process
# at 00:00 and of the light process at 00:05, an orderly stop at 00:10.
clean_events() {
    e rf       2026-11-04T23:50:20Z ERROR soil       "read_failed errno=5 attempt=3/3 seq=10"
    e deg      2026-11-04T23:50:21Z WARN  supervisor "child=gt-sensor-soil state=degraded"
    e giveup   2026-11-04T23:51:25Z ERROR soil       "giving_up failed_cycles=5"
    e exit1    2026-11-04T23:51:25Z ERROR supervisor "child=gt-sensor-soil pid=901 exited code=1"
    e restart1 2026-11-04T23:51:25Z INFO  supervisor "child=gt-sensor-soil restart=1 delay=1s"
    e rec      2026-11-04T23:53:40Z INFO  supervisor "child=gt-sensor-soil state=running"
    e killS    2026-11-05T00:00:01Z ERROR supervisor "child=gt-sensor-soil pid=902 died signal=9"
    e restS    2026-11-05T00:00:01Z INFO  supervisor "child=gt-sensor-soil restart=2 delay=1s"
    e runS     2026-11-05T00:00:04Z INFO  supervisor "child=gt-sensor-soil state=running"
    e killL    2026-11-05T00:05:01Z ERROR supervisor "child=gt-sensor-light pid=903 died signal=9"
    e restL    2026-11-05T00:05:01Z INFO  supervisor "child=gt-sensor-light restart=1 delay=1s"
    e runL     2026-11-05T00:06:05Z INFO  supervisor "child=gt-sensor-light state=running"
    e stopping 2026-11-05T00:10:00Z INFO  supervisor "stopping children=4"
    e exit_hub 2026-11-05T00:10:00Z INFO  supervisor "child=gt-hub pid=1 exited code=0"
    e exit_soi 2026-11-05T00:10:00Z INFO  supervisor "child=gt-sensor-soil pid=2 exited code=0"
    e exit_lig 2026-11-05T00:10:00Z INFO  supervisor "child=gt-sensor-light pid=3 exited code=0"
    e exit_int 2026-11-05T00:10:00Z INFO  supervisor "child=gt-interface pid=4 exited code=0"
    e stopped  2026-11-05T00:10:00Z INFO  supervisor "stopped"
}

# marks [START]: the marks for those events; the run is 30 minutes long, as --hours 0.5 says.
marks() {
    cat >"$WORK/marks" <<EOF
# a rehearsal
${1:-2026-11-04T23:40:00Z} start
2026-11-04T23:50:00Z soil_unplug_start
2026-11-04T23:53:00Z soil_unplug_end
2026-11-05T00:00:00Z kill_soil
2026-11-05T00:05:00Z kill_light
2026-11-05T00:10:00Z stop
EOF
}

# full [EXTRA ARGS]: a clean journal with the events, minus whatever $OMIT names.
full() {
    gen -v noerr=1
    clean_events
    marks
}

M="--marks $WORK/marks"

echo "with marks: a clean rehearsal passes and shows what each injection did"
OMIT=
full
run $M
expect_pass marks_clean
contains "read_failed +20 s, state=degraded +21 s, state=running +40 s after the replug, 1 restart(s) in the window" ||
    fail "marks_clean: no unplug evidence line"
contains "kill_soil 2026-11-05T00:00:00Z: died signal=9 +1 s, state=running +4 s" || fail "marks_clean: no kill_soil line"
contains "kill_light 2026-11-05T00:05:00Z: died signal=9 +1 s, state=running +65 s" || fail "marks_clean: no kill_light line"
contains "stopping x1, exited code=0 x4, stopped x1" || fail "marks_clean: no stop line"
contains "NOTE: the unit result was not checked" || fail "marks_clean: no unit NOTE"
contains "NOTE: the store was not checked" || fail "marks_clean: no store NOTE"

echo "an event outside every window is unplanned, even a restart of the right kind"
full
e extra1 2026-11-04T23:45:00Z ERROR supervisor "child=gt-hub pid=77 died signal=11"
e extra2 2026-11-04T23:45:00Z INFO  supervisor "child=gt-hub restart=1 delay=1s"
run $M
expect_fail unplanned_hub "unplanned event outside the marked windows: 2026-11-04T23:45:00Z ERROR supervisor child=gt-hub pid=77 died signal=11"
contains "child=gt-hub restart=1" || fail "unplanned_hub: the restart was not listed"

echo "a window belongs to its child: the light sensor dying during the soil unplug is unplanned"
full
e extra 2026-11-04T23:51:00Z ERROR supervisor "child=gt-sensor-light pid=88 died signal=9"
run $M
expect_fail unplanned_light "unplanned event outside the marked windows: 2026-11-04T23:51:00Z ERROR supervisor child=gt-sensor-light"

echo "an ERROR line from nobody (not tied to a child) is unplanned"
full
e extra 2026-11-04T23:45:00Z ERROR supervisor "waitpid failed errno=4"
run $M
expect_fail unplanned_bare "unplanned event outside the marked windows: 2026-11-04T23:45:00Z ERROR supervisor waitpid failed"

echo "an event after the stop mark is unplanned, except a clean exit"
full
e extra 2026-11-05T00:10:01Z ERROR hub "store_append failed errno=5"
run $M
expect_fail after_stop "unplanned event outside the marked windows: 2026-11-05T00:10:01Z ERROR hub"

echo "unplug: not logged, logged late, not degraded, no recovery"
OMIT="rf giveup"
full
OMIT=
run $M
expect_fail unplug_none "soil_unplug at 2026-11-04T23:50:00Z: no soil read_failed after it"
OMIT="rf"
full
OMIT=
e rf_late 2026-11-04T23:53:30Z ERROR soil "read_failed errno=5 attempt=3/3 seq=13"
run $M
expect_fail unplug_late "soil_unplug at 2026-11-04T23:50:00Z: first read_failed after 210 s (limit 150 s)"
OMIT="deg"
full
OMIT=
run $M
expect_fail unplug_nodeg "soil_unplug at 2026-11-04T23:50:00Z: no state=degraded for gt-sensor-soil"
OMIT="rec"
full
OMIT=
run $M
expect_fail unplug_norec "soil_unplug_end at 2026-11-04T23:53:00Z: no state=running for gt-sensor-soil within 180 s"
OMIT="rec"
full
OMIT=
e rec_late 2026-11-04T23:56:01Z INFO supervisor "child=gt-sensor-soil state=running"
run $M
expect_fail unplug_slowrec "soil_unplug_end at 2026-11-04T23:53:00Z: no state=running for gt-sensor-soil within 180 s"

echo "kill -9: not logged as died signal=9, a different signal, or slow to come back"
OMIT="killS"
full
OMIT=
run $M
expect_fail kill_nodied "kill_soil at 2026-11-05T00:00:00Z: no 'died signal=9' for gt-sensor-soil within 5 s"
OMIT="killL"
full
OMIT=
e killL_term 2026-11-05T00:05:01Z ERROR supervisor "child=gt-sensor-light pid=903 died signal=15"
run $M
expect_fail kill_wrongsig "kill_light at 2026-11-05T00:05:00Z: no 'died signal=9' for gt-sensor-light within 5 s"
OMIT="runL"
full
OMIT=
e runL_late 2026-11-05T00:07:30Z INFO supervisor "child=gt-sensor-light state=running"
run $M
expect_fail kill_slowrun "kill_light at 2026-11-05T00:05:00Z: no state=running for gt-sensor-light within 120 s"

echo "the stop must be orderly: one stopping, four exits with code 0, one stopped"
OMIT="exit_hub"
full
OMIT=
run $M
expect_fail stop_noexit "stop at 2026-11-05T00:10:00Z: gt-hub did not exit with code 0"
OMIT="exit_int"
full
OMIT=
e exit_int_bad 2026-11-05T00:10:00Z ERROR supervisor "child=gt-interface pid=4 exited code=1"
run $M
expect_fail stop_badexit "stop at 2026-11-05T00:10:00Z: gt-interface did not exit with code 0"
OMIT="stopped"
full
OMIT=
run $M
expect_fail stop_nostopped "stop at 2026-11-05T00:10:00Z: expected 1 'stopped' line, found 0"
OMIT="stopping"
full
OMIT=
run $M
expect_fail stop_nostopping "stop at 2026-11-05T00:10:00Z: expected 1 'stopping' line, found 0"

echo "the run must last --hours between the start and stop marks"
full
marks 2026-11-04T23:50:00Z
run $M
expect_fail short_run "run lasted 0.33 h from the start mark to the stop mark, wanted 0.5 h"

echo "a marks file with no stop mark, or no start mark, fails the run"
full
grep -v ' stop$' "$WORK/marks" >"$WORK/marks2" && mv "$WORK/marks2" "$WORK/marks"
run $M
expect_fail no_stop_mark "marks: no stop mark"
full
grep -v ' start$' "$WORK/marks" >"$WORK/marks2" && mv "$WORK/marks2" "$WORK/marks"
run $M
expect_fail no_start_mark "marks: no start mark"

echo "a bad marks file is a usage error, exit 2"
full
printf '2026-11-04T23:40:00Z start\n2026-11-04T23:50:00Z shrug\n' >"$WORK/marks"
run $M
expect_usage marks_label
printf '2026-11-04 23:40:00 start\n' >"$WORK/marks"
run $M
expect_usage marks_time
printf '2026-11-04T23:40:00Z start\n2026-11-04T23:50:00Z soil_unplug_start\n' >"$WORK/marks"
run $M
expect_usage marks_unplug_open
printf '2026-11-04T23:40:00Z start\n2026-11-04T23:50:00Z soil_unplug_end\n' >"$WORK/marks"
run $M
expect_usage marks_unplug_end_alone
printf '2026-11-04T23:40:00Z start\n2026-11-04T23:41:00Z start\n' >"$WORK/marks"
run $M
expect_usage marks_two_starts
printf '2026-11-04T23:50:00Z start\n2026-11-04T23:40:00Z stop\n' >"$WORK/marks"
run $M
expect_usage marks_order
run --marks "$WORK/no_such_marks"
expect_usage marks_missing

echo "the unit result: success and 0 pass; anything else, or no such lines, fails"
full
printf 'Result=success\nExecMainStatus=0\n' >"$WORK/unit"
run $M --unit-result "$WORK/unit"
expect_pass unit_ok
contains "unit: Result=success ExecMainStatus=0" || fail "unit_ok: no unit line"
contains "NOTE: the unit result was not checked" && fail "unit_ok: still has the unit NOTE"
printf 'Result=exit-code\nExecMainStatus=1\n' >"$WORK/unit"
run $M --unit-result "$WORK/unit"
expect_fail unit_bad "unit: Result=exit-code ExecMainStatus=1 (wanted success and 0)"
printf 'Result=success\nExecMainStatus=2\n' >"$WORK/unit"
run $M --unit-result "$WORK/unit"
expect_fail unit_status "unit: Result=success ExecMainStatus=2 (wanted success and 0)"
: >"$WORK/unit"
run $M --unit-result "$WORK/unit"
expect_fail unit_empty "unit: no Result= or ExecMainStatus= line in"
run $M --unit-result "$WORK/no_such_unit"
expect_usage unit_missing

echo "the store: judged by the checker, hours*60 records, no lost or synthetic records"
mkdir "$WORK/store"
# fake_checker NAME EXIT BODY: a stand-in gt-logcheck that prints BODY and exits EXIT.
fake_checker() {
    printf '#!/bin/sh\n%s\nexit %s\n' "$3" "$2" >"$WORK/$1"
    chmod +x "$WORK/$1"
}
OK_LINE='echo "logcheck records=30 first_seq=0 last_seq=29 lost=0 corrupt=0 synthetic=0"; echo PASS'
fake_checker ck_ok 0 "$OK_LINE"
fake_checker ck_few 0 'echo "logcheck records=29 first_seq=0 last_seq=28 lost=0 corrupt=0 synthetic=0"; echo PASS'
fake_checker ck_lost 1 'echo "logcheck records=30 first_seq=0 last_seq=31 lost=2 corrupt=0 synthetic=0"; echo "FAIL gap in seq"'
fake_checker ck_synth 0 'echo "logcheck records=30 first_seq=0 last_seq=29 lost=0 corrupt=0 synthetic=5"; echo PASS'
fake_checker ck_rc 1 'echo "logcheck records=30 first_seq=0 last_seq=29 lost=0 corrupt=3 synthetic=0"; echo "FAIL 3 records fail their CRC"'
fake_checker ck_nosummary 1 'echo "checker confused"'
full
run $M --store "$WORK/store" --checker "$WORK/ck_ok"
expect_pass store_ok
contains "store: 30 records, lost=0, corrupt=0, synthetic=0" || fail "store_ok: no store line"
contains "NOTE: the store was not checked" && fail "store_ok: still has the store NOTE"
run $M --store "$WORK/store" --checker "$WORK/ck_few"
expect_fail store_few "store: 29 records, wanted at least 30 for 0.5 h"
run $M --store "$WORK/store" --checker "$WORK/ck_lost"
expect_fail store_lost "store: 2 records lost (a gap in the sequence numbers)"
expect_fail store_lost_rc "store: checker exit 1: FAIL gap in seq"
run $M --store "$WORK/store" --checker "$WORK/ck_synth"
expect_fail store_synth "store: 5 synthetic records in a real run"
run $M --store "$WORK/store" --checker "$WORK/ck_rc"
expect_fail store_rc "store: checker exit 1: FAIL 3 records fail their CRC"
run $M --store "$WORK/store" --checker "$WORK/ck_nosummary"
expect_fail store_nosummary "store: no record count from the checker"
run $M --store "$WORK/store" --checker "$WORK/no_such_checker"
expect_usage store_nochecker
run $M --store "$WORK/no_such_store" --checker "$WORK/ck_ok"
expect_usage store_nodir

echo "without --marks the events are ignored: only the status lines are judged"
gen
clean_events
run
expect_pass no_marks_ignores_events

# --- soakmark.sh: the tool that writes the marks file ---
MARK=$(dirname "$REPORT")/soakmark.sh
MF=$WORK/mark_file
rm -f "$MF"

# mark LABEL: runs soakmark.sh on $MF; its output is in OUT and its exit code in RC.
mark() {
    OUT=$(sh "$MARK" "$MF" "$1" 2>&1)
    RC=$?
}

# lines N: the marks file has N lines.
lines() {
    [ "$(wc -l <"$MF" | tr -d ' ')" = "$1" ] || fail "$2: the marks file has $(wc -l <"$MF" | tr -d ' ') lines, wanted $1"
}

echo "soakmark writes one UTC line per label, in order, and prints it"
mark start
[ "$RC" = 0 ] || fail "mark_start: exit $RC"
case $OUT in
20[2-9][0-9]-[01][0-9]-[0-3][0-9]T[0-2][0-9]:[0-5][0-9]:[0-6][0-9]Z\ start) ;;
*) fail "mark_start: the line is not '<UTC time>Z start'" ;;
esac
[ "$(cat "$MF")" = "$OUT" ] || fail "mark_start: the file does not hold the printed line"
for label in soil_unplug_start soil_unplug_end kill_soil kill_light stop; do
    mark $label
    [ "$RC" = 0 ] || fail "mark_$label: exit $RC"
done
lines 6 mark_all
[ "$(awk '{ print $2 }' "$MF" | tr '\n' ' ')" = "start soil_unplug_start soil_unplug_end kill_soil kill_light stop " ] ||
    fail "mark_all: the labels are not in the order they were written"

echo "soakmark refuses what the report would reject, and writes nothing"
mark start
[ "$RC" = 1 ] || fail "second start: exit $RC, wanted 1"
mark stop
[ "$RC" = 1 ] || fail "second stop: exit $RC, wanted 1"
lines 6 refused_repeat
rm -f "$MF"
mark soil_unplug_end
[ "$RC" = 1 ] || fail "unplug_end alone: exit $RC, wanted 1"
[ ! -e "$MF" ] || fail "unplug_end alone: wrote a file"
mark soil_unplug_start
[ "$RC" = 0 ] || fail "unplug_start: exit $RC"
mark soil_unplug_start
[ "$RC" = 1 ] || fail "unplug_start twice: exit $RC, wanted 1"
lines 1 refused_open
mark soil_unplug_end
[ "$RC" = 0 ] || fail "unplug_end: exit $RC"
mark soil_unplug_start
[ "$RC" = 0 ] || fail "a second unplug: exit $RC"
mark soil_unplug_end
mark kill_soil
mark kill_soil
[ "$RC" = 0 ] || fail "kills may repeat: exit $RC"
lines 6 refused_pairs
OUT=$(sh "$MARK" "$WORK/no_such_dir/marks" kill_soil 2>&1); RC=$?
[ "$RC" = 1 ] || fail "unwritable file: exit $RC, wanted 1"

echo "soakmark: bad arguments exit 2"
OUT=$(sh "$MARK" 2>&1); RC=$?;                      expect_usage mark_no_args
OUT=$(sh "$MARK" "$MF" 2>&1); RC=$?;                expect_usage mark_no_label
OUT=$(sh "$MARK" "$MF" shrug 2>&1); RC=$?;          expect_usage mark_bad_label
OUT=$(sh "$MARK" "$MF" start stop 2>&1); RC=$?;     expect_usage mark_extra

echo "what soakmark writes is a marks file the report accepts (it is not a usage error)"
rm -f "$MF"
for label in start soil_unplug_start soil_unplug_end kill_soil kill_light stop; do
    mark $label
done
gen -v noerr=1
run --marks "$MF"
[ "$RC" != 2 ] || fail "report_reads_marks: the report rejected the marks file"

echo "PASS"
