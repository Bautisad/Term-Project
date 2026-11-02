#!/bin/sh
# nonetcheck.sh: the "no network calls except serving our own LAN page" guard (CLAUDE.md, a graded
# boundary).  Reads the product source (comments removed, strings kept) and, if given, the built
# daemon, and fails on anything that could reach out.
#
# usage: nonetcheck.sh SRC_DIR [BINARY]      SRC_DIR is the repository's src/ directory
# exit:  0 clean, 1 violations (one line each: file:line: [RULE] what), 2 bad usage
#
# Rules (the allow-lists are the variables just below; extend one only with a reason):
#   R1  no resolver, datagram send, shell, process-spawn or library-loading call anywhere in src/
#       (getaddrinfo, gethostby*, sendto, system, popen, dlopen, ...); connect() only where
#       CONNECT_OK says (the AF_UNIX link to the hub); fork()/exec*() only where SPAWN_OK says
#   R2  no datagram, raw, packet or netlink sockets anywhere; AF_INET/AF_INET6 only where INET_OK
#       says (the LAN page)
#   R3  src/interface creates exactly one socket, a SOCK_STREAM that it listens on and accepts
#       from; every other socket() or socketpair() in src/ says AF_UNIX on the same line
#   R4  the LAN page is self-contained: no http(s):// URL, script, stylesheet link, @import,
#       url() or protocol-relative src/href in src/interface
#   R5  the files R3 needs are there, so the checks cannot pass by looking at nothing
#   R6  with BINARY: it does not import those R1 functions (needs nm and a dynamically linked
#       binary; otherwise it says so and skips only this rule).  connect() is deliberately not
#       banned here: sensor_util.o links the AF_UNIX hub connection, which the page never uses;
#       interface_lifecycle_test checks at run time that every socket it holds is local or the
#       page's own.
#
# A grep cannot see everything (a call through a function pointer or a macro slips by), which is
# why the run-time check exists too.  Plain POSIX sh and awk, so it runs on the Pi (dash, mawk).

CONNECT_OK='ipc/ipc_io.c'                # shell globs, relative to SRC_DIR
SPAWN_OK='supervisor/child.c'
INET_OK='interface/*'
NEEDED='page.c http.c daemon.c main.c'   # files of src/interface that R3 and R5 rely on

NAME_START='(^|[^A-Za-z0-9_])'
P_BAN="${NAME_START}(getaddrinfo|gethostby[a-z0-9_]*|getnameinfo|sendto|sendmsg|sendmmsg|recvfrom|res_query|res_search|res_init|system|popen|dlopen|posix_spawnp?)[[:space:]]*\\("
P_CONNECT="${NAME_START}connect[[:space:]]*\\("
P_SPAWN="${NAME_START}(fork|vfork|execv|execve|execvp|execvpe|execl|execle|execlp|fexecve)[[:space:]]*\\("
P_SOCKTYPE="${NAME_START}(SOCK_DGRAM|SOCK_RAW|AF_PACKET|PF_PACKET|AF_NETLINK|PF_NETLINK)([^A-Za-z0-9_]|\$)"
P_INET="${NAME_START}(AF_INET6?|PF_INET6?)([^A-Za-z0-9_]|\$)"
P_SOCKCALL="${NAME_START}(socket|socketpair)[[:space:]]*\\("
P_SOCKET="${NAME_START}socket[[:space:]]*\\("
P_WEB="https?://|<script|<link|@import|(src|href)[[:space:]]*=[^/]*//|${NAME_START}url[[:space:]]*\\("
# What R6 refuses as an import of the binary (R1's names, plus the C library's internal spellings).
BIN_BAN='getaddrinfo|gethostby[a-z0-9_]*|getnameinfo|sendto|sendmsg|sendmmsg|recvfrom|res_query|res_search|res_init|__res_[a-z_]*|dlopen|system|popen|fork|vfork|posix_spawnp?|exec[vl][a-z]*|fexecve'

usage() {
    echo "usage: nonetcheck.sh SRC_DIR [BINARY]" >&2
    exit 2
}

[ $# -ge 1 ] && [ $# -le 2 ] || usage
SRC=$1
BIN=$2
[ -d "$SRC" ] || { echo "nonetcheck: $SRC is not a directory" >&2; exit 2; }
[ -z "$BIN" ] || [ -f "$BIN" ] || { echo "nonetcheck: $BIN is not a file" >&2; exit 2; }

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT
CODE=$WORK/code
FILES=$WORK/files
VIOL=$WORK/violations
: > "$VIOL"
mkdir "$CODE" || exit 2

# Removes comments and keeps everything else, one output line per input line so the line numbers
# still match.  A comment marker inside a string or character literal is not a comment ("http://"
# must stay visible), and a quote inside a comment is not a literal.  One awk run for all files:
# each input SRC/x/y.c is written to CODE/x/y.c.
cat > "$WORK/strip.awk" <<'EOF'
FNR == 1 {
    if (prev != "") { close(prev) }
    prev = out "/" substr(FILENAME, length(src) + 2)
    blk = 0
}
{
    res = ""
    n = length($0)
    i = 1
    while (i <= n) {
        c = substr($0, i, 1)
        d = substr($0, i + 1, 1)
        if (blk) {
            if (c == "*" && d == "/") { blk = 0; i += 2; res = res " " } else { i++ }
        } else if (str != "") {
            res = res c
            if (c == "\\") { res = res d; i += 2 }
            else { if (c == str) { str = "" }; i++ }
        } else if (c == "/" && d == "*") { blk = 1; i += 2 }
        else if (c == "/" && d == "/") { i = n + 1 }
        else if (c == "\"" || c == "\047") { str = c; res = res c; i++ }
        else { res = res c; i++ }
    }
    str = ""
    print res > prev
}
END { if (prev != "") { close(prev) } }
EOF

(cd "$SRC" && find . -type f \( -name '*.c' -o -name '*.h' \)) | sed 's|^\./||' | sort > "$FILES"

if [ -s "$FILES" ]; then
    awk -F/ 'NF > 1 { sub(/\/[^\/]*$/, ""); print }' "$FILES" | sort -u |
    while IFS= read -r d; do
        mkdir -p "$CODE/$d" || exit 2
    done
    while IFS= read -r f; do
        : > "$CODE/$f"   # an empty source file gives awk no line to write, and grep needs a file
    done < "$FILES"
    # shellcheck disable=SC2046  # file names here never contain spaces
    awk -v src="$SRC" -v out="$CODE" -f "$WORK/strip.awk" $(sed "s|^|$SRC/|" "$FILES") ||
        { echo "nonetcheck: cannot read the sources" >&2; exit 2; }
fi

viol() {
    echo "$1" >> "$VIOL"
}

# pick ONLY EXEMPT: the code files whose path matches the glob ONLY and not the glob EXEMPT
# (empty: none exempt), on one line.
pick() {
    p_list=
    while IFS= read -r p_f; do
        case $p_f in
        $1) ;;
        *) continue ;;
        esac
        if [ -n "$2" ]; then
            case $p_f in
            $2) continue ;;
            esac
        fi
        p_list="$p_list $p_f"
    done < "$FILES"
    echo "$p_list"
}

# scan RULE WHAT PATTERN ONLY EXEMPT [LACKING]: every line of the files pick(ONLY, EXEMPT) that
# matches PATTERN (and, if given, does not contain LACKING).  One grep for all of them.
scan() {
    s_files=$(pick "$4" "$5")
    [ -n "$s_files" ] || return 0
    # shellcheck disable=SC2086
    (cd "$CODE" && grep -nE -e "$3" $s_files /dev/null) |
    { if [ -n "$6" ]; then grep -v -e "$6"; else cat; fi; } |
    awk -v rule="$1" -v what="$2" '{
        i = index($0, ":"); f = substr($0, 1, i - 1); rest = substr($0, i + 1)
        j = index(rest, ":"); ln = substr(rest, 1, j - 1); t = substr(rest, j + 1)
        sub(/^[ \t]+/, "", t)
        print f ":" ln ": [" rule "] " what ": " t
    }' >> "$VIOL"
}

# count PATTERN ONLY: how many lines of the files pick(ONLY) match PATTERN.
count() {
    c_files=$(pick "$2" "")
    # shellcheck disable=SC2086
    (cd "$CODE" && grep -cE -e "$1" $c_files /dev/null) | awk -F: '{ s += $NF } END { print s + 0 }'
}

scan R1 "banned call (resolver, datagram send, shell, library loading)" "$P_BAN" '*' ''
scan R1 "connect() outside $CONNECT_OK" "$P_CONNECT" '*' "$CONNECT_OK"
scan R1 "fork/exec outside $SPAWN_OK" "$P_SPAWN" '*' "$SPAWN_OK"
scan R2 "datagram, raw, packet or netlink socket" "$P_SOCKTYPE" '*' ''
scan R2 "AF_INET/AF_INET6 outside $INET_OK" "$P_INET" '*' "$INET_OK"
scan R3 "socket without AF_UNIX outside src/interface" "$P_SOCKCALL" '*' 'interface/*' 'AF_UNIX'
scan R4 "the page reaches outside itself" "$P_WEB" 'interface/*' ''

# R5, then R3 for src/interface itself.
missing=0
for f in $NEEDED; do
    if [ ! -f "$CODE/interface/$f" ]; then
        viol "interface/$f:0: [R5] file missing, so the interface checks cannot run"
        missing=1
    fi
done
if [ "$missing" -eq 0 ]; then
    n=$(count "$P_SOCKET" 'interface/*')
    [ "$n" -eq 1 ] || viol "interface:0: [R3] expected exactly one socket() call (the listener), found $n"
    grep -qE 'SOCK_STREAM' "$CODE/interface/daemon.c" || viol "interface/daemon.c:0: [R3] the listener is not a SOCK_STREAM socket"
    grep -qE "${NAME_START}listen[[:space:]]*\\(" "$CODE/interface/daemon.c" || viol "interface/daemon.c:0: [R3] no listen() call"
    grep -qE "${NAME_START}accept4?[[:space:]]*\\(" "$CODE/interface/daemon.c" || viol "interface/daemon.c:0: [R3] no accept() call"
fi

# R6
BINNOTE="not given"
if [ -n "$BIN" ]; then
    if ! command -v nm >/dev/null 2>&1; then
        BINNOTE="NOT CHECKED (nm is not installed)"
    else
        SYMS=$(nm -D -u "$BIN" 2>/dev/null | awk '{ print $NF }' | sed 's/@.*$//')
        if [ -z "$SYMS" ]; then
            BINNOTE="NOT CHECKED (no dynamic symbols: a static build?)"
        else
            BINNOTE="checked"
            printf '%s\n' "$SYMS" | grep -E "^($BIN_BAN)\$" | sort -u |
            while IFS= read -r sym; do
                viol "$BIN:0: [R6] the binary imports $sym"
            done
        fi
    fi
fi

NFILES=$(wc -l < "$FILES" | tr -d ' ')
if [ -s "$VIOL" ]; then
    sort "$VIOL"
    echo "nonetcheck: FAIL ($(wc -l < "$VIOL" | tr -d ' ') violations in $NFILES files; binary: $BINNOTE)"
    echo "nonetcheck: an allow-list lives at the top of tools/nonetcheck.sh; extend it only with a reason"
    RESULT=1
else
    echo "nonetcheck: OK ($NFILES files; binary: $BINNOTE)"
    RESULT=0
fi
# The script ends here rather than with `exit`: bash does not free the command it is running when
# `exit` runs, which Valgrind (make memcheck, which runs this under bash) reports as a leak.  The
# last command's status is the script's status.
[ "$RESULT" -eq 0 ]
