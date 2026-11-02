#!/bin/sh
# Tests for the no-network guard.  usage: nonet_test.sh <nonetcheck.sh> <src dir>
# The guard is only worth having if it fails when it should, so each case copies the real src/
# into a scratch directory, plants one violation, and checks that the guard exits 1 and names the
# right rule.  Other cases check that it stays quiet where it should (comments, the real tree)
# and that it refuses bad usage.

CHECK=$1
SRC=$2
[ -f "$CHECK" ] && [ -d "$SRC" ] || { echo "usage: nonet_test.sh <nonetcheck.sh> <src dir>" >&2; exit 2; }
WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT

T=$WORK/src          # the scratch copy the cases change
OUT=
RC=0
CASES=0

fail() {
    echo "FAIL: $*"
    echo "--- guard output (exit $RC):"
    echo "$OUT"
    exit 1
}

fresh() {
    rm -rf "$T"
    mkdir "$T" && cp -R "$SRC/." "$T" || fail "cannot copy $SRC"
}

run() {
    OUT=$(sh "$CHECK" "$@" 2>&1)
    RC=$?
}

contains() {
    case $OUT in
    *"$1"*) return 0 ;;
    esac
    return 1
}

# plant FILE LINE...: add the lines to the end of a file of the copy (it is never compiled).
plant() {
    f=$1
    shift
    [ -f "$T/$f" ] || fail "no such file in the copy: $f"
    printf '%s\n' "$@" >> "$T/$f"
}

# violation DESCRIPTION RULE: the guard must exit 1 and report RULE.
violation() {
    CASES=$((CASES + 1))
    run "$T" "$BINARY"
    [ "$RC" -eq 1 ] || fail "$1: expected exit 1, got $RC"
    contains "[$2]" || fail "$1: expected a [$2] violation"
}

# quiet DESCRIPTION: the guard must exit 0.
quiet() {
    CASES=$((CASES + 1))
    run "$T" "$BINARY"
    [ "$RC" -eq 0 ] || fail "$1: expected exit 0, got $RC"
}

BINARY=

# --- the real tree is clean -------------------------------------------------------------
fresh
quiet "the real src/"

# --- banned calls (R1) -------------------------------------------------------------------
fresh; plant interface/daemon.c 'void leak(void) { connect(1, 0, 0); }'
violation "connect() in the page daemon" R1
fresh; plant sensors/soil_main.c 'void leak(void) { getaddrinfo("x", 0, 0, 0); }'
violation "getaddrinfo() in a sensor" R1
fresh; plant sensors/sensor_log.c 'int leak(void) { return system("curl x"); }'
violation "system()" R1
fresh; plant interface/http.c 'void leak(void) { sendto(1, 0, 0, 0, 0, 0); }'
violation "sendto()" R1
fresh; plant sensors/sensor_util.c 'void leak(void) { popen("wget x", "r"); }'
violation "popen()" R1
fresh; plant sensors/soil_proc.c 'void leak(void) { dlopen("libx.so", 0); }'
violation "dlopen()" R1
fresh; plant sensors/light_proc.c 'void leak(void) { fork(); }'
violation "fork() outside the supervisor" R1
fresh; plant ipc/ipc_io.c 'void leak(void) { execv("/bin/sh", 0); }'
violation "exec outside the supervisor" R1
fresh; printf 'void leak(void) { connect(1, 0, 0); }\n' > "$T/ipc/other.c"
violation "connect() in an ipc file other than ipc_io.c" R1
fresh; mkdir "$T/hub" && printf 'void leak(void) { gethostbyname("x"); }\n' > "$T/hub/main.c"
violation "a new directory (the future hub) is scanned too" R1

# --- sockets (R2, R3) --------------------------------------------------------------------
fresh; plant interface/http.c 'int leak(void) { return socket(AF_INET, SOCK_DGRAM, 0); }'
violation "a datagram socket in the page" R2
fresh; plant sensors/sensor_util.c 'int leak = AF_INET;'
violation "AF_INET outside the page" R2
fresh; plant ipc/ipc_io.c 'int leak = AF_PACKET;'
violation "AF_PACKET" R2
fresh; plant interface/page.c 'int leak(void) { return socket(AF_INET, SOCK_STREAM, 0); }'
violation "a second socket in the page" R3
fresh; plant sensors/soil_proc.c 'int leak(int d) { return socket(d, SOCK_STREAM, 0); }'
violation "a socket without AF_UNIX outside the page" R3
fresh; plant sensors/soil_proc.c 'int leak(int d) { return socketpair(d, SOCK_STREAM, 0, 0); }'
violation "a socketpair without AF_UNIX outside the page" R3

# --- the page stays self-contained (R4) --------------------------------------------------
fresh; plant interface/page.c 'const char* leak = "<img src=\"https://cdn.example/x.png\">";'
violation "an https URL in the page" R4
fresh; plant interface/page.c 'const char* leak = "<p>see http://example.com</p>";'
violation "an http URL in the page" R4
fresh; plant interface/page.c 'const char* leak = "<script>load()</script>";'
violation "a script in the page" R4
fresh; plant interface/page.c 'const char* leak = "<link rel=stylesheet href=x.css>";'
violation "a stylesheet link in the page" R4
fresh; plant interface/page.c 'const char* leak = "<style>@import \"x.css\";</style>";'
violation "@import in the page" R4
fresh; plant interface/page.c 'const char* leak = "<img src=\"//cdn.example/x.png\">";'
violation "a protocol-relative source in the page" R4
fresh; plant interface/page.c 'const char* ok = "<a href=\"/status.json\">json</a>";'
quiet "a relative link inside the page"

# --- the guard cannot pass by looking at nothing (R5) ------------------------------------
fresh; rm "$T/interface/daemon.c"
violation "daemon.c missing" R5
fresh; rm "$T/interface/main.c"
violation "main.c missing" R5
fresh; rm -r "$T/interface"
violation "src/interface missing" R5

# --- comments are not code, strings are not comments -------------------------------------
fresh
plant interface/daemon.c '/* connect(fd) getaddrinfo(x) https://example.com */'
plant interface/daemon.c '// system("x"); fork();'
plant interface/daemon.c '/* a longer comment'
plant interface/daemon.c '   sendto(1, 0, 0, 0, 0, 0); socket(AF_INET6, SOCK_DGRAM, 0);'
plant interface/daemon.c '   <script src="http://x/y.js"> */ int after_comment;'
quiet "banned words inside comments"
fresh; plant interface/daemon.c 'const char* s = "/*"; int leak(void) { return system("x"); }'
violation "a string holding /* does not start a comment" R1
fresh; plant interface/daemon.c 'const char* s = "a // b"; int leak(void) { return system("x"); }'
violation "a string holding // does not start a comment" R1
fresh; plant interface/daemon.c "char c = '\"'; int leak(void) { return system(\"x\"); }"
violation "a quote character literal does not open a string" R1
fresh; plant sensors/soil_main.c 'int file_system(void); int my_system_ok = 1; int connection = 2;'
quiet "names that only contain a banned word"

# --- usage -------------------------------------------------------------------------------
CASES=$((CASES + 1)); run;                         [ "$RC" -eq 2 ] || fail "no arguments: expected exit 2"
CASES=$((CASES + 1)); run "$WORK/not-a-directory"; [ "$RC" -eq 2 ] || fail "missing directory: expected exit 2"
CASES=$((CASES + 1)); run "$T" "$WORK/no-such-binary"; [ "$RC" -eq 2 ] || fail "missing binary: expected exit 2"
CASES=$((CASES + 1)); run "$T" a b;                [ "$RC" -eq 2 ] || fail "three arguments: expected exit 2"

# --- the binary check (R6): needs a C compiler and nm ------------------------------------
CC=
for c in "${CC_FOR_TEST:-}" cc gcc clang; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { CC=$c; break; }
done
if [ -n "$CC" ] && command -v nm >/dev/null 2>&1; then
    printf 'int main(void) { return 0; }\n' > "$WORK/good.c"
    printf 'int getaddrinfo();\nint main(void) { return getaddrinfo(0, 0, 0, 0); }\n' > "$WORK/bad.c"
    if "$CC" -o "$WORK/good" "$WORK/good.c" 2>/dev/null && "$CC" -o "$WORK/bad" "$WORK/bad.c" 2>/dev/null &&
       [ -n "$(nm -D -u "$WORK/bad" 2>/dev/null)" ]; then
        fresh
        BINARY=$WORK/good
        quiet "a binary that imports nothing banned"
        contains "binary: checked" || fail "the clean binary was not actually checked"
        BINARY=$WORK/bad
        violation "a binary importing getaddrinfo" R6
        BINARY=
    else
        echo "SKIP: cannot build a dynamically linked test binary here; the R6 cases did not run"
    fi
else
    echo "SKIP: no C compiler or nm here; the R6 cases did not run"
fi

echo "PASS ($CASES cases)"
