#!/bin/sh
# Supervisor behaviour tests.  usage: supervisor_test.sh <gt-supervisor> <gt-fake-child>
# Each case runs the real supervisor against gt-fake-child and checks its event log.

SUP=$1
FAKE=$2
WORK=$(mktemp -d) || exit 1
LOG=$WORK/events.log
SUPPID=
# A sanitizer finding must not look like a normal exit status (0 clean, 1 nothing left).
export ASAN_OPTIONS=exitcode=99:detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:exitcode=99

cleanup() {
    [ -n "$SUPPID" ] && kill -KILL "$SUPPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    echo "--- events log:"
    cat "$LOG" 2>/dev/null
    exit 1
}

count() { grep -c "$1" "$LOG" 2>/dev/null; }

# wait_count PATTERN MIN SECONDS: wait until PATTERN appears at least MIN times.
wait_count() {
    i=0
    while [ "$(count "$1")" -lt "$2" ]; do
        i=$((i + 1))
        [ "$i" -gt $(($3 * 10)) ] && fail "timeout waiting for $2x '$1'"
        sleep 0.1
    done
}

# alive PID: running and not a zombie (the shell has not wait()ed for it yet).
alive() {
    st=$(sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | cut -c1)
    [ -n "$st" ] && [ "$st" != Z ]
}

# start_sup MODE [FLAGS]: run the supervisor, with FLAGS (split into words) for its options,
# and one fake child behaving as MODE.
# GT_VALGRIND (set by `make memcheck`) is a command prefix, so valgrind checks the
# supervisor and the child it execs instead of this shell.
start_sup() {
    GT_FAKE=$1 $GT_VALGRIND "$SUP" $2 "$FAKE" 2>"$LOG" &
    SUPPID=$!
}

# wait_exit SECONDS: wait for the supervisor to exit; its status goes in RC.
wait_exit() {
    i=0
    while alive "$SUPPID"; do
        i=$((i + 1))
        [ "$i" -gt $(($1 * 10)) ] && fail "supervisor still running after $1s"
        sleep 0.1
    done
    wait "$SUPPID"
    RC=$?
    SUPPID=
}

stop_sup() {
    kill -TERM "$SUPPID"
    wait_exit 8
    [ "$RC" -eq 0 ] || fail "SIGTERM shutdown exited $RC, wanted 0"
}

echo "a killed child is restarted, with growing backoff"
start_sup crash
wait_count "died signal=9" 1 5
wait_count "restart=1 delay=1s" 1 5
wait_count "restart=2 delay=2s" 1 8
stop_sup

echo "a config exit is not restarted; supervisor exits 1 when nothing is left"
start_sup exit:2
wait_exit 5
[ "$RC" -eq 1 ] || fail "exit status $RC, wanted 1"
[ "$(count 'no_restart')" -eq 1 ] || fail "missing no_restart"
[ "$(count 'restart=')" -eq 0 ] || fail "restarted a config exit"

echo "SIGTERM stops a running child cleanly"
start_sup run
wait_count "start child=" 1 3
wait_count "gt-fake-child: ready" 1 15 # SIGTERM before the child's handler exists kills it
stop_sup
[ "$(count 'exited code=0')" -eq 1 ] || fail "child did not exit 0 on SIGTERM"

echo "the child gets only fd 3; nothing else leaks across exec"
start_sup fdcheck
wait_exit 5
[ "$(count 'exited code=0')" -eq 1 ] || fail "fdcheck child did not exit 0"

HB="--soft 1 --hard 3 --grace 1" # heartbeat limits short enough for a test

echo "a silent child is warned, terminated, and restarted although it exits 0 on SIGTERM"
start_sup run "$HB"
wait_count "heartbeat_missed" 1 5
wait_count "heartbeat_timeout" 1 5
wait_count "start child=" 2 5
stop_sup

echo "a child that ignores SIGTERM is killed after the grace period, then restarted"
start_sup stubborn "$HB"
wait_count "killed signal=SIGKILL" 1 8
wait_count "start child=" 2 5
stop_sup

echo "a child that keeps sending heartbeats is left alone, and its state is logged"
start_sup beat "$HB"
wait_count "state=running" 1 5
sleep 5 # longer than the hard limit
[ "$(count 'heartbeat_')" -eq 0 ] || fail "a healthy child was reported silent"
[ "$(count 'restart=')" -eq 0 ] || fail "a healthy child was restarted"
stop_sup

echo "shutdown does not wait for a child that ignores SIGTERM"
start_sup stubborn "--soft 10 --hard 20 --grace 1" # long deadlines: only shutdown can send it
wait_count "gt-fake-child: ready" 1 15
stop_sup
[ "$(count 'killed signal=SIGKILL')" -eq 1 ] || fail "no SIGKILL at shutdown"

echo "a crash loop is logged once as DEGRADED, and the restart delay is held at the cap"
start_sup crash "--max 2 --window 60"
wait_count "restart=3 delay=60s" 1 10
[ "$(count 'crash_loop.*state=DEGRADED')" -eq 1 ] || fail "crash_loop not logged exactly once"
sleep 3 # the held-back child must not start again
[ "$(count 'start child=')" -eq 3 ] || fail "child started during the held-back delay"
stop_sup

echo "a status line is logged for each child on the --status interval"
start_sup beat "--status 1"
wait_count "status child=.* alive=1 .*rss_kb=[0-9][0-9]* fds=[0-9][0-9]* restarts=0 faults=0" 2 10
[ "$(count ' WARN \| ERROR ')" -eq 0 ] || fail "a healthy child's status gave a warning or error"
stop_sup

echo "the status line counts a crashing child's restarts and faults"
start_sup crash "--status 2"
wait_count "status child=.* alive=. .*restarts=[1-9][0-9]* faults=[1-9][0-9]*" 1 12
stop_sup

EV=$WORK/events.file

echo "--events appends the same lines as stderr, and its fd is not leaked by restarts"
start_sup crash "--events $EV"
wait_count "restart=1 delay=1s" 1 5
fds1=$(ls "/proc/$SUPPID/fd" | wc -l) # the child is dead here, so its control socket is closed
wait_count "restart=3 delay=4s" 1 10
fds3=$(ls "/proc/$SUPPID/fd" | wc -l)
stop_sup
[ "$fds1" -eq "$fds3" ] || fail "fd count went from $fds1 to $fds3 across restarts"
grep ' supervisor ' "$LOG" | cmp -s - "$EV" || fail "events file differs from the stderr lines"

echo "an unusable --events path is logged and the supervisor still restarts its child"
start_sup crash "--events $WORK/nodir/events.log"
wait_count "events open failed" 1 3
wait_count "restart=1 delay=1s" 1 5
stop_sup

PIDS=$WORK/children

echo "kill -9 of the supervisor orphans a child that ignores SIGTERM; the next supervisor kills it"
start_sup stubborn "--grace 1 --pids $PIDS"
wait_count "gt-fake-child: ready" 1 15
ORPHAN=$(sed -n 's/.*start child=gt-fake-child pid=//p' "$LOG")
kill -KILL "$SUPPID"
wait "$SUPPID" 2>/dev/null
SUPPID=
alive "$ORPHAN" || fail "the child died with the supervisor, so it was not an orphan"
start_sup stubborn "--grace 1 --pids $PIDS"
wait_count "start orphans_killed=1" 1 10
alive "$ORPHAN" && fail "orphan $ORPHAN is still running"
[ "$(count "orphan pid=$ORPHAN")" -eq 1 ] || fail "orphan pid not logged"
wait_count "gt-fake-child: ready" 1 15
stop_sup

echo "kill -9 of the supervisor sends SIGTERM to a child that handles it"
start_sup run
wait_count "gt-fake-child: ready" 1 15
ORPHAN=$(sed -n 's/.*start child=gt-fake-child pid=//p' "$LOG")
kill -KILL "$SUPPID"
wait "$SUPPID" 2>/dev/null
SUPPID=
i=0
while alive "$ORPHAN"; do
    i=$((i + 1))
    [ "$i" -gt 30 ] && fail "child $ORPHAN outlived the supervisor"
    sleep 0.1
done

echo "PASS"
