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


## AI-Use Plan
This project will requires us to build a smart device that uses two sensors to analyze data and make decisions using code you write yourself, without relying on AI models or cloud services. Our program must do more than check simple thresholds; it should combine sensor information to identify patterns or conditions. You can use mathematical libraries as long as you understand how they work. The device must function without an internet connection and implement at least two operating-system mechanisms from the assignment, such as custom drivers, shared memory, or a watchdog.

