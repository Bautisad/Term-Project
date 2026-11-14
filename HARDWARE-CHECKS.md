# Hardware checks on the Pi

Evidence from the Pi (`greenthumb`, Pi 5), pasted from the terminal. Each entry says what was run and what came back. Where something was not recorded, the entry says so.

## 2026-11-02: raw hardware reads and the sensor drivers

### Test suite

`make test` on the Pi: 100% passed, 0 failed out of 17 (total 51.08 s). This includes `supervisor_behavior`, `ipc_roundtrip`, `soil_process` and `light_process`. `make asan` and `make memcheck` were not run in this session.

### Raw reads (no project code)

```
$ sudo i2cdetect -y 1
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- -- -- --
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
30: -- -- -- -- -- -- 36 -- -- -- -- -- -- -- -- --
40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
60: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
70: -- -- -- -- -- -- -- --

$ i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2
0xff 0xff
$ i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2
0x03 0x9f
$ i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2
0x03 0x7e
$ i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2
0x03 0x79

$ gpioget -c gpiochip4 17
"17"=active
$ gpioget -c gpiochip4 17
"17"=active
$ gpioget -c gpiochip4 17
"17"=inactive

$ vcgencmd get_throttled
throttled=0x0
```

- The soil sensor is on the bus at 0x36.
- The first read after `i2cdetect` was `0xffff` again. That is 4 of 4 times (the README's earlier count was 3 of 3). The next three reads were 927, 894 and 889.
- The three `gpioget` reads changed from `active` to `inactive`, so the pin moves. The cover and lamp state for each read was not noted. The driver run below shows lamp on giving `level=1`.
- `get_throttled` is `0x0`.
- `dmesg | tail -50` held only boot-time lines (the last one about 9 s after boot): Bluetooth, Wi-Fi firmware, ethernet. It had no i2c, gpio or under-voltage messages. The lines are not reproduced here.

### Drivers, run by hand against a stand-in hub

A throwaway Python listener on `/tmp/gt-check.sock` (not in the repo) printed each message the sensor processes sent. The binaries were started with `--hub /tmp/gt-check.sock`. They log `no control socket on fd 3: running without heartbeats` when run by hand, which is expected.

Listener output from the soil and light runs (the paste does not separate the two runs). The soil process sent one message, so its once-a-minute cadence was not observed:

```
SOIL  seq=0 raw=914 (0x0392) status=0x00
LIGHT seq=0 level=0 bright_ms=0 edges=0 status=0x00
LIGHT seq=1 level=0 bright_ms=0 edges=0 status=0x00
LIGHT seq=2 level=0 bright_ms=0 edges=0 status=0x00
LIGHT seq=3 level=0 bright_ms=0 edges=0 status=0x00
LIGHT seq=4 level=0 bright_ms=280 edges=2 status=0x00
LIGHT seq=5 level=1 bright_ms=1674 edges=1 status=0x00
LIGHT seq=6 level=0 bright_ms=490 edges=1 status=0x00
LIGHT seq=7 level=0 bright_ms=0 edges=0 status=0x00
LIGHT seq=8 level=0 bright_ms=0 edges=0 status=0x00
LIGHT seq=9 level=0 bright_ms=0 edges=0 status=0x00
LIGHT seq=10 level=0 bright_ms=0 edges=0 status=0x00
```

- `raw=914` is inside the 889 to 927 range of the raw reads above, so the driver and `i2ctransfer` agree.
- The light windows are consistent. Seq 4 starts dark, has 2 edges and ends dark. Seq 5 starts dark, has 1 edge and ends bright (1674 ms bright). Seq 6 starts bright, has 1 edge and ends dark. With the lamp on, `level` was 1 (bright), which matches the inverted GPIO17 mapping in DESIGN.md.
- Every message had `status=0x00`.

Stopping with Ctrl-C (the terminal printed a stray `.` after the exit code because of a typo; the code is 0):

```
$ build/gt-sensor-soil --hub /tmp/gt-check.sock
2026-11-02T13:17:08Z WARN soil no control socket on fd 3: running without heartbeats
^C2026-11-02T13:17:14Z INFO soil stopping
$ echo $?
0

$ build/gt-sensor-light --hub /tmp/gt-check.sock --window 2000
2026-11-02T13:17:27Z WARN light no control socket on fd 3: running without heartbeats
^C2026-11-02T13:17:50Z INFO light stopping
$ echo $?
0
```

Error paths. The exit codes are the ones the supervisor's restart policy uses (3 restarts with backoff, 2 does not restart):

```
$ build/gt-sensor-soil --dev /dev/nope;   echo $?
2026-11-02T13:20:34Z WARN soil no control socket on fd 3: running without heartbeats
2026-11-02T13:20:34Z ERROR soil open_failed dev=/dev/nope errno=2
3
$ build/gt-sensor-light --chip /dev/nope; echo $?
2026-11-02T13:20:44Z WARN light no control socket on fd 3: running without heartbeats
2026-11-02T13:20:44Z ERROR light open_failed chip=/dev/nope line=17 errno=2
3
$ build/gt-sensor-soil --bogus 1;         echo $?
usage: build/gt-sensor-soil [--dev PATH] [--hub PATH] [--samples N] [--lo N] [--hi N] [--stuck N]
2
```

### Not covered by this entry

- Heartbeats and supervisor restarts with the real drivers (the sensors were run by hand, with no supervisor).
- The soil process's once-a-minute cadence over several cycles.
- Fault behaviour on real hardware (a disconnected DO wire, a pulled soil probe): tests S6 and L6, issue #30.
- Anything about `0xffff` beyond the 4 of 4 count. The mechanism is still unexplained, and Andrew diagnoses it.

## 2026-11-02: the LAN page on the Pi

Run on the Pi (`greenthumb`, Pi 5) in `~/Documents/Term-Project`. The commit was not pasted; the 23 tests below include the two `nonet` tests from PR #73, so it had at least that code. The sensors and the hub were not involved: the data came from the fake sensor feeder (`gt-feeder`), because `src/hub` does not exist yet. So the soil and light values below are the feeder's made-up numbers, not readings.

### Test suite

`make test` on the Pi: 100% passed, 0 failed out of 23 (total 58.95 s). The 23 include `interface_page`, `interface_http`, `interface_daemon`, `interface_lifecycle`, `nonet_behavior` and `nonet_src`, which had not run on the Pi before. Only the summary was pasted, so the per-test times are not recorded. `make asan` and `make memcheck` were not run on the Pi.

### Feeder

```
$ build/tools/gt-feeder --dir /tmp/gt-demo --rate 1
2026-11-02T23:42:31Z INFO store recovered segment=none dropped_bytes=0 last_seq=none
2026-11-02T23:42:31Z INFO feeder start dir=/tmp/gt-demo rate=1 resumed_seq=1 dropped_bytes=0
2026-11-02T23:42:31Z INFO store segment_created segment=2026-11-02.log
^C2026-11-02T23:46:42Z INFO feeder done records=251 last_seq=251 elapsed_ms=250479 rate=1.0
```

### The page

`gt-interface` was run on the same directory (`/tmp/gt-demo`) and its status was read from the Pi. The command line and the daemon's own log were not pasted. A feeder was writing again when this was read: the newest record is 0 seconds old and the count is 254, which is the 251 above plus 3 more.

```
{"state":"ok","reasons":[],"store_errno":0,"age_s":0,"soil":{"raw":598,"age_s":0},"light":{"bright":false,"bright_ms":3302,"age_s":0},"needs_water":"unknown","time_until_dry":null,"disk_free_mb":4031,"window":{"records":254,"soil_invalid":0,"soil_stuck":0,"light_invalid":0,"undervolt":0,"time_unset":0,"gap_events":0,"missing":0,"corrupt":0}}
```

Reported by Duke, not pasted: the page loaded on an iPhone in Safari, using the `greenthumb.local` name (not the IP address). No screenshot was kept.

### Not covered by this entry

- Real sensor data through the hub (there is no hub), and the page under the supervisor: `./gt-interface` is not in the supervisor's default child list yet.
- The stale and degraded cases on the Pi (the feeder stopped for more than 3 minutes, a missing store). They are covered by tests on the development machine and in `make test` on the Pi, but were not looked at on a phone.
- A phone that is not on a private network, and an Android phone (`.local` names often do not resolve there).
- A long run of the page. Only a few minutes were watched.

## 2026-11-03: needs water on the LAN page on the Pi

**Status: run on the Pi on 2026-11-03.** The `yes`, `no` and `unknown` states each showed as expected in `/status.json`, and `make test` passed on the Pi (30/30). The phone check is thin (see Results). The dry threshold itself is not checked.

What this checks (#43): that `gt-interface --dry N` shows `needs_water` as `yes`, `no` and `unknown` on the Pi, on the fake feeder's data. It does not check the threshold itself. The default dry line was 565 when this was run (it is now 450, see README), and C4 is judged against Adafruit's typical STEMMA ranges rather than a weight or finger check (see DESIGN.md), so a pass here says the logic and the page work, not that the flag is right for a particular plant.

### Steps (Duke runs these on the Pi)

1. `git pull`, then `make test`. Paste the summary line. This is the first Pi run of the hub, `child_logic`, `hub_proc` and `analysis_thirst` tests.
2. Terminal A: `build/tools/gt-feeder --dir /tmp/gt-demo --rate 1`.
3. Terminal B: `build/gt-interface --dir /tmp/gt-demo --dry 700`, then, from a third terminal, `curl -s localhost:8080/status.json`. Note `soil.raw` in the output. Expected: `"needs_water":"yes"` if `soil.raw` is at or below 700.
4. Stop `gt-interface` (Ctrl-C), start it again as `build/gt-interface --dir /tmp/gt-demo --dry 400`, curl again. Expected: `"needs_water":"no"` if `soil.raw` is above 400.
5. Stop the feeder (Ctrl-C), wait more than 3 minutes, curl again. Expected: `"needs_water":"unknown"` (the newest record is stale).
6. Open the page on a phone in one of the states and say which.

### Results

- `make test` on the Pi: 100% passed, 0 failed out of 30 (total 85.08 s). Only the summary was pasted, so the per-test times are not recorded, and the paste did not name the host or the commit (recorded as the Pi run asked for in step 1). `make asan` and `make memcheck` were not run on the Pi.
- `--dry 700`: passed. Pasted from the Pi (the daemon's command line was not pasted, but `dry_raw` is 700 in the output). Soil 537 is at or below 700, and the page shows `needs_water` `yes`:

```
{"state":"ok","reasons":[],"store_errno":0,"age_s":0,"soil":{"raw":537,"age_s":0},"light":{"bright":true,"bright_ms":1313,"age_s":0},"needs_water":"yes","dry_raw":700,"time_until_dry":null,"disk_free_mb":4031,"window":{"records":101,"soil_invalid":0,"soil_stuck":0,"light_invalid":0,"undervolt":0,"time_unset":0,"gap_events":0,"missing":0,"corrupt":0}}
```
- `--dry 400`: passed. Pasted from the Pi (the daemon's command line was not pasted, but `dry_raw` is 400 in the output). The same feeder directory kept growing (409 records, against 101 in the `--dry 700` run). Soil 733 is above 400, and the page shows `needs_water` `no`:

```
{"state":"ok","reasons":[],"store_errno":0,"age_s":0,"soil":{"raw":733,"age_s":0},"light":{"bright":true,"bright_ms":5317,"age_s":0},"needs_water":"no","dry_raw":400,"time_until_dry":null,"disk_free_mb":4031,"window":{"records":409,"soil_invalid":0,"soil_stuck":0,"light_invalid":0,"undervolt":0,"time_unset":0,"gap_events":0,"missing":0,"corrupt":0}}
```
- After the feeder stopped for over 3 minutes: passed. Pasted from the Pi, with `dry_raw` still 400. The newest record was 341 s old, the page state was `degraded` with the reason `stale`, and `needs_water` was `unknown`. The last soil value (705) is above 400 and would have read `no`, so it did not fall back to the old value. The record count is 465, 56 more than the 409 in the `--dry 400` run (probably the feeder running at 1 record a second until it was stopped; not checked).

```
{"state":"degraded","reasons":["stale"],"store_errno":0,"age_s":341,"soil":{"raw":705,"age_s":341},"light":{"bright":true,"bright_ms":6045,"age_s":341},"needs_water":"unknown","dry_raw":400,"time_until_dry":null,"disk_free_mb":4031,"window":{"records":465,"soil_invalid":0,"soil_stuck":0,"light_invalid":0,"undervolt":0,"time_unset":0,"gap_events":0,"missing":0,"corrupt":0}}
```
- Phone: Duke reported that the page on his phone says "degraded", while the feeder was stopped and the JSON above was `degraded` / `stale`. That matches. He did not report the needs-water text or the listed reason, and no screenshot was kept, so the phone check only confirms the degraded state, not how `unknown` is worded on the page. The `yes` and `no` states were not looked at on a phone.

### Not covered by this entry

- Real sensor data, the hub and the systemd unit (Andrew's, not tried on the Pi).
- Time-until-dry on real history. (The dry threshold follows Adafruit's typical STEMMA ranges; see DESIGN.md S1 and C4.)

## 2026-11-04: the synthetic-data label on the Pi

**Status: not run on the Pi yet.** The label is built and passes `make test`, `make asan` and `make memcheck` on the development machine (see README "View the page"). Nothing below has been pasted from the Pi, so no result is claimed. The runs above (2026-11-02 and 2026-11-03) were made before the label existed, so their feeder records have no flag.

What this checks: that records written by `gt-feeder` are labeled as synthetic on the Pi, on the page, in the JSON, in the feeder log, and in the tools. It does not check the sensors; with real sensors and the hub, the same page should say `"source":"live"` and show no banner, which is a separate run.

### Steps (Duke runs these on the Pi)

1. `git pull`, then `make test`. Paste the summary line.
2. Terminal A: `build/tools/gt-feeder --dir /tmp/gt-label --rate 1`. Paste its first log line. Expected: it contains `source=SYNTHETIC`.
3. Terminal B: `build/gt-interface --dir /tmp/gt-label`, then `curl -s localhost:8080/status.json`. Expected: `"source":"synthetic"` and `"synthetic_records"` equal to `window.records`.
4. Open `http://greenthumb.local:8080/` on a phone. Expected: a dark "SYNTHETIC DATA" banner under the title. Say whether it is visible.
5. Stop both (Ctrl-C). Run `build/tools/gt-analyze --dir /tmp/gt-label --now $(($(date +%s)*1000))`, `build/tools/gt-csv --dir /tmp/gt-label | head -3` and `build/tools/gt-logcheck --dir /tmp/gt-label`. Expected: a `WARNING: n of n records are SYNTHETIC` line, a last column `synthetic` with `yes`, and `synthetic=n` at the end of the summary.

### Results

None yet.

### Not covered by this entry

- Live data: a store written by the hub from the real sensors should read `"source":"live"` with no banner. That needs the hub on the Pi with the real sensors, which has not been run.
- A directory that holds both fake and live records (`"source":"mixed"`) is covered by tests on the development machine only.

## 2026-11-10 to 2026-11-13: the 48-hour soak on the Pi

**Status: run on the Pi.** Commit 26d9d38 (main), built on the Pi and installed with `cmake --install build --prefix ~/greenthumb`, run by the systemd unit with the real soil and light sensors. Raw files are in `48 Hour Soak Results/`; the store copy and the full journal stay on the Pi (`~/soak/run/`). Duke pasted the output below; nothing here is written from memory.

### Setup (2026-11-10)
- Clock NTP-synced, `throttled=0x0`, 101 GB free, store absent, user in the `i2c` and `gpio` groups.
- The journal was volatile (Pi OS ships `/usr/lib/systemd/journald.conf.d/40-rpi-volatile-storage.conf` with `Storage=volatile`). Fixed with `/etc/systemd/journald.conf.d/50-persistent.conf` containing `Storage=persistent`.

### Result (`soakreport.sh`, pasted 2026-11-13)
- Run: 2026-11-11T03:13:14Z to 2026-11-13T03:15:01Z, 48.03 h. 192 hourly status lines, 48 per child.
- RSS growth over the run: gt-hub 0 kB, gt-sensor-soil 0, gt-interface 0, gt-sensor-light 0 (1184 to 1168 kB).
- Open files: soil 7, light 7, interface 6, all constant. Hub 11 in 47 samples and 10 in one.
- Faults injected by cron: `kill -9` of the soil process at 2026-11-11T21:13:01Z (`died signal=9` +1 s, `state=running` +12 s) and of the light process at 2026-11-12T09:13:01Z (`died signal=9` +0 s, `state=running` +61 s). No other restart, death, exit or `ERROR` line.
- Stop at 2026-11-13T03:15:01Z: `stopping` once, four `exited code=0`, `stopped` once. Unit `Result=success`, `ExecMainStatus=0`.
- Store (`gt-logcheck` on `store-copy/store`, copied right after the stop): 2,882 records, `lost=0`, `corrupt=0`, `synthetic=0`.
- The report's one FAIL: `gt-hub open files changed from 11 to 10 at 2026-11-12T09:13:14Z (restarts=0)`. That sample is 13 s after the light kill. The light process was still `starting` and the hub had logged `client_closed slot=1`; the hub was back at 11 afterwards. Our reading is a closed socket for the dead sensor, not a leak. Andrew has not confirmed it.
- Pi state after the run: `/` is ext4 `rw,noatime`, 100 GB free, `throttled=0x0`.

### Not covered by this entry
- The rehearsal run was skipped (Duke's decision).
- L6 hand run: only part A (pin levels) was done.
- The 10-minute soil unplug was not done; two `kill -9` injections stand in for the one-injected-fault requirement. S6 is partly run.
- The Pi was not a bare install. After the run, `systemctl` showed `ssh`, `xrdp`, `wayvnc`, `avahi-daemon`, `bluetooth`, `NetworkManager` and `lightdm` running. Whether all of them were running during the soak was not checked.
- Duke started the service again on 2026-11-13, after the store was copied, so the live store under `/var/lib/greenthumb/` (files dated 2026-11-13 16:30) holds records from that later start. The figures above come from the copy made at the stop.
- CPU per process: `cpu.log` (245 lines) is kept; its figures are not summarized here.
- Hourly event counts: the status line has none (the supervisor has no record counter).

### Findings for Andrew (raw evidence from soak preparation; no diagnosis made)
- After `i2cdetect -y 1`, the first soil read gave `0xffff`, the second gave `Error: Sending messages failed: Remote I/O error`, and three later reads were fine (0x031d, 0x02ff, 0x030d).
- A hand-run `gt-sensor-light` stopped with Ctrl-C exited 130 (killed by SIGINT). It may handle only SIGTERM; not verified.
- In the hand-run L6 setup (a stand-in `gt-hub` on /tmp paths, `gt-sensor-light --window 2000`), the hub logged `message_dropped type=2 slot=0` every 2 s. Not seen in the real service at start. Unexplained.
- With the DO wire pulled, GPIO17 read `inactive` 3 of 3 times with no chatter in a 3 s sample (lit reads `inactive`, covered reads `active`).
- The light process took 61 s to reach `state=running` after the kill; the soil process took 12 s.
- The hub fd count dipped by one for one sample after the light kill (above).
