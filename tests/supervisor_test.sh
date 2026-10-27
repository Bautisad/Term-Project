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

# start_sup MODE: run the supervisor with one fake child behaving as MODE.
# GT_VALGRIND (set by `make memcheck`) is a command prefix, so valgrind checks the
# supervisor and the child it execs instead of this shell.
start_sup() {
    GT_FAKE=$1 $GT_VALGRIND "$SUP" "$FAKE" 2>"$LOG" &
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
stop_sup
[ "$(count 'exited code=0')" -eq 1 ] || fail "child did not exit 0 on SIGTERM"

echo "the child gets only fd 3; nothing else leaks across exec"
start_sup fdcheck
wait_exit 5
[ "$(count 'exited code=0')" -eq 1 ] || fail "fdcheck child did not exit 0"

echo "PASS"
