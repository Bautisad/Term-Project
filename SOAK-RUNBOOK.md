# Soak runbook: the 48-hour run, its rehearsal and the L6 hand run

How a soak is made on the Pi, what to capture, and how the result is judged. It covers DESIGN test P4 (48-hour soak with real sensors) and the real-hardware part of S6 and L6 (fault detection). Duke runs these on the Pi and pastes the output; Andrew reads the evidence and diagnoses anything that looks like hardware.

Nothing in this file has been run on the Pi yet. Where an expected line is given, it comes from the code (`src/supervisor`, `src/sensors`) and the tests, not from a Pi paste.

## The order

1. [Pre-flight](#1-pre-flight-service-stopped) (service stopped).
2. [L6 hand run](#2-l6-hand-run-service-stopped) (service stopped).
3. [Rehearsal](#3-rehearsal-30-minutes). It must give PASS before the real run is started.
4. [The 48-hour run](#4-the-48-hour-run), [capture](#5-capture) and [report](#6-the-report).
5. [Record the results](#7-recording-the-results).

## What the soak asks (DESIGN P4)

2,880 records with no unexplained sequence gaps, RSS growth under 1 MB per process, constant fd counts, and 0 unplanned supervisor restarts. These targets are not edited. A miss is recorded with its reason.

Inside the 48 hours three faults are injected on purpose: one 10-minute soil unplug, one `kill -9` of the soil process, one `kill -9` of the light process. Each is written to a marks file by `soakmark.sh`, so `soakreport.sh` can tell a planned restart from an unplanned one. "0 unplanned restarts" means none outside a marked window.

## Where things are

Everything below runs on the Pi, from the deploy directory, with the unit stopped unless a step says otherwise.

- Binaries and scripts: `~/greenthumb/bin` (from `make deploy`). It holds `gt-supervisor`, `gt-hub`, `gt-sensor-soil`, `gt-sensor-light`, `gt-interface`, `gt-logcheck`, `gt-csv`, `soakmark.sh` and `soakreport.sh`.
- The unit: `/etc/systemd/system/greenthumb.service`. It runs `gt-supervisor` with no flags as `dbautista`. The hub socket is `/run/greenthumb/hub.sock` and the store is `/var/lib/greenthumb/store`. **Both come from the unit. Do not start `gt-supervisor` by hand for a soak or a rehearsal.**
- Work files: `~/soak/<run name>/`. They are raw evidence, kept on the Pi and not committed (the same rule as the S2/S3 CSVs).

```
B=~/greenthumb/bin
mkdir -p ~/soak/l6 ~/soak/rehearsal ~/soak/run
```

`make deploy` copies binaries only. If the code changed, run it from the dev machine first, and note `git log -1` of what was deployed in the paste.

## 1. Pre-flight (service stopped)

```
sudo systemctl stop greenthumb; systemctl is-active greenthumb
```

Expected: `inactive`. Then paste each of these.

1. **Clock.** The Pi has no network time and `soakmark.sh` refuses a clock before 2026. Set it, then show it:
   `sudo timedatectl set-time "YYYY-MM-DD HH:MM:SS"` (the time is read in the Pi's time zone), then `timedatectl` and `date -u`.
2. **Journal kept on disk.** `ls /var/log/journal` must list a directory, and `journalctl --disk-usage` must answer. If `/var/log/journal` does not exist, the journal is in RAM and a reboot loses it: set `Storage=persistent` in `/etc/systemd/journald.conf`, run `sudo systemctl restart systemd-journald`, and tell Andrew.
3. **Groups.** `getent group i2c gpio` must print both lines.
4. **Power.** `vcgencmd get_throttled` must say `throttled=0x0`.
5. **Disk.** `df -h /var/lib/greenthumb ~` shows free space.
6. **Store.** `ls /var/lib/greenthumb/store` is empty or missing for the real run. If there is an old store (a rehearsal), move it aside: `sudo mv /var/lib/greenthumb/store /var/lib/greenthumb/store-old-$(date -u +%Y%m%d)`. The record check counts the whole directory, so an old store would pass the count for the wrong reason.
7. **Raw hardware evidence** (CLAUDE.md asks for these with any hardware problem; take them now so there is a "before"):
   ```
   dmesg | tail -50
   sudo i2cdetect -y 1
   i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2     # first read after i2cdetect is 0xffff: discard it
   i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2
   gpioget -c gpiochip4 17
   grep -i 'i2c\|gpio' /proc/interrupts
   ```
   Expected: `0x36` on the bus, a soil value near the 889 to 977 seen before, `dmesg` with no i2c, gpio or under-voltage lines.

Two traps:
- `i2cdetect` makes the **next** soil read `0xffff` (4 of 4 times so far; the mechanism is unexplained, Andrew diagnoses). Run it only with the service stopped, or at the one planned point in the unplug (below), never right before a replug.
- `gpioget` fails with "busy" while `gt-sensor-light` holds the line. Take GPIO17 readings with the service stopped.

## 2. L6 hand run (service stopped)

Test L6: a disconnected DO wire must be logged as INVALID and never stored as a real bright or dark reading, and covering the sensor must be recorded as real darkness.

**What this run can and cannot show.** The light process marks a window INVALID only for two reasons (`src/sensors/light_logic.c`): no change for `--stuck` milliseconds, or more than `--chatter` changes in one window. The default `--stuck` is 48 hours, and the supervisor cannot pass flags, so a hand run with a short `--stuck` is the only way to see INVALID on the real line.

So covering the sensor for longer than `--stuck` is **also** INVALID, because a dark room and a pulled wire look the same to the process (DESIGN "L6 why"). The cover step below is kept short, under the limit, on purpose. The run shows that the stuck timer fires on a disconnected line, not that the process can tell a wire from a dark room. That limit is written down, not hidden.

Pins: the module's DO output reads active (high) when dark and inactive (low) when lit; the driver inverts it.

**A. Pin levels, with nothing running.** Three `gpioget -c gpiochip4 17` reads each for: sensor lit, sensor fully covered, DO wire disconnected (power and ground still connected). Paste them. The disconnected reading shows whether a floating input sits at one level or chatters.

**B. The processes.** A stand-in hub is needed so the sensor has somewhere to send. Use the real `gt-hub` on throwaway paths (the 2026-11-02 run used a Python listener that is not in the repo).

```
cd ~/soak/l6
rm -rf /tmp/gt-l6-store /tmp/gt-l6.sock
$B/gt-hub --sock /tmp/gt-l6.sock --store /tmp/gt-l6-store 2>&1 | tee hub.log &       # terminal A
$B/gt-sensor-light --hub /tmp/gt-l6.sock --window 2000 --stuck 120000 2>&1 | tee light.log   # terminal B
```

By hand the processes log `WARN ... no control socket on fd 3: running without heartbeats`. That is expected. The hub may also log that the soil sensor is silent, because soil is not started in this run.

`--stuck 120000` is 2 minutes. Every phase below must be shorter than that since the last change, until the disconnect step.

**C. Steps** (note the clock time of each in the paste, with `date -u`):

1. Lit, 30 s. Expect no `WARN` line.
2. Cover fully, hold 60 s. Expect no `WARN` line: this is real darkness.
3. Uncover, hold 30 s. Expect no `WARN` line.
4. Disconnect the DO wire and hold at least 150 s. Expect `WARN light status=0x01 level=.. edges=.. seq=..` within about 2 minutes (sooner if the floating pin chatters). Paste the level and edges it shows.
5. Reconnect the wire, then cover and uncover once. Expect `INFO light status=0x00 ...`. A recovery needs a change in the level, so reconnecting alone may stay INVALID if the level did not change. Say which happened.
6. Ctrl-C the light process, then the hub. Both must exit 0 (`echo $?`).
7. Show what the hub stored: `$B/gt-csv --dir /tmp/gt-l6-store`. Look at `light_valid` for the rows from steps 1 to 3 (expect valid, with `light` 0 for the covered period) and for the disconnect (expect not valid).

Paste: the `gpioget` reads, `light.log`, `hub.log`, the CSV, and the exit codes. Clean up: `rm -rf /tmp/gt-l6-store /tmp/gt-l6.sock`.

Result goes in `HARDWARE-CHECKS.md` as a new dated entry, with what was not covered. L6 is not called a pass from this file alone.

## 3. Rehearsal (30 minutes)

A scaled run to prove the whole pipeline (journal, marks, report) before 48 hours are spent. It uses the real unit with a temporary status interval of 60 s instead of 1 hour. The report is the same one, with `--hours 0.5 --interval 60`.

```
sudo systemctl edit greenthumb        # in the editor, add these three lines, then save
#   [Service]
#   ExecStart=
#   ExecStart=/home/dbautista/greenthumb/bin/gt-supervisor --status 60
cd ~/soak/rehearsal
```

Make sure the store is empty (pre-flight step 6), then run this schedule. Each mark is chained to the action it names, in one command line.

| Minute | Action | Command |
|---|---|---|
| 0 | start | `$B/soakmark.sh marks.log start && sudo systemctl start greenthumb` |
| 5 | unplug the soil data wire, wait 3 min, replug | `$B/soakmark.sh marks.log soil_unplug_start`, then after 3 min `$B/soakmark.sh marks.log soil_unplug_end` (mark just before pulling and just after replugging) |
| 12 | kill the soil process | `$B/soakmark.sh marks.log kill_soil && sudo pkill -9 -x gt-sensor-soil` |
| 18 | kill the light process | `$B/soakmark.sh marks.log kill_light && sudo pkill -9 -x gt-sensor-light` |
| 30 | stop | `$B/soakmark.sh marks.log stop && sudo systemctl stop greenthumb` |

Then capture and report as in sections 5 and 6, with `--hours 0.5 --interval 60`. Undo the override and move the rehearsal store aside:

```
sudo rm /etc/systemd/system/greenthumb.service.d/override.conf
sudo systemctl daemon-reload
sudo mv /var/lib/greenthumb/store /var/lib/greenthumb/store-rehearsal-$(date -u +%Y%m%d)
```

Gate: the report says PASS, and each injection's line shows in its "checks" output. If the report fails, fix the checker or this runbook first. Do not start the 48-hour run on a failed rehearsal. A fix in `tools/` is Duke's; anything in `src/` or `systemd/` goes to Andrew.

## 4. The 48-hour run

Pre-flight and the rehearsal gate come first. Then `cd ~/soak/run`.

Before the first kill, check that each name matches exactly one process while the service is running: `pgrep -x gt-sensor-soil` and `pgrep -x gt-sensor-light` each print one pid. (Process names are cut at 15 characters; `gt-sensor-light` is exactly 15.)

**Start.** T is the time of the start mark.

```
$B/soakmark.sh marks.log start && sudo systemctl start greenthumb
```

About a minute later, `journalctl -u greenthumb --since "-2min" --no-pager` must show `start child=` and `state=running` for `gt-hub`, `gt-sensor-soil`, `gt-sensor-light` and `gt-interface`. Paste it. The first hourly `status` line comes at T+1 h.

**Schedule.** Times are targets, not exact: the report works from the marks, not the clock. Hours 0 to 6 and 42 to 48 are clean baseline. Do not open the LAN page near an hour mark; the interface briefly holds extra file descriptors and would show up as an fd change.

| When | Event | Commands |
|---|---|---|
| T | start | as above |
| T+6 h | unplug the soil data wire for 10 min | `$B/soakmark.sh marks.log soil_unplug_start`, pull the wire. At about 5 min: one `sudo i2cdetect -y 1` (the bus should show no `36`; paste it). After 10 min, replug and `$B/soakmark.sh marks.log soil_unplug_end`. **Nothing from i2c after the replug.** |
| T+18 h | kill the soil process | `$B/soakmark.sh marks.log kill_soil && sudo pkill -9 -x gt-sensor-soil` |
| T+30 h | kill the light process | `$B/soakmark.sh marks.log kill_light && sudo pkill -9 -x gt-sensor-light` |
| T+48 h | orderly stop | `$B/soakmark.sh marks.log stop && sudo systemctl stop greenthumb` |

`soakmark.sh` writes `<UTC time>Z label` to `marks.log` and echoes the line. It refuses a second `start` or `stop`, a `soil_unplug_end` with no open start, and a clock before 2026. Paste each echoed line.

What the code does under each fault, so the lines are not a surprise:

- **Soil unplug:** each 60 s cycle logs `ERROR soil read_failed errno=.. attempt=3/3`, and the supervisor logs `WARN child=gt-sensor-soil state=degraded`. After 5 failed cycles (about 5 min) the soil process logs `ERROR soil giving_up failed_cycles=5`, exits 1 and is restarted, then does the same again. Ten minutes is about two planned restarts, and no crash loop. The hub keeps writing records with the soil field INVALID. After the replug the first good cycle gives `state=running`.
- **`kill -9` of a sensor:** `died signal=9`, `restart=1 delay=1s`, then `state=running` after the first heartbeat and reading (the light sensor needs a full 60 s window). The other sensor and the hub are unaffected and no record is skipped.
- **Orderly stop:** `stopping`, four `exited code=0`, `stopped`; the unit ends with `Result=success` and `ExecMainStatus=0`.

If anything looks wrong during the run, paste first and fix later: `dmesg | tail -50`, `journalctl -u greenthumb --since "-30min" --no-pager`, and, for a hardware-looking problem, the evidence set from pre-flight step 7. No fix is proposed from a verbal description. The unplug and the kills are marked, so do not "fix" them; an unmarked event is the finding.

## 5. Capture

Right after the stop, in the run's directory (for the rehearsal use `~/soak/rehearsal`):

```
cd ~/soak/run
START=<the start mark time, UTC, minus one minute, like "2026-11-06 14:03:00 UTC">
sudo journalctl -u greenthumb -o short-iso --no-pager --since "$START" > soak-journal.txt
systemctl show greenthumb -p Result -p ExecMainStatus > unit-result.txt
sudo cp -a /var/lib/greenthumb/store store-copy
dmesg | tail -50 > dmesg-after.txt
sha256sum soak-journal.txt unit-result.txt marks.log
cat unit-result.txt marks.log
```

Keep `soak-journal.txt`, `unit-result.txt`, `marks.log`, `store-copy/` and `dmesg-after.txt` on the Pi, untracked. The journal must start after any earlier run (the `--since` time), or the old restarts show up as unplanned events.

## 6. The report

```
$B/soakreport.sh --journal soak-journal.txt --marks marks.log \
    --unit-result unit-result.txt --store store-copy --checker $B/gt-logcheck
```

For the rehearsal add `--hours 0.5 --interval 60`. Defaults are `--hours 48 --interval 3600 --warmup 1`. Exit 0 is PASS, 1 is some check failed, 2 is bad arguments or a bad marks file.

It prints one row per process (`rss_first_kb rss_last_kb rss_growth_kb fds_min fds_max restarts faults samples`), a "checks" list showing what each injection produced (for example `kill_soil ...: died signal=9 +0 s, state=running +4 s`), and then `PASS` or one `FAIL ...` line per failed check. It fails on:

- a process with no status line, too few, a gap of more than 1.5 intervals, one that stopped reporting while the others went on, or one that was not alive at its last line;
- RSS growth over 1 MB (1024 kB) within a run (a run ends where a process's restart count rises, because a restart starts the process over);
- a changed open file count within a run after the warm-up;
- with `--marks`: a restart, death, exit, missed heartbeat, kill, degraded state or ERROR line outside a marked window of the same process (a window runs from 5 s before its mark to 150 s after); an unplug not logged as `read_failed` within 150 s, not shown as `state=degraded`, or not followed by `state=running` within 180 s of the replug; a kill not logged as `died signal=9` within 5 s or not followed by `state=running` within 120 s; a stop that is not `stopping`, four `exited code=0` and `stopped`; a run shorter than `--hours` between the start and stop marks;
- with `--unit-result`: `Result` not `success`, or `ExecMainStatus` not 0;
- with `--store`: the checker rejects the store, fewer than `hours * 60` records, a lost record (a gap in the sequence numbers), or synthetic records.

The result is called a pass only if the report says `PASS` on the raw export. Paste the whole output.

## 7. Recording the results

- `DESIGN.md` Part 4: add a "Measured results (P4)" table in the same shape as P1 and P2, with the target unchanged and the report's figures. State that S6 "short it" and "hold constant" were not run.
- `HARDWARE-CHECKS.md`: a new dated entry for the L6 hand run, and one for the rehearsal and the 48-hour run, each with the pasted output and a "Not covered" list.
- Issue #30 stays open until Duke decides to close it.

## Known limits

These are stated here so the result is not read as more than it is.

- **S6 is partly run.** The soil unplug is run inside the soak. Shorting a wire (pin risk) and holding a reading constant (60 identical cycles) are not run, and are recorded as not run with that reason.
- **A DO-wire disconnect inside the soak may never be flagged.** The light process only marks INVALID after 48 hours without a change (the default) or on chatter, and the supervisor passes no flags. L6 is therefore a separate hand run with a short `--stuck`.
- **Dark and disconnected look the same** to the light process: both are "no change". A long enough cover is INVALID too. A recovery after a reconnect needs a change in the level.
- **Planned faults restart processes.** "0 unplanned restarts" means none outside the marked windows.
- **No record count from the supervisor.** It has no counter (it would need a change to the frozen `ipc.h`). Records are counted from the store by `gt-logcheck`.
- **The `0xffff` after `i2cdetect`** is unexplained; it is avoided, not understood.
- **Not changed here, Andrew's to weigh:** the hub exiting 1 when `/run/greenthumb` is missing, and the 48-hour default for the light stuck limit.
