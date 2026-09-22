# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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
