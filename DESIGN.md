## Design Document


## Architecture
For the architecture the system is one supervisor plus three worker processes, and analysis and the interface read the sotre on their own. No processes uses threads, because each one runs a single poll loop.
-**gt-supervisor** this starts the other processes, reads the exit codes and heartbeats, after that it restarts failed ones with backoff, and writes its own event log. Once an hour (`--status S` changes the interval) it also logs one `status` line per child. it reads no sensors and stores no readings.
--**gt-sensor-soil** reads the STEMMA soil over I2C (which is adress 0x36 once a minute and sends one soil message to the hub.
--**gt-sensor-light** counts the bright/dark transitions on the LM393 DO pin (which is gpio17) and sends a summary message to the hub once a minute. 
- **gt-hub is the only writer, once a minnute it builds one 24-byte record from the latest soil and the light messages and then appends it with 'store_append'.
- **analysis and interface** read the store through 'store_read_range'. Because of that they don't talk to the hub or sensors. Analysis is a library (`src/analysis`) with a command-line tool, `gt-analyze`; it is not a process of its own. The LAN page uses its needs-water part (`src/analysis/thirst.c`, #43) but not the dry-down prediction yet.
- **gt-interface** serves the LAN page. It listens on TCP port 8080, answers only private IPv4 addresses, and accepts at most [8] connections at once, each with [5 s] to finish its request. It rebuilds its answer from the last 24 hours of the store at most every [5 s], and only when a request needs it. It sends heartbeats like the other children. It is the only process that accepts network connections, and it opens none.

## Data flow and rates
| From | To | Channel | Message | Rate | Size |
|---|---|---|---|---|---|
| gt-sensor-soil | gt-hub | AF_UNIX SOCK_SEQPACKET, `/run/greenthumb/hub.sock` | GT_MSG_SOIL | 1 per minute | 32 B |
| gt-sensor-light | gt-hub | same socket | GT_MSG_LIGHT | 1 per minute | 32 B |
| gt-hub | store (disk) | `store_append`, fsync after each record | record | 1 per minute | 24 B (about 35 KB per day) |
| each child | gt-supervisor | socketpair inherited as fd 3 | GT_MSG_HEARTBEAT | every 10 s | 24 B |
| gt-supervisor | children | fork/exec, SIGTERM, SIGKILL | none | only on start, stop or failure | none |
| gt-interface | store (disk) | `store_read_range`, read only, the last 24 hours | records | at most once every [5 s], and only when a request needs it | up to about 35 KB (1,440 records of 24 B) |
| a browser on the LAN | gt-interface | TCP port 8080, HTTP GET of `/`, `/index.html` or `/status.json`, private IPv4 peers only | request, then the page or the JSON | one request per page load | request up to 2 KB, response under 9 KB |

### Operating-System mechanisms used
-** fork,exec,waitpid** (supervisor starts and reaps childrien)
-** signals and signalfd** (sigcld, sigterm and sigint handled in the supervisor)
-**poll (every process waits on its inputs and a timer in one loop)
-**AF_UNIX SOCK_SEQPACKET sockets** (messages between process; a dead peer shows up as end of file)
-**close_range** (each chiled keeps only fds 0 to 3 so no descriptors leaks across restarts)
-** I2C through i2c-dev**(soil sensor reads and **GPIO through the gpio chip character device, v2 edge events( which is for the light sensor)**
-**flock**(the hub holds an exclusive lock so that only one writer runs)
-**fsync on the file and its directory** (power cut loses at most the record being written)

### Failure isolation 
incase there is a crash,hang or i2c error in one of the sensors it affects only that proccess. The hub records the missing reading as invalid and then it keeps going.
If there is a situation that the hub dies, the supervisor restarts it, and resumes numbering from the last record in the store, 
so then the gap appears in the log. Since the store is append only, a crash loses at most the record in flight.


## Mechanism Mapping
| Mechanism | Where it lives | How it is checked | Status |
|---|---|---|---|
| Multi-process isolation with a supervisor (mechanism E) | src/supervisor, src/ipc | tests/supervisor_test.sh: restart with backoff, no restart on a config exit, clean SIGTERM, no fd leak; `make memcheck` runs the supervisor under Valgrind | Supervisor restart, shutdown and heartbeat deadlines implemented; hub and sensor processes implemented (hub_process, soil_process, light_process); crash-loop DEGRADED implemented (child_logic, supervisor_behavior); hourly `status` line per child implemented (`status_logic` for the /proc reader and format, `child_logic` for the fault count, `supervisor_behavior` for the timer; it ran in the 48-hour soak on the Pi, but its output has not been checked yet, and it has no record count); the events.log file is written with `--events` (`supervisor_behavior`; enabled in the soak's service file, output not yet checked), and orphan cleanup is written (`--pids` registry swept at start, plus `PR_SET_PDEATHSIG` so children get SIGTERM when the supervisor dies; `orphan_logic` and `supervisor_behavior`; the soak's service file passes `--pids`, but cleanup itself was not exercised on the Pi) |
| Custom storage layer (mechanism D) | src/store | store_test, store_failures and store_error_paths (fault injection on every libc call), the kill -9 loop, and the log checker | Implemented |
| Watchdog through heartbeats | src/supervisor, src/ipc | tests/supervisor_test.sh with short limits (`--soft 1 --hard 3 --grace 1`): a silent child is warned, sent SIGTERM and restarted even though it exits 0; a child that ignores SIGTERM is sent SIGKILL; a healthy child is left alone; shutdown does not wait for a stubborn child | Implemented: the sensors and the hub send heartbeats; the supervisor warns at 30 s of silence, sends SIGTERM at 60 s and SIGKILL 5 s later. Not yet run on the Pi |
| Custom drivers | src/sensors | Userspace drivers over kernel interfaces (i2c-dev, GPIO character device), not a kernel module. The loops run in tests with fake drivers (soil_logic, light_logic, soil_process, light_process); the real drivers are checked on the Pi with raw sensor reads and the GPIO17 level | Sensor processes and drivers written; drivers checked on the Pi on 2026-11-02 (see HARDWARE-CHECKS.md) |
| Dry-down analysis and time-until-dry (#27) | src/analysis, tools/analyze.c | `analysis_dry` (cycle detection on seeded synthetic series: noise of +-16 counts, gaps, INVALID, STUCK and corrupt records, out-of-order times, short and badly fitting cycles, the bucket limit), `analysis_predict` (the median rate worked out by hand, every reason for withholding, the log line) and `analysis_store` (three weeks of synthetic history in 23 daily segments on disk, through the library and the real `gt-analyze`: cycles, prediction, a corrupt record, windows, an empty and a missing store, bad arguments) | Implemented and tested on synthetic data only. Not yet run on recorded data: the hub does not exist, so there is no real history. The dry threshold is an input; the default is 450, the top of the typical STEMMA dry range (350-450). Not run on the Pi |
| LAN page daemon | src/interface | `interface_page` (states and rendering), `interface_http` (the strict request parser, response builder and private-address check, with a fuzz pass), `interface_daemon` (the real loop over loopback TCP: routes, connection limit and deadline, refresh interval, heartbeats, exit codes, no leaked descriptors), `interface_lifecycle` (the real binary with the real feeder: exit codes, heartbeat, the page following a store that appears, descriptors and sockets), and `nonet_src` and `nonet_behavior` (the no-outbound-network rule) | Implemented and run on the Pi on 2026-11-02 (see HARDWARE-CHECKS.md). Needs-water (yes, no or unknown) is built on `src/analysis/thirst.c` and tested by `analysis_thirst` and the page tests, against a dry threshold (`--dry`, default 450). Time-until-dry still says "not enough data". The supervisor now starts it, and it ran as a child in the 48-hour soak on the Pi. The page was not viewed during the soak, so it has been checked only with fake-feeder data, not with the hub or real sensors |


## Failure-Mode Table
**How to read this table.** For each component: how it can fail, how we detect it, what the system does about it, and what the log shows. The "Verified by" column points to the test that proves the row (IDs from the evaluation plan). `NEW` means we still need to write that test. Every log line below is a **draft**: once we inject the failure for real, we replace it with the line the system actually printed.
 
**Log line format:** `<ISO time> <LEVEL> <component> <event> key=value ...`

**Hourly status line.** Every hour (`--status S`, 1 to 3600 s) the supervisor logs one line per child, running or not, so a slow leak or a quiet restart pattern shows up in `journalctl -u greenthumb` before a daemon dies:
`2026-11-04T23:21:27Z INFO supervisor status child=gt-hub alive=1 state=running degraded=0 rss_kb=5120 fds=7 restarts=0 faults=0`
`rss_kb` and `fds` come from `/proc/<pid>` and are `-1` when the child is not running. `faults` counts exits that were not a clean 0 (a signal, a non-zero code, or a kill for silence), not counting shutdown. **Not reported yet: a record count.** The supervisor has no record counter, and getting one needs a change to the frozen `ipc.h` contract (both owners) or a read of the store.
 
Thresholds in [brackets] are our first guesses and will be set from measurements.
 
| # | Component | How it fails | How we detect it | What the system does | What the log shows (draft) | Verified by |
|---|---|---|---|---|---|---|
| 1 | Soil sensor | Wire loose or disconnected. The I2C read may fail outright (row 4) or return a value stuck at 0 or full scale (row 5). | Raw value outside [calibrated dry − margin, wet + margin], or noise far above normal. In use: the bounds `[250,1100]`, kept on 2026-11-02. Observed range: about 320 (probe in air) to 977 (wet soil). S1 was not run, so there is no measured dry/wet range. If real history nears an edge, widen the bounds, do not tighten them. | Soil field marked INVALID, never stored as a good reading. Analysis skips it. Light and hub keep running. Sampling continues, so it recovers when the wire is back. | `WARN soil status=0x01 raw=65535 seq=4812` (0x01 = INVALID) | S6 |
| 2 | Soil sensor | Stuck reading | Identical raw value for [60] consecutive samples. Real soil plus sensor noise rarely does this, but very stable soil might (see note). | Flag STUCK, log once, keep storing the raw value, exclude it from analysis | `WARN soil status=0x02 raw=700 seq=4850` (0x02 = STUCK, logged when the status changes) | S6 |
| 3 | Soil sensor | Noise spike | Sample differs from the median of the last [5] by more than [N]% of span | Median-of-N filter discards the spike and counts it | `INFO soil spike_rejected raw=29100 median=14620 seq=4901` | S2 |
| 4 | Soil sensor (I2C) | I2C transfer to the soil sensor (0x36) fails | The syscall returns an error (errno is logged) | Retry [3] times, then mark this cycle INVALID. After [5] failed cycles in a row, exit nonzero so the supervisor restarts the process. | `ERROR soil read_failed errno=5 attempt=3/3 seq=4902`, then after the fifth failed cycle `ERROR soil giving_up failed_cycles=5` | NEW (unplug the soil sensor SDA/SCL, or disable I2C) |
| 5 | Soil sensor (I2C) | Bad wiring returns a constant 0 or full-scale with no error. The transfer itself succeeds, so it looks like a real reading. | Same range and stuck checks as rows 1 and 2 | Same as rows 1 and 2 | `WARN soil status=0x01 raw=0 seq=4903` | S6 |
| 6 | Light sensor (LM393 module) | DO wire disconnected, or the module unpowered. The GPIO17 input floats or sits at one level, so a dead sensor can look like constant darkness or brightness. | The flag never changes in [48 h] (a plant sees day and night), or it changes more than [20] times in one 60 s window (a floating input chatters). | Light field marked INVALID and flagged separately. Never stored as a real bright or dark reading. Retried each cycle so it recovers when reconnected. | `WARN light status=0x01 level=0 edges=0 seq=4920` then, when it recovers, `INFO light status=0x00 level=1 edges=1 seq=4934` (the status line is logged only when the status changes) | L6 |
| 7 | I2C bus | I2C bus hangs (for example SDA held low) and the read blocks. Only the soil sensor is on the bus, so soil readings stop. The light flag is read from GPIO17 and keeps working. | Read exceeds a [2 s] timeout, and the process heartbeat stops | Supervisor sends SIGTERM, then SIGKILL after [5 s], and restarts the soil reader. If the bus is still stuck after [3] restarts, mark soil DEGRADED and keep the light reader, hub and store running. | `WARN supervisor child=soil heartbeat_missed age=35s` then `ERROR supervisor child=soil killed signal=SIGKILL restart=1/3` | NEW (SIGSTOP the process to simulate a hang; this does not simulate a hung bus) |
| 8 | Sensor process | Crash (segfault, abort, nonzero exit) | Supervisor `waitpid()` reports the exit status or signal | Restart with exponential backoff ([1 s, 2 s, 4 s ... cap 60 s]). Other sensors are unaffected. | `ERROR supervisor child=soil pid=2210 died signal=11` then `INFO supervisor child=soil restart=1 delay=1s` then `INFO supervisor child=soil heartbeat pid=2231` | P3 |
| 9 | Sensor process | Hang (alive but doing nothing) | No heartbeat for [30 s] (hard limit 60 s) | SIGTERM, then SIGKILL, then restart as in row 8 | As row 7 | P3 (add a SIGSTOP variant) |
| 10 | Sensor process | Crash loop | More than [5] restarts in [5 min] | Stop fast restarts and hold at the backoff cap. Mark the child DEGRADED. Keep the rest of the system running. | `ERROR supervisor child=soil crash_loop restarts=6 window=300s state=DEGRADED` (logged once; `restarts` is the one that tripped it; the child is marked recovered after a full window up) | NEW |
| 11 | Hub | Crash or hang | Same as rows 8 and 9 | Restart. On startup it reads the last sequence number from the store so numbering continues. Readings during the downtime are lost, and the sequence gap shows it. | `INFO hub start resumed_seq=4813 gap=2` | P3 |
| 12 | IPC | Message lost or malformed | Hub checks message length and version. Sender sees EPIPE or EAGAIN. | Drop the bad message and log it. The sender reconnects. | `WARN hub bad_message from=soil len=9 expected=16` | NEW |
| 13 | Supervisor | Supervisor itself dies | External watcher: [systemd `Restart=always`]. If we don't use systemd, this is a single point of failure and we say so here. | systemd restarts it. On startup it kills any orphaned children so there are no duplicates. | journald: `supervisor.service: Main process exited` then ours: `INFO supervisor start orphans_killed=2` | NEW (`kill -9` the supervisor) |
| 14 | Store | Torn write on power loss or `kill -9` | Short final record or CRC mismatch at the tail of the newest segment, checked on startup | Truncate at the last valid record and continue | `WARN store recovered segment=2026-10-25.log dropped_bytes=17 last_seq=4811` | P1, P2 |
| 15 | Store | Corruption in the middle of a log (bit rot) | CRC mismatch on a record that isn't at the tail | Skip it. Fixed-size records let the reader resync at the next boundary. The gap is marked in the CSV export. | `ERROR store crc_mismatch segment=2026-10-25.log offset=2400 skipped=1` | NEW (flip bytes in a copy of a log) |
| 16 | Store | Disk full | Write returns ENOSPC | Stop appending but keep sensing. Show DEGRADED on the LAN page. Events go to stderr and journald. | `ERROR store append_failed errno=28 seq=5200` | NEW (small loopback filesystem) |
| 17 | SD card | Filesystem remounts read-only | Write returns EROFS or EIO | Same as row 16. A human has to fix it. | `ERROR store append_failed errno=30 seq=5201` | NEW |
| 18 | Clock | Wrong time after reboot. The product makes no network calls of its own, but the Pi's operating system does keep time over NTP when it is on a network (it was synced before the soak); offline, the Pi's real-time clock needs a backup battery to keep time. | New timestamp earlier than the last stored one, or earlier than the build time | Flag the record TIME_UNSET. Rely on sequence numbers and the monotonic clock until the time is fixed. | `WARN clock backwards now=2026-01-01T00:00:03 last=2026-10-25T19:00:00 seq=5300` | NEW (set the clock back with `date`) |
| 19 | Power | Brownout or under-voltage. Both sensors run from the 3.3 V rail, so a sag can shift the soil reading or flip the light flag. | Pi under-voltage flag (kernel message, or `vcgencmd get_throttled`). We need to confirm this works on the Pi 5. | Log it and flag samples taken during the event | `WARN power undervoltage seq=5400` | NEW |
| 20 | Analysis | Wrong or unsupported prediction (no crash) | Too few dry-down cycles recorded, or recent data is INVALID or STUCK | Withhold the prediction and show "not enough data" | `INFO analysis prediction_withheld reason=insufficient_cycles have=2 need=3` | C3. The withholding rules and this log line are tested in `analysis_predict`; the reasons are `no_recent_data`, `insufficient_cycles`, `not_drying` and `not_falling` |
| 21 | LAN page | Process dies | Supervisor, as in row 8 | Restart. Logging is unaffected because processes are isolated. | As row 8 | P3. The real binary's exit codes, heartbeat and clean SIGTERM are in `interface_lifecycle`; the supervisor now starts it, and it ran as a child in the 48-hour soak; a kill-and-restart run aimed at this process alone has not been done |
 
**Note on row 2:** a stuck-sensor check can false-alarm in very stable soil. We measure how often it does on clean data, and we adjust the threshold or report the false-alarm rate here.
 
## Failures we cannot detect
 
The soak test grades this table's honesty, so these are listed on purpose. The log shows nothing for any of them.
 
| Failure | Why we can't detect it | How we'd find out |
|---|---|---|
| Slow soil sensor drift or coating degradation | With one sensor, drift and real soil change look the same | Manual pot-weight check, test S5. We store raw values so we can recalibrate later. |
| Threshold set wrong (blue pot), so the flag flips at the wrong light level | The module reports only a flag, and there is no trusted reference on board | None. The threshold was set by eye once and is never checked against a reference. We only claim bright/dark at the pot's setting. |
| Plausible but wrong soil reading (probe moved, air pocket around it) | It looks like valid data | Test S3 measures how much reinsertion changes readings. |
| SD card reporting `fsync` complete before the data is durable | The card hides it from software | Only real power pulls (P2) can show it. |
| Temperature effects on the soil sensor and on drying | No temperature sensor | Hand-recorded room temperature, listed under constraints. |

## Storage and Data
What is stored?

- One record per reading cycle: timestamp, sequence number, raw soil value (the STEMMA sensor's capacitive reading), light flag (bright or dark)
- Raw soil value (not calibrated percent) so you can recalibrate later when the sensor drifts. The light flag is stored as read.
- A SYNTHETIC flag (`GT_REC_SYNTHETIC`, 0x0040, in the record's flags field) marks a record written by the test feeder instead of read from a sensor. The hub never sets it. Every reader labels such data and none filters it out: the LAN page shows a banner and a `source` field (`none`, `live`, `synthetic` or `mixed`), `gt-analyze` prints a warning, `gt-csv` has a `synthetic` column and `gt-logcheck` prints `synthetic=N`. This is how replayed or fake data is kept from passing as live. The flag was added to a shared header, so it needs both owners' agreement.
- Supervisor events (sensor died, restarted, log recovered) go in a separate text log

At what rate?

- Once per minute
- Soil moisture and light change over minutes to hours, so faster sampling adds noise and SD wear without adding information
- If readings are noisy, take several quick samples per minute and store the median

In what format?

- Append-only log of fixed-size binary records (about 24 bytes)
- Each record ends with a CRC32 checksum, so you can tell a complete record from a half-written one
- Fixed size makes recovery simple
- One hub process is the only writer, so appends never interleave
- A small export script converts the log to CSV for graphing

What retention?

- About 1,440 records/day, roughly 35 KB/day or 13 MB/year, so storage isn’t a constraint
- Keep everything
- Split into one segment file per day or week so one damaged file never threatens the rest
- Old segments can be archived or deleted without touching the active one

What happens when power dies mid-write?

- fsync after every append: cheap at one record a minute, and a power cut costs at most the single in-flight record
- fsync the directory when creating a new segment file, or the file can vanish after a crash
- A torn write shows up as a short final record or a failed CRC
- On startup, scan the tail of the newest segment, truncate at the last valid record, and log “recovered, dropped N bytes”
- Gaps in sequence numbers or timestamps show when the Pi was down
- To measure it: kill -9 the writer at random moments and do a few real power pulls, then confirm every recovery leaves a valid log with at most one record lost

## Constraints and Substitutions
| | Ideal | What we're using | What the gap costs |
|---|---|---|---|
| Soil sensors | Several, across several pots, so they corroborate each other | One capacitive sensor, one pot | We can't cross-check readings, so we can't tell a sensor fault from real soil behavior. |
| Plants | Several plants and soil types | One plant | Results describe this plant and pot only. |
| Light sensor | Calibrated lux sensor, checked against a reference meter | LM393 light module (LDR plus comparator), read as a bright/dark flag on GPIO17. The switch point is set by the blue pot. | The module gives no light level, only whether light is above or below the pot's threshold, so we can't measure how bright it is or compare two bright days. We use light as a bright/dark signal (day vs. night, lit vs. shaded), not as a measurement. |
| Temperature | Temperature sensor beside the soil | None | Temperature affects both capacitive readings and drying speed. We can't separate it from moisture or light effects, so some of what we attribute to light may be temperature. |
| Time | A full season of dry-down cycles | [N] weeks until the deadline, roughly [N] dry-down cycles | Too few cycles for strong prediction statistics. We report results as a small sample. |
| Soil moisture reference | Lab-grade probe or oven-dry samples | Kitchen scale (pot weight) | Weight reflects the whole pot, not the soil at the probe. |
| Sensor interface | A separate ADC (or bus) per sensor | Soil sensor on I2C (0x36), light flag on its own GPIO pin (17). No ADC. | Each sensor has its own interface, so an I2C fault takes out soil only (row 7). With no ADC, there is no analog light level (see the light sensor row). |
| Power-loss testing | Many pulls on fresh SD cards | [10] pulls on one card | Limited by card wear. Most crash testing uses `kill -9` with a synthetic feeder. |
| Alerting | A push notification (phone, email or SMS) when the plant needs water | A "needs water" state on the LAN page only. The product makes no outside network calls, so there is no push, email or SMS. | Nobody is told unless they open the page, so a dry plant can go unnoticed. The page shows the state and the current reading, and shows "not enough data" when the reading cannot be trusted. |

### Narrowed claims
- We claim the device predicts time-until-dry for **this plant in this pot**, within [X]% over [N] cycles. We do not claim it generalizes.
- We treat light as a **bright/dark flag**. We claim only that it separates lit from unlit at the pot's setting, based on the baseline readings in Part 2. We do not claim it reports a light level or true lux, or that the switch point stays consistent over days.
- Without a temperature reading, we report any light-vs-dry-down relationship as a trend that temperature may also explain.
- The light-adjusted estimate (#26) assumes the drying rate is a straight line in bright hours per day, fitted to [5] or more cycles of one plant. With so few cycles the fit can be wrong, which is why it is withheld when the light barely varies or the current light is outside what it has seen, and why C3 judges it against the soil-only estimate instead of assuming it is better.
- Why a threshold is not enough: a threshold only knows the current soil value. "Time until dry" needs how fast this plant dries, which comes from the fitted dry-down history, and the light-adjusted version also needs how the light changes that speed. Neither can be read from one value with an if-statement.
- The time-until-dry estimate assumes each dry-down is a straight line in the raw soil value. A real dry-down curves as the soil dries, so the estimate is least trustworthy near the dry end. The line's R^2 must be at least [0.8] or the cycle is thrown out, and the prediction needs at least [3] accepted finished cycles. The estimate is tested on synthetic series only so far. It is a statement about this plant and pot, once there is real history, and it counts down to a dry threshold that is not calibrated.
- The LAN page shows needs-water as yes, no or unknown (#43, `thirst.c`): a Schmitt trigger that flips to yes at or below the dry threshold [450] and back to no only at threshold + [48] counts (three times the 16-count noise), and says unknown, never no, when there is no good recent reading. The threshold is 450, the top of the typical STEMMA dry range (350-450; moist or ideal is 450-750). C4 has not been done, so the flag has not been checked against real watering. Time-until-dry still says "not enough data" until #27 runs on real history. The page shows OK only for a fresh, valid record with nothing flagged. Its limits are drafts: [3 min] without a record is stale, a record [60 s] ahead of the clock is a clock fault, [100 MB] free is low disk, and [24 h] is the history it reads.

## Evaluation Plan
Our device uses two sensors, a capacitive soil moisture sensor (read over I2C) and a light sensor that reports bright or dark (an LM393 module with an LDR), to tell us when a houseplant needs water and why. Our design doc says the biggest risk is that drift or noise makes the readings look confident but wrong. So this plan mostly tests the sensors themselves: are they accurate, stable, and actually useful together? A short last section checks that the data survives crashes and power loss.

Every measurement has a method and a target. We're writing the targets down before running anything so we can't quietly move the goalposts later. If we miss one, we record the result and figure out why instead of editing the number.

A note on the targets: most sensor targets below are our best guesses before we've measured anything. If the first real runs show one is unrealistic, we can change it, but we'll add a note saying what we measured and why. Targets marked (correctness) are pass/fail and stay as written.


---


## Part 1: Soil moisture sensor

**Baseline readings** (raw, measured on the Pi, 10 reads per condition):

| Condition | Raw reading |
|---|---|
| Air | about 320 |
| Glass of water | about 522 |
| Super-dry soil | about 565 |
| Wet soil | about 977 |

The glass of water reads below dry soil. That was repeatable in two runs and was not investigated; the product reads soil, so S1 uses soil only. A single soil reading varies by about 16 counts (one standard deviation) in these sets. The invalid-reading bounds in the failure table (`[250,1100]`) are drafts built from these numbers. S1 was not run, so they were kept as they are (decision of 2026-11-02, failure table row 1).

| ID | Test | Method | Target |
|----|------|--------|--------|
| S1 | Calibration curve | Weigh a pot of dry soil, saturate it, then take sensor readings at 5 moisture levels as it dries. Calculate water content from the weight at each point and plot raw reading against it. | The curve is monotonic (it never goes the wrong direction as soil dries) and R² ≥ 0.9. |
| S2 | Noise | Hold the soil at a fixed moisture and take 100 readings in a row. Compare the spread of single readings against the spread of median-of-5 readings. | Standard deviation under 1% of the dry-to-wet range after taking the median of several samples. |
| S3 | Reinsertion repeatability | At fixed moisture, pull the sensor out and push it back in 10 times, recording a reading each time. | Readings spread under 5% of the range. If it's worse, we'll document it and add "fix the probe in place" to the setup instructions. |
| S4 | Response time | Move the sensor from dry soil into wet soil and record readings every second until they stop changing. | Settles within 5% of its final value in 10 seconds. This only measures the sensor, not how fast water spreads through the pot. |
| S5 | Drift | Leave the sensor in a sealed container of soil at fixed moisture for 48 hours, logging once a minute. | Drift under 2% of the range over 48 h. This tests our named risk directly. |
| S6 | Fault detection | Disconnect the sensor wire, short it, and hold a reading constant to fake a stuck sensor. | Each fault is logged within 2 sample cycles, the light sensor keeps running, and clean data causes fewer than 1 false alarm per week. |

- **S1 why:** The raw sensor number means nothing until we know how it maps to actual wetness.
- **S3 why:** Capacitive sensors are sensitive to how well the soil touches the probe, and a plant owner will move it around.

**Measured results (S1-S5).** Captured with `tools/soilread.sh` and judged with `tools/soilstats.sh` on the Pi. The targets above are not edited: a miss is recorded here with the reason. Raw captures are kept with the run, every read as the sensor returned it.

| ID | Target | Measured | Result | Date | Notes |
|----|--------|----------|--------|------|-------|
| S1 | Monotonic, R² ≥ 0.9 | not run | DESCOPED | 2026-10-29 | Descoped by Duke, not measured. Consequence: no measured dry/wet range and no pot-weight calibration, so the bounds `[250,1100]` (failure table row 1) come from the readings of our build and the dry threshold from the typical STEMMA ranges. Affects #31, #43 and C4. |
| S2 | Median-of-5 stdev under 1% of the range | 1.83% (7.52 counts); single reads 3.84% (15.84 counts); 100 reads, 797-853, 0 failed, 0 at 0xffff, range 565-977 | FAIL | 2026-10-29 | Target not met and not changed. For independent reads a median of 5 cuts the noise only to about 0.56x a single read; about 20 or more samples per cycle would be needed (estimate, untested). |
| S3 | Reinsertion spread under 5% of the range | 33.25% (137 counts); 10 insertions, 702-839, falling over the run | FAIL | 2026-10-29 | Target not met and not changed. The values fall steadily (last five: 762, 734, 726, 709, 702). A 10-minute no-touch control stayed within 722-758 (36 counts), so ordinary drying does not explain the 137-count spread; reinsertion or soil disturbance does, at least in large part. The control's level (about 740) also differs from S2 (about 821), so placement or slow drying may add to it. Not repeated on stable soil; taken at face value. Setup instruction to add: fix the probe in place. |
| S4 | Settles within 5% in 10 s | not run | DESCOPED | 2026-10-29 | Descoped by Duke, not measured. Consequence: no measured settle time after watering, so we cannot say how soon after watering a reading can be trusted. |
| S5 | Drift under 2% of the range over 48 h | not run | DESCOPED | 2026-10-29 | Descoped by Duke, not measured. Consequence: our named risk (drift and corrosion of a cheap capacitive sensor) has no measured figure; any drift claim must say so. |

---

## Part 2: Light sensor (LM393 module, bright/dark flag)

**Baseline.** We use one set of readings as the baseline for this sensor and do not run separate threshold, chatter, angle, stability or day/night series. The module's DO output reads active (high) when it is dark and inactive (low) when it is lit, so the light reader inverts GPIO17 to get the bright flag (`GT_REC_LIGHT_BRIGHT`, 1 = bright). The blue pot is set once and then left alone, because moving it shifts the threshold and makes data from before and after incomparable. With no reference measurement, we claim only lit versus unlit at the pot's setting (see Narrowed claims).

| ID | Test | Method | Target |
|----|------|--------|--------|
| L6 | Fault detection | Disconnect the DO wire mid-run, and separately cover the sensor fully. | A disconnected wire is logged as INVALID, never recorded as a real bright or dark reading (see failure table row 6). Covering the sensor is recorded as real darkness. |

- **L6 why:** A disconnected sensor and a dark room can look similar if we aren't careful.

---

## Part 3: The two sensors together

This is the part that tests our design claim that the sensors are *correlated* instead of just sitting next to each other.

| ID | Test | Method | Target |
|----|------|--------|--------|
| C1 | Timestamp alignment | Check that each record's soil and light readings were taken close together. | The two readings in one record are within 1 second of each other. |
| C2 | Does light change the dry-down rate? | Record dry-down cycles for the same plant or identical pots in a bright spot and a dim spot, with a similar starting moisture. Measure each spot as hours of bright per day from the flag. | A clearly measurable difference in dry-down rate between the two spots. If we find no difference, that's an important result, and we'd need to rethink why we're using light at all. |
| C3 | Light-adjusted prediction vs soil-only | Record at least 5 real dry-down cycles. At 12 hours before the plant crosses the dry threshold, predict "time until dry" two ways: using soil readings only, and using soil plus hours of bright per day. Compare both to what actually happened. | The light-adjusted prediction has lower median error than the soil-only one, and its median error is under 25% of the remaining time. |
| C4 | "Needs water" flag vs a real check | Each time the device flags a plant as needing water, check the plant by weighing the pot or pushing a finger into the soil. Do this 20 times. | The flag agrees with the real check at least 90% of the time. |

- **C3 note:** This is our weakest guess. We expect to revise the number, and that's fine as long as we document why.
- **C3 status:** the comparison is built (`lightadj_backtest`, run with `gt-analyze --compare`) and tested on synthetic data only. For each finished cycle that reached the dry level it predicts 12 hours before the crossing from all the OTHER cycles, both ways, so it needs at least [5] such cycles with the light adjustment available, and the cycles at the extremes of the light range are not scored for the adjusted estimate (it does not extrapolate). Pass means the adjusted median error is lower than the soil-only one and under [25]% of 12 hours. It has not been run on real data, because the hub has not recorded real dry-downs yet.

---

## Part 4: Making sure the sensor data survives

The sensors only matter if the log we build from them stays trustworthy. These tests cover our two graded mechanisms, D (crash-safe storage) and E (multi-process architecture with a supervisor).

| ID | Test | Method | Target |
|----|------|--------|--------|
| P1 | Surviving `kill -9` on the writer *(correctness)* | Run a fake sensor feeder at about 1,000 records/s so a random kill actually lands mid-write. Kill the writer after a random delay, restart, and run the log checker. | 500 kills, 0 CRC failures mid-log, at most 1 record lost per kill, and a "recovered" event logged each time. |
| P2 | Real power pulls *(correctness)* | Unplug the Pi mid-run, at the real once-a-minute rate and at the high rate. Reboot and run the checker. | 10 pulls, 0 unrecoverable logs, at most 1 record lost per pull. If a pull loses more than one record, the SD card may be reporting `fsync` as done too early, which is a real finding. |
| P3 | Restart after a child process is killed *(correctness)* | Kill the soil process, the light process, and the hub at random moments, 200 times each, and time the restart. | Median restart under 5 s and p99 under 10 s. The surviving sensor keeps logging and the gap shows up in the event log. |
| P4 | 48-hour soak with real sensors *(correctness)* | Run the full system unattended on the Pi with both real sensors for 48 hours, recording each process's memory (RSS) and open file counts every hour. | 2,880 records with no unexplained sequence gaps, RSS growth under 1 MB per process, constant fd counts, and 0 unplanned supervisor restarts. |

**Measured results (P1).** Captured with `tools/killloop.sh` on the Pi. The targets above are not edited: a miss is recorded here with the reason.

| ID | Target | Measured | Result | Date | Notes |
|----|--------|----------|--------|------|-------|
| P1 | 500 kills, 0 CRC failures mid-log, at most 1 record lost per kill, "recovered" logged each time | 500 of 500 passed, 0 failed; `max_dropped=0`, `torn_restarts=0`; 1,000 records/s, kill 1-1000 ms after ready; store on SD card (ext4); seed 1793573235; commit `ac42b0b` | PASS | 2026-11-01 | Target met and not changed. No kill left a partial record, so the torn-tail truncation was not exercised here (likely one `write()` per record and `kill -9` does not split it; untested). Power loss is P2. |

**Measured results (P2).** Captured on the Pi with `gt-feeder --progress-sync` and `gt-logcheck`, with the store and the progress file both on the SD card (ext4). The target in the table above is 10 pulls. Duke ran 2, on 2026-11-02, because of the time each pull takes and the risk to the SD card. The other conditions are unchanged, and the remaining 8 pulls were not run.

| ID | Target | Measured | Result | Date | Notes |
|----|--------|----------|--------|------|-------|
| P2 | 10 pulls, 0 unrecoverable logs, at most 1 record lost per pull | 2 of 10 pulls run. Pull 1 at `--rate 0` (about 105 records/s): 2,896 records, `lost=0 corrupt=0`, the log ended at the last acknowledged seq (`acked=2896`). Pull 2 at `--rate 1`: 3,030 records, `lost=0 corrupt=0`, `acked=3030`. Both recoveries logged `dropped_bytes=0` and `recovered=1 max_dropped=0`. | PASS on the 2 run; 8 not run | 2026-11-02 | See the limits below. |

Limits of this result:
- Two pulls is a small sample. A card that loses data on only a fraction of pulls could pass two.
- The witness (the fsynced progress file) is on the same SD card as the log. A card that reports fsync done too early could hide a loss in both.
- `--rate 1` is the slowest the feeder offers, not the once-a-minute rate in the method, and at that pace a pull rarely lands mid-write.
- No torn record was seen, so truncating a partial record after power loss was not exercised.
- The writer log is not fsynced and its contents were lost in the pull, so each recovery was checked against a log of one start (`--restarts 0`).

**Measured results (P4).** The run was done on the Pi from 2026-11-11T03:13:14Z to 2026-11-13T03:15:01Z (commit 26d9d38, built on the Pi). The figures below come from `tools/soakreport.sh` on the raw journal export (kept in `48 Hour Soak Results/`), following `SOAK-RUNBOOK.md`. The store has not been checked yet, so the record count and sequence gaps are still pending. The target in the table above is not edited: a miss is recorded here with the reason.

| ID | Target | Measured | Result | Date | Notes |
|----|--------|----------|--------|------|-------|
| P4 | 2,880 records with no unexplained sequence gaps, RSS growth under 1 MB per process, constant fd counts, 0 unplanned supervisor restarts | `soakreport.sh` on the journal, marks and unit result (48.03 h, 48 status samples per child): RSS growth 0 kB for hub, soil and interface, and 0 for light (1184 to 1168 kB); 0 unplanned restarts, deaths, exits or `ERROR` lines outside the two marked kills; both kills logged `died signal=9` within 1 s and back to `state=running` in 12 s (soil) and 61 s (light); stop was `stopping`, four `exited code=0`, `stopped`; unit `Result=success`, `ExecMainStatus=0`. fd counts: soil 7, light 7, interface 6 constant; hub 11 in 47 samples and 10 in one (see Notes). Record count and sequence gaps: not checked yet, because the report was run without `--store` | FAIL on the fd criterion (report exit 1); all other checks passed; record count pending | 2026-11-11 to 2026-11-13 | The one fd miss: the hub showed `fds=10` at 2026-11-12T09:13:14Z, 13 s after the light kill, when the light sensor was still `starting` and the hub had logged `client_closed slot=1`; it was back at 11 once the light sensor was running again. The report counts any fd change within a run that has no hub restart, so it fails this; our reading is a closed socket for the dead sensor, not a leak (not yet confirmed by Andrew). Faults injected by cron: `kill -9` of the soil process at 2026-11-11T21:13:01Z and of the light process at 2026-11-12T09:13:01Z, each logged as `died signal=9`. The 10-minute soil unplug was not done (dropped; two kills cover the one-injected-fault spec). "0 unplanned restarts" means none outside the two marked kills. |

Limits of the P4 run, fixed in advance:
- S6 is partly run. The soil unplug was planned inside the soak but was not run (see the P4 row); shorting a wire and holding a reading constant (the "stuck" case) are not run, because of the pin risk and the 60 identical cycles it takes.
- A disconnected DO wire inside the soak may never be flagged, because the light process marks INVALID only after 48 hours with no change. L6 is a separate hand run with a short `--stuck` (see `SOAK-RUNBOOK.md`).
- The supervisor reports no record count, so records are counted from the store by `gt-logcheck`.

### Mechanism evaluation: D and E

We claim two mechanisms from the course menu: **D** (custom storage layer, with
a crash-consistency story) and **E** (multi-process architecture: sensors in
separate processes, a hub, and a supervisor that survives any child dying). We
do not claim A (we use the kernel's i2c-dev and GPIO drivers, not our own), C (no
real-time scheduling), or F (one record a minute, not a high-rate pipeline). B:
the light reader uses GPIO edge events, but we have no polling version to
compare against, so we do not claim it.

| | What the course asks | Measured so far | Not measured yet | Test that closes it |
|---|---|---|---|---|
| D | Append-only log with a crash-consistency story | P1: 500 of 500 `kill -9` passed (2026-11-01). P2: 2 of 10 power pulls passed (2026-11-02). Both on the fake feeder. | 8 more pulls; the once-a-minute rate; a pull with the real hub writing | P2 |
| E | Sensors in separate processes, a hub, and a supervisor that survives any child dying | Supervisor tests with stand-in children: restart with backoff, a silent child, SIGKILL after the grace period, crash loop to DEGRADED (`supervisor_behavior`). On the Pi (48-hour soak, 2026-11-11 to 2026-11-13): the systemd unit ran the supervisor and four children, the real soil and light processes were each killed with `kill -9` once (`died signal=9`), and the unit stopped cleanly with status 0. Restart timing from the report: 12 s (soil) and 61 s (light) back to running. No RSS growth | The store check from the soak (record count, gaps); the real hub killed under the supervisor; the rehearsal, L6 part B and the soil unplug were not run | P3, P4 (store check pending), S6/L6 |

Until those rows are filled in, any claim about E on real hardware must say it
rests on stand-in children only.


## Ownership Map

### Andrew Faga: mechanism E (multi-process architecture) and wiring

| Path | Purpose |
|---|---|
| src/sensors | One reader per sensor, each run as its own isolated child process. The soil reader reads the STEMMA sensor over I2C at address 0x36, and the light reader reads the LM393 module's DO pin on GPIO17 (bright or dark). Each sends its readings to the hub and sends a heartbeat. A failure here must not affect the other sensor. |
| src/ipc | The communication layer between the supervisor, the hub and the sensor processes. It implements the messages defined in include/ipc.h. |
| src/hub | The only writer. It collects the soil and light readings once per cycle (once a minute, optionally a median of several quick samples), builds one record (timestamp, sequence number, raw soil value, light flag), and calls store_append. It notices a missing reading from a sensor. |
| src/supervisor | Starts the hub and each sensor process, watches their exit codes and heartbeats (a heartbeat must arrive within 60s of start), and restarts or resets a failed one. It writes supervisor events to a separate text log. It must leave no orphaned file descriptors across a restart. |
| Makefile | Build and test targets: make, make test, make asan, make memcheck, make soakcheck, make deploy. |
| diagram.json and pin map | The wiring design: which Pi pins connect to the soil sensor (3.3 V, GND, SDA, SCL) and to the light module (3.3 V, GND, DO on GPIO17, Pi pin 11), the soil sensor's I2C address (0x36), and the light module's threshold setting (blue pot). |

### Duke Bautista: mechanism D (storage layer), analysis, and the physical build

| Path | Purpose |
|---|---|
| src/store | The custom storage layer. It keeps an append-only log of fixed-size binary records (about 24 bytes), each ending in a CRC32, in one segment file per day or week. It calls fsync after every append and after creating a segment. On startup it scans the newest segment, truncates after the last valid record, and logs how many bytes were dropped. It also serves reads by time range. |
| src/analysis | Reads the history through store_read_range. It combines soil and light readings to judge whether the plant needs water and how soon, applies per-plant thresholds, and flags drift or noise in the sensors. All of it is written by us. Built so far (#27): `dry.c` averages the good soil samples into [10 min] buckets, finds the dry-down cycles (a rise of [60] counts is watering) and fits a line to each; `predict.c` takes the median slope of the accepted cycles to estimate the hours until the soil reaches the dry threshold, or says why it will not; `analyze.c` joins them to the store. Built for #43: `thirst.c` decides needs-water, ok or unknown from the newest good soil samples (Schmitt trigger, band [48], stale after [3 min]); the dry threshold is an input (default 450). Built for #26: `lightadj.c` fits the drying rate of the accepted cycles against their bright hours per day (least squares, `dry.c` now also keeps each cycle's light) and reads the current rate off that line at the light the open cycle has had so far; it holds back, with the reason, with fewer than [5] cycles, a light spread under [2] h/day, too little valid light, a light outside the fitted range, or a rate that is not falling. Its backtest is test C3 (see Part 3). Drift flagging is not built yet. |
| src/interface | The LAN page. `page.c` decides the state and renders the HTML and JSON, `http.c` is the strict request parser, response builder and private-address check, `daemon.c` is the poll loop (sockets, heartbeat, store refresh) and `main.c` is the program. It makes no calls outside the local network, and `tools/nonetcheck.sh` and the lifecycle test check that. |
| tools/ | Test helpers: a fake sensor feeder, a log checker (every CRC passes, sequence numbers are contiguous with at most one lost record, a recovery entry is logged), a CSV export script, `gt-analyze` (runs the dry-down analysis on a store and prints the cycles and the prediction), a kill loop that runs kill -9 on the writer at random moments, and a scan of the source and the built page daemon that enforces the no-outbound-network rule (nonetcheck.sh). |
| Physical build | Duke builds Andrew's wiring on the Pi and runs the hardware tests. |

### Shared contracts (frozen first; changed only with both owners' agreement)

| File | Purpose |
|---|---|
| include/record.h | The on-disk record layout: field order and widths, byte order, size, and a way to mark a bad reading. The store writes it and the analysis reads it, so any change is a format change. The raw soil field holds the STEMMA sensor's capacitive reading, which the sensor returns as a 16-bit value, and the light field is a single bright/dark flag. Both widths are fixed here. |
| include/ipc.h | The messages the processes exchange: reading messages, the heartbeat, and the supervisor and child rules (exit codes, restart conditions). |
| include/store.h | The storage API that the hub and analysis use: store_append, store_recover, store_read_range, with their signatures and error handling. |

### How the two halves meet
- The hub (Andrew) calls the store (Duke) only through store.h.
- Duke tests with the fake sensor feeder and does not depend on Andrew's drivers. Andrew develops against a stub store that prints records.

### Hardware workflow
Andrew designs the wiring and Duke builds it. Hardware bugs are reported with pasted evidence (dmesg, i2cdetect -y 1, raw sensor read, GPIO17 level), and Andrew diagnoses them.


## AI-Use Plan
This project will requires us to build a smart device that uses two sensors to analyze data and make decisions using code you write yourself, without relying on AI models or cloud services. Our program must do more than check simple thresholds; it should combine sensor information to identify patterns or conditions. You can use mathematical libraries as long as you understand how they work. The device must function without an internet connection and implement at least two operating-system mechanisms from the assignment, such as custom drivers, shared memory, or a watchdog.

