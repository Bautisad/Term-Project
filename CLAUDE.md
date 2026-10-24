# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Project Green Thumb: team rules

## Commands
- Build: ‘make‘ Cross/deploy: ‘make deploy PI=pi@<host>‘
- Tests: ‘make test‘ Sanitizers: ‘make asan‘ Valgrind: ‘make memcheck‘
- Soak sanity: ‘make soakcheck‘ (1-hour miniature of the 48h run)
- A change is DONE only when build, test, and asan pass. Show output.
## Hard constraints
- The PRODUCT makes no network calls except serving its own LAN
interface. No LLM APIs, no cloud inference, no pretrained models.
The intelligence in analysis/ is ours. This is a graded boundary.
- Every daemon must be supervisable: clean exit codes, no orphaned
fds across restart, heartbeat within 60s of start.
- Every allocation checked; every syscall’s error path handled and
logged. The 48-hour soak is the test suite of last resort.
- NEVER weaken, skip, or delete a test to make the suite pass.
## Ownership
- Andrew Faga: multi-process architecture (src/sensors, src/ipc, src/hub,
  src/supervisor), Makefile, wiring design (diagram.json, pin map)
- Duke Bautista: storage layer and analysis (src/store, src/analysis,
  src/interface, tools/), test harness, physical build and hardware
  testing on the Pi
- Shared contracts: include/record.h, include/ipc.h, include/store.h.
  Frozen first; change only with both owners’ agreement.
- Hardware bugs need pasted evidence from the Pi (dmesg, i2cdetect -y 1,
  raw ADC read). Duke runs the tests and pastes; Andrew diagnoses.
- State whose session it is at the start of every session. The agent
  edits outside that owner’s area only when told explicitly.
## Style
- Systems core: C17, -Wall -Wextra -Werror, no VLAs.
goto-cleanup for multi-resource functions.
- Python permitted only under tools/ and ui/. No graded mechanism
may live there.
- Smallest diff that passes. Do not refactor unrelated code.
## Workflow
- Multi-file or algorithmic change: plan first, wait for approval.
- Hardware bugs: paste real evidence (dmesg, timing capture,
/proc/interrupts). No fixes proposed from a verbal description.
- Commit only from a green state; message format "M<n>: <what>".

## Project status

This repository currently contains only planning documentation (`PROBLEMMEMO.md`) for a CS370 term project. No source code, build system, or tests exist yet. There are no build/lint/test commands to run at this stage — when code is added, this file should be updated with the relevant commands.

## Project overview

The project is a device for monitoring houseplant soil moisture and light exposure, aimed at owners who under- or over-water plants due to unreliable manual moisture checks.

Planned hardware:
- Capacitive Soil Moisture Sensor
- BH1750 Light Sensor (ambient light readings are correlated with moisture readings to judge watering urgency)

Planned architecture (from `PROBLEMMEMO.md`):
- **Custom storage layer** — retains a history of moisture and light readings over multiple weeks (not just point-in-time values).
- **Multi-process architecture** — the system is meant to run unattended for days at a time. Each sensor is isolated behind its own "supervisor" process, so a single sensor/driver failure doesn't take down the whole system; the supervisor is responsible for resetting a sensor when it fails.

Known risk area: cheap soil moisture sensors are prone to miscalibration and corrosion over time, which the design needs to account for (e.g. via the supervisor/reset mechanism).
