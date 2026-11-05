#!/bin/sh
# Soak mark: writes one line to the marks file that soakreport.sh reads, so the report knows
# which restarts and errors of a soak were planned.  Run it just before the action it names,
# in one command line (the report also accepts a mark typed up to 5 s after the action), for
# example:  soakmark.sh marks.log kill_soil && sudo pkill -9 -x gt-sensor-soil
#
# usage: soakmark.sh MARKS_FILE LABEL
#   LABEL  start              the service is started
#          soil_unplug_start  the soil sensor's data wire is about to be pulled
#          soil_unplug_end    the wire has been put back
#          kill_soil          the soil process is about to be killed with kill -9
#          kill_light         the light process is about to be killed with kill -9
#          stop               the service is about to be stopped (systemctl stop)
# Appends "<UTC ISO time>Z LABEL" and prints the same line.  Refuses, and writes nothing, for
# a second start or stop, an unplug_end with no open unplug_start, an unplug_start while one is
# open, and a clock that was never set (a year before 2026: the Pi has no network time, so
# set it first).
# Exit 0: written.  1: refused or could not write.  2: bad arguments.

usage() {
    sed -n '2,/^# Exit 0/p' "$0" | sed 's/^# \{0,1\}//'
}

usage_error() {
    echo "soakmark: $*" >&2
    usage >&2
    exit 2
}

refuse() {
    echo "soakmark: $*" >&2
    exit 1
}

case $1 in
-h | --help)
    usage
    exit 0
    ;;
esac
[ $# -eq 2 ] || usage_error "need a marks file and a label"
FILE=$1
LABEL=$2
case $LABEL in
start | soil_unplug_start | soil_unplug_end | kill_soil | kill_light | stop) ;;
*) usage_error "unknown label: $LABEL" ;;
esac

# count TEXT: how many lines of the marks file end in " TEXT" (0 if there is no file yet).
count() {
    n=$(grep -c " $1\$" "$FILE" 2>/dev/null)
    echo "${n:-0}"
}

case $LABEL in
start | stop)
    [ "$(count "$LABEL")" -eq 0 ] || refuse "$FILE already has a $LABEL mark"
    ;;
soil_unplug_start)
    [ "$(count soil_unplug_start)" -eq "$(count soil_unplug_end)" ] ||
        refuse "the unplug in $FILE has no soil_unplug_end yet"
    ;;
soil_unplug_end)
    [ "$(count soil_unplug_start)" -eq $(($(count soil_unplug_end) + 1)) ] ||
        refuse "$FILE has no open soil_unplug_start"
    ;;
esac

NOW=$(date -u +%Y-%m-%dT%H:%M:%SZ) || refuse "date failed"
YEAR=${NOW%%-*}
[ "$YEAR" -ge 2026 ] 2>/dev/null || refuse "the clock reads $NOW: set the time first"

printf '%s %s\n' "$NOW" "$LABEL" >>"$FILE" || refuse "cannot write $FILE"
echo "$NOW $LABEL"
exit 0
