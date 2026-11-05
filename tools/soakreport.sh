#!/bin/sh
# Soak report: judges a soak run from a journal export (DESIGN test P4, and the S6 evidence).
# Reads the supervisor's hourly "status" lines (see DESIGN.md "Hourly status line") and checks
# each child's memory (RSS) and open file count over the run.  With --marks it also checks the
# planned fault injections, the orderly stop and the run length; with --unit-result the unit's
# exit; with --store the records.  See SOAK-RUNBOOK.md for how a run is made.
#
# usage: soakreport.sh --journal FILE [--hours 48] [--interval 3600] [--warmup 1]
#                      [--marks FILE] [--unit-result FILE] [--store DIR] [--checker PATH]
#   --journal FILE      the export: `journalctl -u greenthumb -o short-iso --no-pager > FILE`, or
#                       the supervisor's stderr saved to a file
#   --hours H           how long the run was meant to be, default 48 (may be fractional, for a
#                       short rehearsal)
#   --interval S        seconds between status lines, default 3600 (the supervisor's --status)
#   --warmup N          status lines to skip at the start of each run of a child, default 1; a
#                       run ends where the child's restart count rises, because a restart
#                       starts the process over
#   --marks FILE        the injection marks, one "<UTC ISO time>Z label" per line, written by
#                       soakmark.sh.  Labels: start, soil_unplug_start, soil_unplug_end,
#                       kill_soil, kill_light, stop.  Without it only the status lines are judged.
#   --unit-result FILE  `systemctl show greenthumb -p Result -p ExecMainStatus` after the stop
#   --store DIR         the store the hub wrote; judged by gt-logcheck
#   --checker PATH      gt-logcheck, default: next to this script
#
# Fails on, per child (gt-hub, gt-sensor-soil, gt-sensor-light, gt-interface):
#   no status lines, too few of them, a gap of more than 1.5 intervals between two of them, one
#   that stopped reporting while the others went on, one that was not alive at its last line,
#   a malformed or out-of-order line, RSS growth over 1 MB (1024 kB) within a run, and an
#   open file count that changes within a run after the warm-up.
# With --marks, also:
#   a restart, death, exit, missed heartbeat, kill, degraded state or ERROR line that is not
#   inside a marked window of the same child (an unplanned event); an unplug that is not logged
#   as read_failed within 150 s, not shown as state=degraded, or not followed by state=running
#   within 180 s of the replug; a kill -9 not logged as "died signal=9" within 5 s, or not
#   followed by state=running within 120 s; a stop that is not "stopping", four "exited code=0"
#   and "stopped"; a run shorter than --hours between the start and stop marks; no start or
#   stop mark.  A window runs from 5 s before its mark to 150 s after (after the replug, for
#   an unplug).
# With --unit-result: Result is not success or ExecMainStatus is not 0.
# With --store: the checker rejects the store, it has fewer than hours*60 records, has a lost
#   record (a gap in the sequence numbers), or holds synthetic records.
# Prints a table (one row per child), what each injection showed, and then PASS or one
# "FAIL text" line per failed check.
# Exit 0: pass.  1: some check failed.  2: bad arguments or a bad marks file.

JOURNAL=
HOURS=48
INTERVAL=3600
WARMUP=1
MARKS=
UNIT=
STORE=
CHECKER=

# DESIGN P4: RSS growth under 1 MB per process (soakcheck.sh fails above the same figure).
MAX_RSS_GROWTH_KB=1024

WORK=

usage() {
    sed -n '2,/^# Exit 0/p' "$0" | sed 's/^# \{0,1\}//'
}

usage_error() {
    echo "soakreport: $*" >&2
    usage >&2
    exit 2
}

is_uint() {
    case $1 in
    '' | *[!0-9]*) return 1 ;;
    esac
    return 0
}

# is_positive N: a decimal number above zero, like 48 or 0.5.
is_positive() {
    case $1 in
    '' | *[!0-9.]* | .* | *. | *.*.*) return 1 ;;
    esac
    awk -v x="$1" 'BEGIN { exit !(x + 0 > 0) }'
}

while [ $# -gt 0 ]; do
    case $1 in
    -h | --help)
        usage
        exit 0
        ;;
    --journal | --hours | --interval | --warmup | --marks | --unit-result | --store | --checker) ;;
    *) usage_error "unknown argument: $1" ;;
    esac
    [ $# -ge 2 ] || usage_error "$1 needs a value"
    case $1 in
    --journal) JOURNAL=$2 ;;
    --hours)
        is_positive "$2" || usage_error "--hours needs a number above 0, got '$2'"
        HOURS=$2
        ;;
    --interval)
        is_uint "$2" && [ "$2" -ge 1 ] || usage_error "--interval needs a whole number of seconds, got '$2'"
        INTERVAL=$2
        ;;
    --warmup)
        is_uint "$2" || usage_error "--warmup needs a whole number, got '$2'"
        WARMUP=$2
        ;;
    --marks) MARKS=$2 ;;
    --unit-result) UNIT=$2 ;;
    --store) STORE=$2 ;;
    --checker) CHECKER=$2 ;;
    esac
    shift 2
done

[ -n "$JOURNAL" ] || usage_error "--journal is required"
[ -f "$JOURNAL" ] && [ -r "$JOURNAL" ] || usage_error "cannot read the journal $JOURNAL"
[ -z "$MARKS" ] || { [ -f "$MARKS" ] && [ -r "$MARKS" ]; } || usage_error "cannot read the marks file $MARKS"
[ -z "$UNIT" ] || { [ -f "$UNIT" ] && [ -r "$UNIT" ]; } || usage_error "cannot read the unit result $UNIT"
if [ -n "$STORE" ]; then
    [ -d "$STORE" ] || usage_error "no store directory at $STORE"
    [ -n "$CHECKER" ] || CHECKER=$(cd "$(dirname "$0")" && pwd)/gt-logcheck
    [ -x "$CHECKER" ] || usage_error "no executable checker at $CHECKER (build first, or use --checker)"
fi

WORK=$(mktemp -d) || usage_error "cannot create a scratch directory"
trap 'rm -rf "$WORK"' EXIT

# The unit result: two lines from `systemctl show`.
HAS_UNIT=0
UNIT_RESULT=
UNIT_STATUS=
if [ -n "$UNIT" ]; then
    HAS_UNIT=1
    UNIT_RESULT=$(sed -n 's/^Result=//p' "$UNIT" | head -n 1)
    UNIT_STATUS=$(sed -n 's/^ExecMainStatus=//p' "$UNIT" | head -n 1)
fi

# The store, as soakcheck.sh judges it: the checker's exit code and its summary line.  A soak has
# no kills of the hub, so even a single skipped number is unexplained.
HAS_STORE=0
STORE_RC=
STORE_RECORDS=
STORE_LOST=
STORE_CORRUPT=
STORE_SYNTH=
STORE_WHY=
if [ -n "$STORE" ]; then
    HAS_STORE=1
    "$CHECKER" --dir "$STORE" >"$WORK/check.log" 2>&1
    STORE_RC=$?
    summary=$(grep '^logcheck ' "$WORK/check.log" | head -n 1)
    STORE_RECORDS=$(echo "$summary" | sed -n 's/.* records=\([0-9]*\).*/\1/p')
    STORE_LOST=$(echo "$summary" | sed -n 's/.* lost=\([0-9]*\).*/\1/p')
    STORE_CORRUPT=$(echo "$summary" | sed -n 's/.* corrupt=\([0-9]*\).*/\1/p')
    STORE_SYNTH=$(echo "$summary" | sed -n 's/.* synthetic=\([0-9]*\).*/\1/p')
    STORE_WHY=$(grep '^FAIL' "$WORK/check.log" | head -n 1)
    [ -n "$summary" ] || STORE_WHY="no summary line from the checker"
fi

# The Pi has mawk, which has no mktime(), so the time is worked out by hand (days from civil).
cat >"$WORK/report.awk" <<'AWK'
function epoch(ts,    y, m, d, era, yoe, doy, doe) {
    y = substr(ts, 1, 4) + 0
    m = substr(ts, 6, 2) + 0
    d = substr(ts, 9, 2) + 0
    if (m <= 2) y--
    era = int(y / 400)
    yoe = y - era * 400
    doy = int((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1
    doe = yoe * 365 + int(yoe / 4) - int(yoe / 100) + doy
    return (era * 146097 + doe - 719468) * 86400 + substr(ts, 12, 2) * 3600 + substr(ts, 15, 2) * 60 + substr(ts, 18, 2)
}

function isoz(s) {
    return length(s) == 20 && s ~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z$/
}

function bad(msg) { msgs[++nbad] = msg }

function note(msg) { evid[++nevid] = msg }

function isnum(s) { return s ~ /^-?[0-9]+$/ }

function usage_bad(msg) { if (!usage_fail) usage_fail = msg }

function comp2child(comp) {
    if (comp == "soil") return "gt-sensor-soil"
    if (comp == "light") return "gt-sensor-light"
    if (comp == "hub") return "gt-hub"
    if (comp == "interface") return "gt-interface"
    return ""
}

# A line that is not a status line: kept if it is an ERROR line or a supervisor event about a
# child (restart, death, exit, state change ...) or its stop lines.
function event(    i, lvl, comp, w3, c, kind, st, code, j, tk, text, k) {
    # the log time is the ISO "Z" field followed by a level; a journal prefix has its own time first
    for (i = 1; i < NF; i++) if (isoz($i) && ($(i + 1) == "INFO" || $(i + 1) == "WARN" || $(i + 1) == "ERROR")) break
    if (i > NF - 2) return
    lvl = $(i + 1)
    comp = $(i + 2)
    w3 = $(i + 3)
    kind = "other"
    c = ""
    st = ""
    code = ""
    if (comp == "supervisor") {
        if (w3 ~ /^child=/) {
            c = substr(w3, 7)
            for (j = i + 4; j <= NF; j++) {
                tk = $j
                if (tk == "recovered") { kind = "recovered"; break }
                if (tk ~ /^restart=/) { kind = "restart"; break }
                if (tk == "died") { kind = "died"; code = $(j + 1); sub(/^signal=/, "", code); break }
                if (tk == "exited") { kind = "exited"; code = $(j + 1); sub(/^code=/, "", code); break }
                if (tk ~ /^state=/) { kind = "state"; st = substr(tk, 7); break }
                if (tk == "heartbeat_missed" || tk == "heartbeat_timeout" || tk == "killed" ||
                    tk == "crash_loop" || tk == "no_restart") { kind = tk; break }
            }
        } else if (w3 == "stopping" || w3 == "stopped") {
            kind = w3
        } else if (lvl != "ERROR") {
            return
        }
    } else if (lvl != "ERROR") {
        return
    } else {
        c = comp2child(comp)
    }
    text = $i
    for (k = i + 1; k <= NF; k++) text = text " " $k
    if (comp == "soil" && lvl == "ERROR" && index(text, " read_failed ") > 0) kind = "read_failed"
    ne++
    ev_t[ne] = epoch($i)
    ev_ts[ne] = $i
    ev_c[ne] = c
    ev_kind[ne] = kind
    ev_st[ne] = st
    ev_lvl[ne] = lvl
    ev_code[ne] = code
    ev_text[ne] = substr(text, 1, 140)
}

function addwin(c, lo, hi) {
    nwin++
    win_c[nwin] = c
    win_lo[nwin] = lo
    win_hi[nwin] = hi
}

function in_window(e,    w) {
    for (w = 1; w <= nwin; w++)
        if (win_c[w] == ev_c[e] && ev_t[e] >= win_lo[w] && ev_t[e] <= win_hi[w]) return 1
    return 0
}

# The earliest event of this kind for this child at or after time lo; 0 if there is none.
function first_event(kind, c, lo,    e, best) {
    best = 0
    for (e = 1; e <= ne; e++)
        if (ev_kind[e] == kind && ev_c[e] == c && ev_t[e] >= lo && (best == 0 || ev_t[e] < ev_t[best])) best = e
    return best
}

# The earliest state=ST line for this child at or after time lo.
function first_state(st, c, lo,    e, best) {
    best = 0
    for (e = 1; e <= ne; e++)
        if (ev_kind[e] == "state" && ev_st[e] == st && ev_c[e] == c && ev_t[e] >= lo && (best == 0 || ev_t[e] < ev_t[best])) best = e
    return best
}

BEGIN {
    MARK = " INFO supervisor status "
    nexp = split("gt-hub gt-sensor-soil gt-sensor-light gt-interface", expected, " ")
    gapmax = 1.5 * interval
    PRE = 5            # a mark may be typed a few seconds after the action
    WIN_POST = 150     # an event this long after a mark still belongs to it
    DETECT_MAX = 150   # S6: a fault is logged within 2 sample cycles
    DIED_MAX = 5
    RUN_AFTER_KILL = 120
    RUN_AFTER_REPLUG = 180
    split("start soil_unplug_start soil_unplug_end kill_soil kill_light stop", lab, " ")
    for (i in lab) known[lab[i]] = 1
}

hasmarks && FILENAME == marksfile {
    if (NF == 0 || $1 ~ /^#/) next
    if (NF != 2 || !isoz($1) || !($2 in known)) {
        usage_bad("marks " marksfile " line " FNR ": want '<UTC time>Z label' with a known label")
        next
    }
    if (nm > 0 && epoch($1) < mark_t[nm]) usage_bad("marks " marksfile " line " FNR ": out of time order")
    nm++
    mark_t[nm] = epoch($1)
    mark_ts[nm] = $1
    mark_l[nm] = $2
    next
}

{
    p = index($0, MARK)
    if (p == 0) {
        if (hasmarks) event()
        next
    }
    nstatus++
    ts = p > 20 ? substr($0, p - 20, 20) : ""
    delete v
    nt = split(substr($0, p + length(MARK)), tok, " ")
    for (j = 1; j <= nt; j++) {
        eq = index(tok[j], "=")
        if (eq > 1) v[substr(tok[j], 1, eq - 1)] = substr(tok[j], eq + 1)
    }
    if (!isoz(ts) ||
        v["child"] == "" || !isnum(v["alive"]) || !isnum(v["rss_kb"]) || !isnum(v["fds"]) ||
        !isnum(v["restarts"]) || !isnum(v["faults"])) {
        bad("malformed status line " NR ": " substr($0, 1, 100))
        next
    }
    c = v["child"]
    t = epoch(ts)
    if (n[c] > 0) {
        dt = t - last_t[c]
        if (dt < 0) {
            if (!oo[c]++) bad(c " status lines out of order at " ts)
        } else if (dt > gapmax) {
            bad(c " gap: no status line between " last_ts[c] " and " ts " (" dt " s, interval " interval " s)")
        }
    }
    n[c]++
    last_t[c] = t
    last_ts[c] = ts
    last_alive[c] = v["alive"] + 0
    last_restarts[c] = v["restarts"]
    last_faults[c] = v["faults"]
    if (v["alive"] + 0 == 0) next    # no memory or fd figures for a child that is not running

    r = v["restarts"] + 0
    if (!(c in cur_run) || cur_run[c] != r) {   # a restart starts the process over
        cur_run[c] = r
        run_n[c] = 0
        run_first_rss[c] = v["rss_kb"] + 0
    }
    run_n[c]++
    ns[c]++
    rss = v["rss_kb"] + 0
    if (!(c in rss_first)) {
        rss_first[c] = rss
        worst[c] = 0
    }
    rss_last[c] = rss
    gr = rss - run_first_rss[c]
    if (gr > worst[c]) worst[c] = gr
    if (run_n[c] > warmup) {
        f = v["fds"] + 0
        if (!(c in fmin) || f < fmin[c]) fmin[c] = f
        if (!(c in fmax) || f > fmax[c]) fmax[c] = f
        if (!((c, r) in run_fds)) {
            run_fds[c, r] = f
        } else if (run_fds[c, r] != f && !((c, r) in fd_bad)) {
            fd_bad[c, r] = 1
            bad(c " open files changed from " run_fds[c, r] " to " f " at " ts " (restarts=" r ")")
        }
    }
}

END {
    # --- the marks: windows for the planned events ---
    if (hasmarks) {
        pend = 0
        for (m = 1; m <= nm; m++) {
            l = mark_l[m]
            t = mark_t[m]
            if (l == "start") {
                nstart++
                start_t = t
                start_ts = mark_ts[m]
            } else if (l == "stop") {
                nstop++
                stop_t = t
                stop_ts = mark_ts[m]
            } else if (l == "kill_soil" || l == "kill_light") {
                nk++
                kill_t[nk] = t
                kill_ts[nk] = mark_ts[m]
                kill_l[nk] = l
                kill_c[nk] = (l == "kill_soil") ? "gt-sensor-soil" : "gt-sensor-light"
                addwin(kill_c[nk], t - PRE, t + WIN_POST)
            } else if (l == "soil_unplug_start") {
                if (pend) usage_bad("marks: soil_unplug_start at " mark_ts[m] " but the unplug at " up_s_ts[nu] " has no soil_unplug_end")
                pend = 1
                nu++
                up_s[nu] = t
                up_s_ts[nu] = mark_ts[m]
            } else if (l == "soil_unplug_end") {
                if (!pend) usage_bad("marks: soil_unplug_end at " mark_ts[m] " has no soil_unplug_start")
                pend = 0
                up_e[nu] = t
                up_e_ts[nu] = mark_ts[m]
                addwin("gt-sensor-soil", up_s[nu] - PRE, t + WIN_POST)
            }
        }
        if (pend) usage_bad("marks: soil_unplug_start at " up_s_ts[nu] " has no soil_unplug_end")
        if (nstart > 1 || nstop > 1) usage_bad("marks: more than one start or stop mark")
        if (usage_fail) {
            print "soakreport: " usage_fail > "/dev/stderr"
            exit 2
        }
    }

    need = int(hours * 3600 / interval + 0.000001) - 1
    if (need < 1) need = 1
    tend = 0
    for (c in last_t) if (last_t[c] > tend) tend = last_t[c]

    printf "soakreport: %s: %d status lines, interval %d s, %s h, warm-up %d\n", file, nstatus, interval, hours, warmup
    print "process rss_first_kb rss_last_kb rss_growth_kb fds_min fds_max restarts faults samples"
    for (i = 1; i <= nexp; i++) {
        c = expected[i]
        if (n[c] == 0) {
            print c " - - - - - - - 0"
            bad("no status lines for " c)
            continue
        }
        if (ns[c] == 0) {
            printf "%s - - - - - %s %s 0\n", c, last_restarts[c], last_faults[c]
        } else {
            printf "%s %d %d %d %s %s %s %s %d\n", c, rss_first[c], rss_last[c], worst[c],
                (c in fmin) ? fmin[c] : "-", (c in fmax) ? fmax[c] : "-", last_restarts[c], last_faults[c], ns[c]
        }
        if (n[c] < need)
            bad(c " has " n[c] " status lines, wanted at least " need " for " hours " h at " interval " s")
        if (tend - last_t[c] > gapmax)
            bad(c " stopped reporting at " last_ts[c] " while others went on")
        if (last_alive[c] == 0)
            bad(c " was not alive at its last status line (" last_ts[c] ")")
        if (worst[c] > maxrss)
            bad(c " RSS grew " worst[c] " kB within one run (limit " maxrss " kB)")
    }

    # --- the planned events, the injections and the stop ---
    if (hasmarks) {
        if (nstart == 0) bad("marks: no start mark")
        if (nstop == 0) bad("marks: no stop mark")
        if (nstart == 1 && nstop == 1) {
            if (stop_t - start_t < hours * 3600)
                bad("run lasted " sprintf("%.2f", (stop_t - start_t) / 3600) " h from the start mark to the stop mark, wanted " hours " h")
            else
                note("run: " start_ts " to " stop_ts " (" sprintf("%.2f", (stop_t - start_t) / 3600) " h)")
        }

        # Unplanned events: a restart, death, exit, missed heartbeat, kill, degraded state or
        # ERROR line outside a marked window of the same child.  After the stop mark an exit with
        # code 0 is the orderly stop.
        nun = 0
        for (e = 1; e <= ne; e++) {
            k = ev_kind[e]
            if (k == "exited" && ev_code[e] == "0" && nstop > 0 && ev_t[e] >= stop_t - PRE) continue
            classA = (k == "restart" || k == "died" || k == "exited" || k == "heartbeat_missed" ||
                      k == "heartbeat_timeout" || k == "killed" || k == "crash_loop" || k == "no_restart" ||
                      (k == "state" && ev_st[e] == "degraded"))
            if (!classA && ev_lvl[e] != "ERROR") continue
            if (in_window(e)) continue
            nun++
            if (nun <= 10) bad("unplanned event outside the marked windows: " ev_text[e])
        }
        if (nun > 10) bad((nun - 10) " more unplanned events not listed")
        if (nun == 0 && nstart > 0) note("no unplanned restarts, deaths, exits or ERROR lines outside the marked windows")

        for (u = 1; u <= nu; u++) {
            s = up_s[u]
            en = up_e[u]
            rf = first_event("read_failed", "gt-sensor-soil", s - PRE)
            dg = first_state("degraded", "gt-sensor-soil", s - PRE)
            rn = first_state("running", "gt-sensor-soil", en - PRE)
            nr = 0
            for (e = 1; e <= ne; e++)
                if (ev_kind[e] == "restart" && ev_c[e] == "gt-sensor-soil" && ev_t[e] >= s - PRE && ev_t[e] <= en + WIN_POST) nr++
            if (rf == 0)
                bad("soil_unplug at " up_s_ts[u] ": no soil read_failed after it")
            else if (ev_t[rf] - s > DETECT_MAX)
                bad("soil_unplug at " up_s_ts[u] ": first read_failed after " (ev_t[rf] - s) " s (limit " DETECT_MAX " s)")
            if (dg == 0 || ev_t[dg] > en + WIN_POST)
                bad("soil_unplug at " up_s_ts[u] ": no state=degraded for gt-sensor-soil")
            if (rn == 0 || ev_t[rn] - en > RUN_AFTER_REPLUG)
                bad("soil_unplug_end at " up_e_ts[u] ": no state=running for gt-sensor-soil within " RUN_AFTER_REPLUG " s")
            if (rf != 0 && dg != 0 && rn != 0)
                note(sprintf("soil_unplug %s to %s: read_failed +%d s, state=degraded +%d s, state=running +%d s after the replug, %d restart(s) in the window",
                             up_s_ts[u], up_e_ts[u], ev_t[rf] - s, ev_t[dg] - s, ev_t[rn] - en, nr))
        }

        for (q = 1; q <= nk; q++) {
            t = kill_t[q]
            c = kill_c[q]
            d = first_event("died", c, t - PRE)
            if (d == 0 || ev_code[d] != "9" || ev_t[d] - t > DIED_MAX) {
                bad(kill_l[q] " at " kill_ts[q] ": no 'died signal=9' for " c " within " DIED_MAX " s")
                continue
            }
            rn = first_state("running", c, ev_t[d])
            if (rn == 0 || ev_t[rn] - t > RUN_AFTER_KILL)
                bad(kill_l[q] " at " kill_ts[q] ": no state=running for " c " within " RUN_AFTER_KILL " s")
            else
                note(sprintf("%s %s: died signal=9 %+d s, state=running +%d s", kill_l[q], kill_ts[q], ev_t[d] - t, ev_t[rn] - t))
        }

        if (nstop == 1) {
            nstopping = 0
            nstopped = 0
            nexit0 = 0
            for (i = 1; i <= nexp; i++) exit0[expected[i]] = 0
            for (e = 1; e <= ne; e++) {
                if (ev_t[e] < stop_t - PRE) continue
                if (ev_kind[e] == "stopping") nstopping++
                else if (ev_kind[e] == "stopped") nstopped++
                else if (ev_kind[e] == "exited" && ev_code[e] == "0") exit0[ev_c[e]] = 1
            }
            for (i = 1; i <= nexp; i++) {
                if (exit0[expected[i]]) nexit0++
                else bad("stop at " stop_ts ": " expected[i] " did not exit with code 0")
            }
            if (nstopping != 1) bad("stop at " stop_ts ": expected 1 'stopping' line, found " nstopping)
            if (nstopped != 1) bad("stop at " stop_ts ": expected 1 'stopped' line, found " nstopped)
            if (nstopping == 1 && nstopped == 1 && nexit0 == nexp)
                note("stop " stop_ts ": stopping x1, exited code=0 x" nexit0 ", stopped x1")
        }
        if (!hasunit) note("NOTE: the unit result was not checked (no --unit-result)")
        if (!hasstore) note("NOTE: the store was not checked (no --store)")
    }

    # --- the unit's exit ---
    if (hasunit) {
        if (unit_result == "" || unit_status == "")
            bad("unit: no Result= or ExecMainStatus= line in " unitfile)
        else if (unit_result != "success" || unit_status != "0")
            bad("unit: Result=" unit_result " ExecMainStatus=" unit_status " (wanted success and 0)")
        else
            note("unit: Result=success ExecMainStatus=0")
    }

    # --- the store ---
    if (hasstore) {
        want = int(hours * 60 + 0.000001)
        if (store_rc != 0) bad("store: checker exit " store_rc ": " store_why)
        if (store_records == "") {
            bad("store: no record count from the checker")
        } else {
            if (store_records + 0 < want) bad("store: " store_records " records, wanted at least " want " for " hours " h")
            if (store_lost != "" && store_lost + 0 != 0) bad("store: " store_lost " records lost (a gap in the sequence numbers)")
            if (store_synth != "" && store_synth + 0 != 0) bad("store: " store_synth " synthetic records in a real run")
            if (store_rc == 0 && store_records + 0 >= want && store_lost + 0 == 0 && store_synth + 0 == 0)
                note("store: " store_records " records, lost=" (store_lost + 0) ", corrupt=" (store_corrupt + 0) ", synthetic=" (store_synth + 0))
        }
    }

    if (nevid > 0) {
        print "checks:"
        for (i = 1; i <= nevid; i++) print "  " evid[i]
    }
    for (i = 1; i <= nbad; i++) print "FAIL " msgs[i]
    if (nbad == 0) print "PASS"
    exit (nbad > 0)
}
AWK

awk -v interval="$INTERVAL" -v hours="$HOURS" -v warmup="$WARMUP" -v maxrss="$MAX_RSS_GROWTH_KB" \
    -v file="$JOURNAL" \
    -v hasmarks="$([ -n "$MARKS" ] && echo 1 || echo 0)" -v marksfile="$MARKS" \
    -v hasunit="$HAS_UNIT" -v unitfile="$UNIT" -v unit_result="$UNIT_RESULT" -v unit_status="$UNIT_STATUS" \
    -v hasstore="$HAS_STORE" -v store_rc="$STORE_RC" -v store_records="$STORE_RECORDS" \
    -v store_lost="$STORE_LOST" -v store_corrupt="$STORE_CORRUPT" -v store_synth="$STORE_SYNTH" \
    -v store_why="$STORE_WHY" \
    -f "$WORK/report.awk" ${MARKS:+"$MARKS"} "$JOURNAL"
exit $?
