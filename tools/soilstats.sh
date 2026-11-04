#!/bin/sh
# Soil sensor stats: turns a soilread.sh capture into the numbers for tests S1-S5
# (DESIGN Part 1) and says PASS or FAIL against each target.
#
# usage: soilstats.sh noise    FILE --range DRY WET [--label L]   S2: median-of-5 stdev under 1%
#        soilstats.sh repeat   FILE --range DRY WET [--label L]   S3: reinsertion spread under 5%
#        soilstats.sh response FILE [--label L]                   S4: settles within 5% in 10 s
#        soilstats.sh calib    FILE --dry-weight G                S1: monotonic, R^2 0.9 or better
#        soilstats.sh drift    FILE --range DRY WET [--label L]   S5: drift under 2% over 48 h
#   FILE          noise, repeat, response, drift: a soilread.sh CSV.  calib: lines of
#                 "weight_g raw", one per moisture level (pot weight and the median raw read)
#   --range D W   the dry-to-wet raw range the percentages are taken of (README: 565 and 977)
#   --label L     use only the lines with this label
#   --dry-weight  the weight of the dry pot in grams; water content is weight minus this
#
# Reads that failed (empty raw) are skipped and counted.  0xffff reads are included and
# reported, because an outlier is evidence, not noise to hide.  The response test takes time
# zero from the moment the reading leaves its starting value, so it does not matter when the
# capture was started.  The drift test needs at least 46 of the 48 hours with 5 or more
# reads each, from a single capture.
# Exit 0: PASS.  1: FAIL, with the reason.  2: bad arguments.

SUB=
FILE=
DRY=
WET=
LABEL=
DRYW=

usage() {
    sed -n '2,/^# Exit 0/p' "$0" | sed 's/^# \{0,1\}//'
}

usage_error() {
    echo "soilstats: $*" >&2
    usage >&2
    exit 2
}

is_number() {
    case $1 in
    '' | . | *[!0-9.]* | *.*.*) return 1 ;;
    esac
    return 0
}

[ $# -ge 1 ] || usage_error "give a subcommand"
case $1 in
-h | --help)
    usage
    exit 0
    ;;
noise | repeat | response | calib | drift) SUB=$1 ;;
*) usage_error "unknown subcommand: $1" ;;
esac
shift
[ $# -ge 1 ] || usage_error "$SUB needs a FILE"
FILE=$1
shift

while [ $# -gt 0 ]; do
    case $1 in
    --range)
        [ $# -ge 3 ] || usage_error "--range needs two values"
        is_number "$2" || usage_error "--range needs numbers, got '$2'"
        is_number "$3" || usage_error "--range needs numbers, got '$3'"
        DRY=$2
        WET=$3
        shift 3
        ;;
    --label)
        [ $# -ge 2 ] || usage_error "--label needs a value"
        LABEL=$2
        shift 2
        ;;
    --dry-weight)
        [ $# -ge 2 ] || usage_error "--dry-weight needs a value"
        is_number "$2" || usage_error "--dry-weight needs a number, got '$2'"
        DRYW=$2
        shift 2
        ;;
    *) usage_error "unknown argument: $1" ;;
    esac
done

[ -f "$FILE" ] && [ -r "$FILE" ] || usage_error "cannot read $FILE"
case $SUB in
noise | repeat | drift)
    [ -n "$DRY" ] || usage_error "$SUB needs --range DRY WET"
    awk -v d="$DRY" -v w="$WET" 'BEGIN { exit !(w + 0 > d + 0) }' ||
        usage_error "--range needs WET above DRY, got $DRY $WET"
    ;;
calib)
    [ -n "$DRYW" ] || usage_error "calib needs --dry-weight G"
    ;;
esac

# Shared awk functions.
LIB='
function abs(x) { return x < 0 ? -x : x }
function verdict(ok, msg) {
    if (ok) { print "PASS"; exit 0 }
    print "FAIL " msg
    exit 1
}
function mean(a, lo, hi,   i, s) {
    for (i = lo; i <= hi; i++) s += a[i]
    return s / (hi - lo + 1)
}
function stdev(a, lo, hi,   i, m, q) {
    if (hi - lo < 1) return 0
    m = mean(a, lo, hi)
    for (i = lo; i <= hi; i++) q += (a[i] - m) * (a[i] - m)
    return sqrt(q / (hi - lo))
}
function median(a, lo, hi,   i, j, k, x, b) {
    for (i = lo; i <= hi; i++) b[++k] = a[i]
    for (i = 2; i <= k; i++) {
        x = b[i]
        for (j = i - 1; j >= 1 && b[j] > x; j--) b[j + 1] = b[j]
        b[j + 1] = x
    }
    if (k % 2) return b[(k + 1) / 2]
    return (b[k / 2] + b[k / 2 + 1]) / 2
}
function summary(name) {
    printf "%s reads=%d errors=%d ffff=%d", name, n, errors + 0, ffff + 0
    if (malformed) printf " malformed=%d", malformed
    printf "\n"
    if (ffff) print "NOTE " ffff " read(s) were 0xffff and are included"
    if (malformed) verdict(0, malformed " malformed line(s) in the file")
}
'

# Reads the soilread.sh CSV into t[] (elapsed_s) and v[] (raw), n of them.
CSVREAD='
/^#/ { next }
/^$/ { next }
$1 == "time_utc" { next }
{
    if (NF != 6 || $2 !~ /^[0-9]+$/) { malformed++; next }
    if (label != "" && $3 != label) next
    if ($5 == "") { errors++; next }
    if ($5 !~ /^[0-9]+$/) { malformed++; next }
    n++
    t[n] = $2 + 0
    v[n] = $5 + 0
    if (v[n] == 65535) ffff++
}
'

NOISE='
END {
    range = wet - dry
    summary("noise")
    if (n < 10) verdict(0, "not enough reads (" n "), need at least 10")
    s1 = stdev(v, 1, n)
    for (i = 1; i + 4 <= n; i += 5) m5[++g] = median(v, i, i + 4)
    s5 = stdev(m5, 1, g)
    p1 = s1 / range * 100
    p5 = s5 / range * 100
    printf "single reads: mean=%.1f stdev=%.2f (%.2f%% of the range %g)\n", mean(v, 1, n), s1, p1, range
    printf "median of 5: groups=%d stdev=%.2f (%.2f%% of the range), target under 1%%\n", g, s5, p5
    verdict(p5 < 1, sprintf("median-of-5 stdev is %.2f%% of the range, the target is under 1%%", p5))
}
'

REPEAT='
END {
    range = wet - dry
    summary("repeat")
    if (n < 2) verdict(0, "not enough reads (" n "), need at least 2")
    lo = hi = v[1]
    for (i = 2; i <= n; i++) {
        if (v[i] < lo) lo = v[i]
        if (v[i] > hi) hi = v[i]
    }
    p = (hi - lo) / range * 100
    printf "reinsertions: min=%d max=%d spread=%d (%.2f%% of the range %g), target under 5%%\n", lo, hi, hi - lo, p, range
    verdict(p < 5, sprintf("spread is %.2f%% of the range, the target is under 5%%", p))
}
'

RESPONSE='
END {
    summary("response")
    if (n < 20) verdict(0, "not enough reads (" n "), need at least 20")
    fin = median(v, n - 9, n)
    tol = 0.05 * abs(fin)
    r0 = v[1]
    for (i = 1; i <= n; i++) if (abs(v[i] - r0) > tol) { step = i; break }
    if (!step) verdict(0, "no step found: no read moved more than 5% of the final value from the first read")
    last_out = 0
    for (i = 1; i <= n; i++) if (abs(v[i] - fin) > tol) last_out = i
    if (last_out == n) verdict(0, "never settled: the last read is still more than 5% from the final value")
    settle = t[last_out + 1] - t[step - 1]
    if (settle < 0) settle = 0
    printf "response: start=%g final=%g (median of the last 10), tolerance=%.1f\n", r0, fin, tol
    printf "response: step at %gs, settled at %gs, settle time %gs, target 10 s or less\n", t[step - 1], t[last_out + 1], settle
    verdict(settle <= 10, sprintf("settle time is %g s, the target is 10 s or less", settle))
}
'

DRIFT='
END {
    range = wet - dry
    summary("drift")
    if (n < 2) verdict(0, "not enough reads (" n "), need at least 2")
    for (i = 1; i <= n; i++) {
        if (i > 1 && t[i] < t[i - 1]) verdict(0, "elapsed_s goes backwards at read " i ": use one capture, or --label to pick one")
        h = int(t[i] / 3600)
        if (!(h in hs)) hs[h] = i
        he[h] = i
        if (h > hmax) hmax = h
    }
    for (h = 0; h <= hmax; h++) {
        if ((h in hs) && he[h] - hs[h] + 1 >= 5) {
            u++
            X[u] = h
            Y[u] = median(v, hs[h], he[h])
        }
    }
    if (u < 46) verdict(0, "only " u " hourly medians, need at least 46 of 48 hours")
    for (i = 1; i <= u; i++) { sx += X[i]; sy += Y[i]; sxx += X[i] * X[i]; sxy += X[i] * Y[i] }
    slope = (u * sxy - sx * sy) / (u * sxx - sx * sx)
    drift = slope * 48
    p = abs(drift) / range * 100
    printf "drift: %d hourly medians, first=%g last=%g, slope=%.4f counts/hour\n", u, Y[1], Y[u], slope
    printf "drift: %.2f counts over 48 h (%.2f%% of the range %g), target under 2%%\n", drift, p, range
    verdict(p < 2, sprintf("drift is %.2f%% of the range over 48 h, the target is under 2%%", p))
}
'

CALIB='
/^#/ { next }
/^[ \t]*$/ { next }
{
    if (NF != 2 || $1 !~ /^[0-9]+(\.[0-9]+)?$/ || $2 !~ /^[0-9]+$/) { malformed++; next }
    n++
    w[n] = $1 - dryw
    r[n] = $2 + 0
}
END {
    printf "calib levels=%d dry_weight=%g g\n", n, dryw
    if (malformed) verdict(0, malformed " malformed line(s), expected: weight_g raw")
    if (n < 5) verdict(0, "need 5 moisture levels, got " n)
    for (i = 1; i <= n; i++) if (w[i] < 0) verdict(0, sprintf("level %d weighs less than the dry pot", i))
    for (i = 2; i <= n; i++) {
        x = w[i]; y = r[i]
        for (j = i - 1; j >= 1 && w[j] > x; j--) { w[j + 1] = w[j]; r[j + 1] = r[j] }
        w[j + 1] = x; r[j + 1] = y
    }
    for (i = 2; i <= n; i++) if (w[i] == w[i - 1]) verdict(0, "two levels have the same weight")
    mono = 1
    for (i = 2; i <= n; i++) if (r[i] <= r[i - 1]) { mono = 0; bad = i; break }
    for (i = 1; i <= n; i++) {
        printf "level %d: water=%g g raw=%d\n", i, w[i], r[i]
        sx += w[i]; sy += r[i]; sxx += w[i] * w[i]; syy += r[i] * r[i]; sxy += w[i] * r[i]
    }
    den = (n * sxx - sx * sx) * (n * syy - sy * sy)
    if (den <= 0) verdict(0, "all the raw reads are equal")
    cov = n * sxy - sx * sy
    r2 = cov * cov / den
    slope = cov / (n * sxx - sx * sx)
    printf "fit: raw = %.3f * water_g + %.1f, R^2 = %.4f, target 0.9 or better\n", slope, (sy - slope * sx) / n, r2
    printf "monotonic: %s\n", (mono ? "yes" : "no, raw does not rise at level " bad)
    printf "measured: dry=%d wet=%d (raw at the least and the most water)\n", r[1], r[n]
    if (!mono) msg = "raw does not rise with water at level " bad "; "
    if (r2 < 0.9) msg = msg sprintf("R^2 is %.3f, the target is 0.9 or better", r2)
    verdict(mono && r2 >= 0.9, msg)
}
'

case $SUB in
noise) exec awk -F, -v label="$LABEL" -v dry="$DRY" -v wet="$WET" "$LIB$CSVREAD$NOISE" "$FILE" ;;
repeat) exec awk -F, -v label="$LABEL" -v dry="$DRY" -v wet="$WET" "$LIB$CSVREAD$REPEAT" "$FILE" ;;
response) exec awk -F, -v label="$LABEL" "$LIB$CSVREAD$RESPONSE" "$FILE" ;;
drift) exec awk -F, -v label="$LABEL" -v dry="$DRY" -v wet="$WET" "$LIB$CSVREAD$DRIFT" "$FILE" ;;
calib) exec awk -v dryw="$DRYW" "$LIB$CALIB" "$FILE" ;;
esac
