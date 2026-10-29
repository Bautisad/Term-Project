#!/bin/sh
# kill -9 loop tests.  usage: killloop_test.sh <killloop.sh> <dir with gt-feeder and gt-logcheck>
# Each case runs the real script against the real feeder and checker.

LOOP=$1
BIN=$2
WORK=$(mktemp -d) || exit 1
export ASAN_OPTIONS=exitcode=99:detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:exitcode=99

trap 'rm -rf "$WORK"' EXIT

# A checker that always fails (a path: command -v would return the shell builtin).
FALSE=/bin/false
[ -x "$FALSE" ] || FALSE=/usr/bin/false

fail() {
    echo "FAIL: $*"
    echo "--- output:"
    echo "$OUT"
    exit 1
}

# run ARGS...: runs the loop; its output is in OUT and its exit code in RC.
run() {
    OUT=$(sh "$LOOP" "$@" 2>&1)
    RC=$?
}

contains() {
    case $OUT in
    *"$1"*) return 0 ;;
    esac
    return 1
}

# 1. A small run passes, reports every iteration, and leaves its results where we said.
run -n 5 --max-delay-ms 150 --bin "$BIN" --work "$WORK/w1"
[ "$RC" = 0 ] || fail "smoke run exited $RC"
contains "iterations=5 passed=5 failed=0" || fail "smoke run summary missing"
n=$(echo "$OUT" | grep -c '^iter=')
[ "$n" = 5 ] || fail "expected 5 iter lines, got $n"
[ "$(wc -l <"$WORK/w1/results.log")" = 5 ] || fail "results.log should have 5 lines"
grep -q 'rc=0' "$WORK/w1/checks.log" || fail "checks.log has no checker output"
[ -d "$WORK/w1" ] || fail "a --work directory must not be deleted"

# 2. Without --work the scratch directory is removed after a passing run.
run -n 2 --max-delay-ms 50 --bin "$BIN"
[ "$RC" = 0 ] || fail "default-work run exited $RC"
tmpdir=$(echo "$OUT" | sed -n '1s/.* work=//p')
[ -n "$tmpdir" ] && [ ! -e "$tmpdir" ] || fail "scratch directory $tmpdir was not removed"

# 3. A checker that fails is reported: every iteration fails, exit 1, the store is copied.
run -n 3 --max-delay-ms 50 --bin "$BIN" --checker "$FALSE" --work "$WORK/w3"
[ "$RC" = 1 ] || fail "failing checker: expected exit 1, got $RC"
contains "passed=0 failed=3" || fail "failing checker: summary should say failed=3"
contains "check_restart=FAIL" || fail "failing checker: no FAIL line"
[ -d "$WORK/w3/failed-iter-1/store" ] || fail "failing checker: no copy of the store"

# 4. --stop-on-fail stops at the first failure.
run -n 5 --max-delay-ms 50 --stop-on-fail --bin "$BIN" --checker "$FALSE" --work "$WORK/w4"
[ "$RC" = 1 ] || fail "stop-on-fail: expected exit 1, got $RC"
contains "iterations=1 passed=0 failed=1" || fail "stop-on-fail: should stop after one iteration"

# 5. Real damage is caught.  The wrapper flips a byte in the third record of the log (once the
# log has at least six, so the damage is mid-log) and then runs the real checker.
cat >"$WORK/flipchk.sh" <<EOF
#!/bin/sh
if [ ! -e "$WORK/flipped" ]; then
    seg=\$(ls "\$2"/*.log 2>/dev/null | head -n 1)
    if [ -n "\$seg" ] && [ "\$(wc -c <"\$seg")" -ge 144 ]; then
        printf '\377' | dd of="\$seg" bs=1 seek=53 conv=notrunc 2>/dev/null
        : >"$WORK/flipped"
    fi
fi
exec "$BIN/gt-logcheck" "\$@"
EOF
chmod +x "$WORK/flipchk.sh"
run -n 4 --max-delay-ms 200 --bin "$BIN" --checker "$WORK/flipchk.sh" --work "$WORK/w5"
[ -e "$WORK/flipped" ] || fail "damage test: the log never grew enough to damage"
[ "$RC" = 1 ] || fail "damage test: expected exit 1, got $RC"
contains "FAIL" || fail "damage test: no FAIL reported"
grep -q 'corrupt=1' "$WORK/w5/checks.log" || fail "damage test: checker did not see the corrupt record"

# 6. The same seed gives the same delays.
run -n 3 --seed 7 --max-delay-ms 100 --bin "$BIN" --work "$WORK/w6a"
d1=$(echo "$OUT" | grep -o 'delay_ms=[0-9]*' | tr '\n' ' ')
run -n 3 --seed 7 --max-delay-ms 100 --bin "$BIN" --work "$WORK/w6b"
d2=$(echo "$OUT" | grep -o 'delay_ms=[0-9]*' | tr '\n' ' ')
[ -n "$d1" ] && [ "$d1" = "$d2" ] || fail "same seed gave different delays: '$d1' vs '$d2'"

# 7. Bad arguments and missing binaries exit 2.
mkdir "$WORK/full" && : >"$WORK/full/file"
for args in "-n abc" "-n 0" "--bogus" "--rate" "--max-delay-ms 0" "--bin $WORK/nowhere" \
    "--bin $BIN --work $WORK/full" "--bin $BIN --checker $WORK/nowhere"; do
    # shellcheck disable=SC2086
    run $args
    [ "$RC" = 2 ] || fail "'$args': expected exit 2, got $RC"
done

echo PASS
