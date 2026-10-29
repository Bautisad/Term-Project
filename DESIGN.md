## Design Document


## Architecture


## Mechanism Mapping


## Failure-Mode Table
**How to read this table.** For each component: how it can fail, how we detect it, what the system does about it, and what the log shows. The "Verified by" column points to the test that proves the row (IDs from the evaluation plan). `NEW` means we still need to write that test. Every log line below is a **draft**: once we inject the failure for real, we replace it with the line the system actually printed.
 
**Log line format:** `<ISO time> <LEVEL> <component> <event> key=value ...`
 
Thresholds in [brackets] are our first guesses and will be set from measurements.
 
| # | Component | How it fails | How we detect it | What the system does | What the log shows (draft) | Verified by |
|---|---|---|---|---|---|---|
| 1 | Soil sensor | Wire loose or disconnected. The I2C read may fail outright (row 4) or return a value stuck at 0 or full scale (row 5). | Raw value outside [calibrated dry − margin, wet + margin], or noise far above normal | Soil field marked INVALID, never stored as a good reading. Analysis skips it. Light and hub keep running. Sampling continues, so it recovers when the wire is back. | `WARN soil invalid reason=out_of_range raw=32767 bounds=[250,1100] seq=4812` | S6 |
| 2 | Soil sensor | Stuck reading | Identical raw value for [60] consecutive samples. Real soil plus sensor noise rarely does this, but very stable soil might (see note). | Flag STUCK, log once, keep storing the raw value, exclude it from analysis | `WARN soil stuck value=14230 samples=60 seq=4850` | S6 |
| 3 | Soil sensor | Noise spike | Sample differs from the median of the last [5] by more than [N]% of span | Median-of-N filter discards the spike and counts it | `INFO soil spike_rejected raw=29100 median=14620 seq=4901` | S2 |
| 4 | Soil sensor (I2C) | I2C transfer to the soil sensor (0x36) fails | The syscall returns an error (errno is logged) | Retry [3] times, then mark this cycle INVALID. After [5] failed cycles in a row, exit nonzero so the supervisor restarts the process. | `ERROR soil read_failed errno=5 attempt=3/3 seq=4902` | NEW (unplug the soil sensor SDA/SCL, or disable I2C) |
| 5 | Soil sensor (I2C) | Bad wiring returns a constant 0 or full-scale with no error. The transfer itself succeeds, so it looks like a real reading. | Same range and stuck checks as rows 1 and 2 | Same as rows 1 and 2 | `WARN soil invalid reason=out_of_range raw=0 seq=4903` | S6 |
| 6 | Light sensor (LM393 module) | DO wire disconnected, or the module unpowered. The GPIO17 input floats or sits at one level, so a dead sensor can look like constant darkness or brightness. | The flag never changes in [48 h] (a plant sees day and night), or it toggles more than [N] times in an hour (a floating input chatters). | Light field marked INVALID and flagged separately. Never stored as a real bright or dark reading. Retried each cycle so it recovers when reconnected. | `WARN light invalid reason=stuck_flag hours=48 seq=4920` then `INFO light recovered after_cycles=14 seq=4934` | L6 |
| 7 | I2C bus | I2C bus hangs (for example SDA held low) and the read blocks. Only the soil sensor is on the bus, so soil readings stop. The light flag is read from GPIO17 and keeps working. | Read exceeds a [2 s] timeout, and the process heartbeat stops | Supervisor sends SIGTERM, then SIGKILL after [5 s], and restarts the soil reader. If the bus is still stuck after [3] restarts, mark soil DEGRADED and keep the light reader, hub and store running. | `WARN supervisor child=soil heartbeat_missed age=35s` then `ERROR supervisor child=soil killed signal=SIGKILL restart=1/3` | NEW (SIGSTOP the process to simulate a hang; this does not simulate a hung bus) |
| 8 | Sensor process | Crash (segfault, abort, nonzero exit) | Supervisor `waitpid()` reports the exit status or signal | Restart with exponential backoff ([1 s, 2 s, 4 s ... cap 60 s]). Other sensors are unaffected. | `ERROR supervisor child=soil pid=2210 died signal=11` then `INFO supervisor child=soil restart=1 delay=1s` then `INFO supervisor child=soil heartbeat pid=2231` | P3 |
| 9 | Sensor process | Hang (alive but doing nothing) | No heartbeat for [30 s] (hard limit 60 s) | SIGTERM, then SIGKILL, then restart as in row 8 | As row 7 | P3 (add a SIGSTOP variant) |
| 10 | Sensor process | Crash loop | More than [5] restarts in [5 min] | Stop fast restarts and hold at the backoff cap. Mark the child DEGRADED. Keep the rest of the system running. | `ERROR supervisor child=soil crash_loop restarts=5 window=300s state=DEGRADED` | NEW |
| 11 | Hub | Crash or hang | Same as rows 8 and 9 | Restart. On startup it reads the last sequence number from the store so numbering continues. Readings during the downtime are lost, and the sequence gap shows it. | `INFO hub start resumed_seq=4813 gap=2` | P3 |
| 12 | IPC | Message lost or malformed | Hub checks message length and version. Sender sees EPIPE or EAGAIN. | Drop the bad message and log it. The sender reconnects. | `WARN hub bad_message from=soil len=9 expected=16` | NEW |
| 13 | Supervisor | Supervisor itself dies | External watcher: [systemd `Restart=always`]. If we don't use systemd, this is a single point of failure and we say so here. | systemd restarts it. On startup it kills any orphaned children so there are no duplicates. | journald: `supervisor.service: Main process exited` then ours: `INFO supervisor start orphans_killed=2` | NEW (`kill -9` the supervisor) |
| 14 | Store | Torn write on power loss or `kill -9` | Short final record or CRC mismatch at the tail of the newest segment, checked on startup | Truncate at the last valid record and continue | `WARN store recovered segment=2026-10-25.log dropped_bytes=17 last_seq=4811` | P1, P2 |
| 15 | Store | Corruption in the middle of a log (bit rot) | CRC mismatch on a record that isn't at the tail | Skip it. Fixed-size records let the reader resync at the next boundary. The gap is marked in the CSV export. | `ERROR store crc_mismatch segment=2026-10-25.log offset=2400 skipped=1` | NEW (flip bytes in a copy of a log) |
| 16 | Store | Disk full | Write returns ENOSPC | Stop appending but keep sensing. Show DEGRADED on the LAN page. Events go to stderr and journald. | `ERROR store append_failed errno=28 seq=5200` | NEW (small loopback filesystem) |
| 17 | SD card | Filesystem remounts read-only | Write returns EROFS or EIO | Same as row 16. A human has to fix it. | `ERROR store append_failed errno=30 seq=5201` | NEW |
| 18 | Clock | Wrong time after reboot. We make no network calls, so there's no NTP, and the Pi's real-time clock needs a backup battery to keep time. | New timestamp earlier than the last stored one, or earlier than the build time | Flag the record TIME_UNSET. Rely on sequence numbers and the monotonic clock until the time is fixed. | `WARN clock backwards now=2026-01-01T00:00:03 last=2026-10-25T19:00:00 seq=5300` | NEW (set the clock back with `date`) |
| 19 | Power | Brownout or under-voltage. Both sensors run from the 3.3 V rail, so a sag can shift the soil reading or flip the light flag. | Pi under-voltage flag (kernel message, or `vcgencmd get_throttled`). We need to confirm this works on the Pi 5. | Log it and flag samples taken during the event | `WARN power undervoltage seq=5400` | NEW |
| 20 | Analysis | Wrong or unsupported prediction (no crash) | Too few dry-down cycles recorded, or recent data is INVALID or STUCK | Withhold the prediction and show "not enough data" | `INFO analysis prediction_withheld reason=insufficient_cycles have=2 need=3` | C3 |
| 21 | LAN page | Process dies | Supervisor, as in row 8 | Restart. Logging is unaffected because processes are isolated. | As row 8 | P3 |
 
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

The glass of water reads below dry soil. That was repeatable in two runs and was not investigated; the product reads soil, so S1 uses soil only. A single soil reading varies by about 16 counts (one standard deviation) in these sets. The invalid-reading bounds in the failure table (`[250,1100]`) are drafts built from these numbers and get refined after S1.

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
| S1 | Monotonic, R² ≥ 0.9 | not run | DESCOPED | 2026-10-29 | Descoped by Duke, not measured. Consequence: no measured dry/wet range and no pot-weight calibration, so the draft bounds `[250,1100]` (failure table row 1) and the per-plant dry threshold are not calibrated. Affects #31, #43 and C4. |
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

---

## Part 4: Making sure the sensor data survives

The sensors only matter if the log we build from them stays trustworthy. These tests cover our two graded mechanisms, D (crash-safe storage) and E (multi-process architecture with a supervisor).

| ID | Test | Method | Target |
|----|------|--------|--------|
| P1 | Surviving `kill -9` on the writer *(correctness)* | Run a fake sensor feeder at about 1,000 records/s so a random kill actually lands mid-write. Kill the writer after a random delay, restart, and run the log checker. | 500 kills, 0 CRC failures mid-log, at most 1 record lost per kill, and a "recovered" event logged each time. |
| P2 | Real power pulls *(correctness)* | Unplug the Pi mid-run, at the real once-a-minute rate and at the high rate. Reboot and run the checker. | 10 pulls, 0 unrecoverable logs, at most 1 record lost per pull. If a pull loses more than one record, the SD card may be reporting `fsync` as done too early, which is a real finding. |
| P3 | Restart after a child process is killed *(correctness)* | Kill the soil process, the light process, and the hub at random moments, 200 times each, and time the restart. | Median restart under 5 s and p99 under 10 s. The surviving sensor keeps logging and the gap shows up in the event log. |
| P4 | 48-hour soak with real sensors *(correctness)* | Run the full system unattended on the Pi with both real sensors for 48 hours, recording each process's memory (RSS) and open file counts every hour. | 2,880 records with no unexplained sequence gaps, RSS growth under 1 MB per process, constant fd counts, and 0 unplanned supervisor restarts. |


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
| src/analysis | Reads the history through store_read_range. It combines soil and light readings to judge whether the plant needs water and how soon, applies per-plant thresholds, and flags drift or noise in the sensors. All of it is written by us. |
| src/interface | The LAN-only interface that shows results to the user. It makes no calls outside the local network. |
| tools/ | Test helpers: a fake sensor feeder, a log checker (every CRC passes, sequence numbers are contiguous with at most one lost record, a recovery entry is logged), a CSV export script, and a kill loop that runs kill -9 on the writer at random moments. |
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

