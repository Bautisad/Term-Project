## Design Document


## Architecture


## Mechanism Mapping


## Failure-Mode Table


## Storage and Data
What is stored?

- One record per reading cycle: timestamp, sequence number, raw soil ADC value, light level in lux
- Raw soil value (not calibrated percent) so you can recalibrate later when the sensor drifts
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
| Light sensor | Calibrated lux sensor, checked against a reference meter | Cheap BH1750 module of unknown quality, [no reference meter / phone app only] | We can't vouch for absolute lux values. We use light as a relative signal (brighter vs. dimmer, day vs. night), not as a measurement. |
| Temperature | Temperature sensor beside the soil | None | Temperature affects both capacitive readings and drying speed. We can't separate it from moisture or light effects, so some of what we attribute to light may be temperature. |
| Time | A full season of dry-down cycles | [N] weeks until the deadline, roughly [N] dry-down cycles | Too few cycles for strong prediction statistics. We report results as a small sample. |
| Soil moisture reference | Lab-grade probe or oven-dry samples | Kitchen scale (pot weight) | Weight reflects the whole pot, not the soil at the probe. |
| ADC | 16-bit (ADS1115) | [MCP3008, 10-bit / ADS1115] | [Fewer usable steps across the sensor's range] |
| Power-loss testing | Many pulls on fresh SD cards | [10] pulls on one card | Limited by card wear. Most crash testing uses `kill -9` with a synthetic feeder. |

### Narrowed claims
- We claim the device predicts time-until-dry for **this plant in this pot**, within [X]% over [N] cycles. We do not claim it generalizes.
- We treat light as a **relative** signal. We claim it reliably separates day from night and bright from dim, not that it reports true lux.
- Without a temperature reading, we report any light-vs-dry-down relationship as a trend that temperature may also explain.

## Evaluation Plan
Our device uses two sensors, a capacitive soil moisture sensor and a BH1750 light sensor, to tell us when a houseplant needs water and why. Our design doc says the biggest risk is that drift or noise makes the readings look confident but wrong. So this plan mostly tests the sensors themselves: are they accurate, stable, and actually useful together? A short last section checks that the data survives crashes and power loss.

Every measurement has a method and a target. We're writing the targets down before running anything so we can't quietly move the goalposts later. If we miss one, we record the result and figure out why instead of editing the number.

A note on the targets: most sensor targets below are our best guesses before we've measured anything. If the first real runs show one is unrealistic, we can change it, but we'll add a note saying what we measured and why. Targets marked (correctness) are pass/fail and stay as written.


---


## Part 1: Soil moisture sensor

| ID | Test | Method | Target |
|----|------|--------|--------|
| S1 | Calibration curve | Weigh a pot of dry soil, saturate it, then take sensor readings at 5 moisture levels as it dries. Calculate water content from the weight at each point and plot raw reading against it. | The curve is monotonic (it never goes the wrong direction as soil dries) and R² ≥ 0.9. |
| S2 | Noise | Hold the soil at a fixed moisture and take 100 readings in a row. Compare the spread of single readings against the spread of median-of-5 readings. | Standard deviation under 1% of the dry-to-wet range after taking the median of several samples. |
| S3 | Reinsertion repeatability | At fixed moisture, pull the sensor out and push it back in 10 times, recording a reading each time. | Readings spread under 5% of the range. If it's worse, we'll document it and add "fix the probe in place" to the setup instructions. |
| S4 | Response time | Move the sensor from dry soil into wet soil and record readings every second until they stop changing. | Settles within 5% of its final value in 10 seconds. This only measures the sensor, not how fast water spreads through the pot. |
| S5 | Drift | Leave the sensor in a sealed container of soil at fixed moisture for 48 hours, logging once a minute. | Drift under 2% of the range over 48 h. This tests our named risk directly. |
| S6 | Fault detection | Disconnect the sensor wire, short it, and hold a reading constant to fake a stuck sensor. | Each fault is logged within 2 sample cycles, the light sensor keeps running, and clean data causes fewer than 1 false alarm per week. |

- **S1 why:** The raw ADC number means nothing until we know how it maps to actual wetness.
- **S3 why:** Capacitive sensors are sensitive to how well the soil touches the probe, and a plant owner will move it around.

---

## Part 2: Light sensor (BH1750)

| ID | Test | Method | Target |
|----|------|--------|--------|
| L1 | Accuracy against a reference | Compare the BH1750 to a reference lux meter at 5 light levels (dark room, dim lamp, normal room light, bright lamp, near a window). | Within ±15% of the reference. |
| L2 | Range and saturation | Record readings from darkness up to the brightest light the plant will ever get, such as a sunny window. Check the BH1750 datasheet for its maximum. | The sensor never saturates at the plant's actual location. If it does, we'll document where. |
| L3 | Angle sensitivity | Hold the light constant and rotate the sensor to 0°, 45°, and 90°, comparing readings. | Pick one mounting orientation and document how much the reading changes at the others. Our estimate is under 20% variation near the chosen angle. |
| L4 | Stability | Run the sensor for 48 hours in a fixed, constant light (a lamp on a timer-free circuit, or a closed box with a small light). | Drift under 5%. |
| L5 | Day/night cycle | Place the sensor at the plant for 3 days and plot the readings. | Readings drop to near 0 lux at night (lights off), peak during the day, and the daily pattern looks consistent from day to day. |
| L6 | Fault detection | Unplug the BH1750 mid-run, and separately cover it fully. | An unplugged sensor is logged as an error within 2 sample cycles, never recorded as "0 lux." Covering it is recorded as real darkness. |

- **L1 note:** Phone lux apps are not reliable. If that's all we can borrow, we'll say so in the results and treat the number as rough.
- **L6 why:** A disconnected sensor and a dark room can look similar if we aren't careful.

---

## Part 3: The two sensors together

This is the part that tests our design claim that the sensors are *correlated* instead of just sitting next to each other.

| ID | Test | Method | Target |
|----|------|--------|--------|
| C1 | Timestamp alignment | Check that each record's soil and light readings were taken close together. | The two readings in one record are within 1 second of each other. |
| C2 | Does light change the dry-down rate? | Record dry-down cycles for the same plant or identical pots in a bright spot and a dim spot, with a similar starting moisture. | A clearly measurable difference in dry-down rate between the two spots. If we find no difference, that's an important result, and we'd need to rethink why we're using light at all. |
| C3 | Light-adjusted prediction vs soil-only | Record at least 5 real dry-down cycles. At 12 hours before the plant crosses the dry threshold, predict "time until dry" two ways: using soil readings only, and using soil plus light. Compare both to what actually happened. | The light-adjusted prediction has lower median error than the soil-only one, and its median error is under 25% of the remaining time. |
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
| src/sensors | One reader per sensor, each run as its own isolated child process. The soil reader gets raw values from the ADC (MCP3008 or ADS1115) and the light reader gets lux from the BH1750 over I2C. Each sends its readings to the hub and sends a heartbeat. A failure here must not affect the other sensor. |
| src/ipc | The communication layer between the supervisor, the hub and the sensor processes. It implements the messages defined in include/ipc.h. |
| src/hub | The only writer. It collects the soil and light readings once per cycle (once a minute, optionally a median of several quick samples), builds one record (timestamp, sequence number, raw soil value, lux), and calls store_append. It notices a missing reading from a sensor. |
| src/supervisor | Starts the hub and each sensor process, watches their exit codes and heartbeats (a heartbeat must arrive within 60s of start), and restarts or resets a failed one. It writes supervisor events to a separate text log. It must leave no orphaned file descriptors across a restart. |
| Makefile | Build and test targets: make, make test, make asan, make memcheck, make soakcheck, make deploy. |
| diagram.json and pin map | The wiring design: which Pi pins connect to the ADC and BH1750, which ADC channel the soil sensor uses, and the I2C address of the BH1750. |

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
| include/record.h | The on-disk record layout: field order and widths, byte order, size, and a way to mark a bad reading. The store writes it and the analysis reads it, so any change is a format change. The ADC choice sets the width of the raw soil field. |
| include/ipc.h | The messages the processes exchange: reading messages, the heartbeat, and the supervisor and child rules (exit codes, restart conditions). |
| include/store.h | The storage API that the hub and analysis use: store_append, store_recover, store_read_range, with their signatures and error handling. |

### How the two halves meet
- The hub (Andrew) calls the store (Duke) only through store.h.
- Duke tests with the fake sensor feeder and does not depend on Andrew's drivers. Andrew develops against a stub store that prints records.

### Hardware workflow
Andrew designs the wiring and Duke builds it. Hardware bugs are reported with pasted evidence (dmesg, i2cdetect -y 1, raw ADC read), and Andrew diagnoses them.


## AI-Use Plan
This project will requires us to build a smart device that uses two sensors to analyze data and make decisions using code you write yourself, without relying on AI models or cloud services. Our program must do more than check simple thresholds; it should combine sensor information to identify patterns or conditions. You can use mathematical libraries as long as you understand how they work. The device must function without an internet connection and implement at least two operating-system mechanisms from the assignment, such as custom drivers, shared memory, or a watchdog.

