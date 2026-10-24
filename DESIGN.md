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


## Evaluation Plan


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

