#!/bin/sh
# Soak check tests.  usage: soakcheck_test.sh <soakcheck.sh> <dir with gt-feeder and gt-logcheck>
# Each case runs the real script for a few seconds.  The failing cases swap in a fake feeder
# (a wrapper around the real one that misbehaves) or a fake checker.

SOAK=$1
BIN=$(cd "$2" && pwd) || exit 1
WORK=$(mktemp -d) || exit 1
export ASAN_OPTIONS=exitcode=99:detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:exitcode=99

trap 'rm -rf "$WORK"' EXIT

REAL=$BIN/gt-feeder
CHECKER=$BIN/gt-logcheck

# A checker that always fails (a path: command -v would return the shell builtin).
FALSE=/bin/false
[ -x "$FALSE" ] || FALSE=/usr/bin/false

fail() {
    echo "FAIL: $*"
    echo "--- output:"
    echo "$OUT"
    exit 1
}

# run ARGS...: runs the soak check; its output is in OUT and its exit code in RC.
run() {
    OUT=$(sh "$SOAK" "$@" 2>&1)
    RC=$?
}

contains() {
    case $OUT in
    *"$1"*) return 0 ;;
    esac
    return 1
}

# expect_fail NAME TEXT: the last run failed with exit 1 and a FAIL line holding TEXT.
expect_fail() {
    [ "$RC" = 1 ] || fail "$1: expected exit 1, got $RC"
    contains "FAIL $2" || fail "$1: no 'FAIL $2' line"
    contains PASS && fail "$1: printed PASS"
    return 0
}

# fake NAME BODY: writes $WORK/NAME.sh, a feeder that starts the real one in the background
# (so the store fills as usual), then runs BODY.  SIGTERM stops the real feeder and exits 0.
fake() {
    cat >"$WORK/$1.sh" <<EOF
#!/bin/sh
"$REAL" "\$@" &
c=\$!
trap 'kill -TERM \$c 2>/dev/null; wait \$c; exit 0' TERM
$2
EOF
    chmod +x "$WORK/$1.sh"
}

# 1. A short run passes, shows the table, and keeps its samples where we said.
run "$BIN/.." --duration 4 --sample 1 --feeder "$REAL" --checker "$CHECKER" --work "$WORK/w1"
[ "$RC" = 0 ] || fail "smoke run exited $RC"
contains "process rss_first_kb rss_last_kb rss_growth_kb fds_min fds_max" || fail "no table"
contains "feeder " || fail "no feeder row in the table"
contains PASS || fail "smoke run did not say PASS"
[ "$(wc -l <"$WORK/w1/samples.log")" = 4 ] || fail "samples.log should have 4 lines"
[ -s "$WORK/w1/writer.log" ] || fail "writer.log is empty"
[ -d "$WORK/w1" ] || fail "a --work directory must not be deleted"

# 2. The programs are found under BUILD_DIR/tools, and the scratch directory is removed
# after a passing run when --work was not given.
mkdir -p "$WORK/b/tools"
ln -s "$REAL" "$WORK/b/tools/gt-feeder"
ln -s "$CHECKER" "$WORK/b/tools/gt-logcheck"
run "$WORK/b" --duration 2 --sample 1
[ "$RC" = 0 ] || fail "BUILD_DIR run exited $RC"
tmpdir=$(echo "$OUT" | sed -n '1s/.* work=//p')
[ -n "$tmpdir" ] && [ ! -e "$tmpdir" ] || fail "scratch directory $tmpdir was not removed"

# 3. A feeder that dies mid-run is an unplanned restart.
cat >"$WORK/dies.sh" <<'EOF'
#!/bin/sh
sleep 1.5
exit 1
EOF
chmod +x "$WORK/dies.sh"
run --duration 4 --sample 1 --feeder "$WORK/dies.sh" --checker "$CHECKER" --work "$WORK/w3"
expect_fail "dying feeder" "feeder: exited"
[ -d "$WORK/w3" ] || fail "dying feeder: the work directory should be kept"

# 4. An open file count that keeps growing is caught.
fake leaks 'n=3
while :; do
    [ "$n" -le 9 ] && eval "exec $n</dev/null"
    n=$((n + 1))
    sleep 1 & wait $!
done'
run --duration 4 --sample 1 --feeder "$WORK/leaks.sh" --checker "$CHECKER" --work "$WORK/w4"
expect_fail "fd leak" "feeder: open file count changed"

# 5. Memory growth over 1 MB is caught.
fake grows 's=$(head -c 65536 /dev/zero | tr "\\0" x)
while :; do
    [ "${#s}" -lt 8000000 ] && s="$s$s"
    sleep 0.25 & wait $!
done'
run --duration 4 --sample 1 --feeder "$WORK/grows.sh" --checker "$CHECKER" --work "$WORK/w5"
expect_fail "RSS growth" "feeder: RSS grew"

# 6. A checker that reports a lost record is a failure here, even when it exits 0: a soak
# has no kills, so every gap is unexplained.
cat >"$WORK/gap.sh" <<'EOF'
#!/bin/sh
echo "logcheck records=1000 first_seq=1 last_seq=1001 lost=1 corrupt=0 recovered=1 max_dropped=0"
echo PASS
EOF
chmod +x "$WORK/gap.sh"
run --duration 2 --sample 1 --checker "$WORK/gap.sh" --feeder "$REAL" --work "$WORK/w6"
expect_fail "sequence gap" "checker: 1 records lost"

# 7. A checker that rejects the store is a failure.
run --duration 2 --sample 1 --feeder "$REAL" --checker "$FALSE" --work "$WORK/w7"
expect_fail "failing checker" "checker: exit 1"

# 8. A feeder that writes far fewer records than its rate promises is a failure.
cat >"$WORK/slow.sh" <<EOF
#!/bin/sh
exec "$REAL" --dir "\$2" --rate 1
EOF
chmod +x "$WORK/slow.sh"
run --duration 2 --sample 1 --rate 20 --feeder "$WORK/slow.sh" --checker "$CHECKER" --work "$WORK/w8"
expect_fail "too few records" "records:"

# 9. An ERROR line in the feeder's log is a failure.
fake noisy 'echo "2026-01-01T00:00:00Z ERROR feeder boom" >&2
while :; do sleep 1 & wait $!; done'
run --duration 2 --sample 1 --feeder "$WORK/noisy.sh" --checker "$CHECKER" --work "$WORK/w9"
expect_fail "ERROR line" "feeder: 1 ERROR lines"

# 10. Bad arguments and missing binaries exit 2.
bad() {
    run "$@"
    [ "$RC" = 2 ] || fail "$*: expected exit 2, got $RC"
}
bad --nonsense
bad --duration abc
bad --duration 10 --sample 6
bad --sample 0
bad --rate 0
bad --duration
bad "$WORK/nowhere" --duration 2 --sample 1
bad --feeder "$WORK/nowhere" --checker "$CHECKER" --duration 2 --sample 1
bad --feeder "$REAL" --checker "$CHECKER" --duration 2 --sample 1 --work "$WORK/w1"

echo PASS
