# Project Green Thumb

A small device that watches one houseplant's soil moisture and light, so the owner knows when to water instead of guessing. A Raspberry Pi 5 reads a capacitive soil moisture sensor and a bright/dark light module, keeps weeks of history in its own crash-safe log, and will show a "needs water" state on a page served only to the local network.

**Status:** hardware bring-up and the build system work today. The sensor readers, hub, storage layer, analysis and LAN page are still being written (see [Not built yet](#not-built-yet)), so there is nothing to run end to end yet. This README covers wiring the hardware, checking that it works, and building and deploying what exists. Run instructions will be added when the device runs.

## Ground rules

- The product makes no network calls except serving its own LAN page. No cloud services, no LLM APIs, no pretrained models. The analysis code is written by us.
- Each sensor runs in its own process under a supervisor, so one failing sensor does not take down the rest.

## Parts

- Raspberry Pi 5 with a power supply, running 64-bit Raspberry Pi OS (the exact release is not confirmed yet)
- Adafruit STEMMA capacitive soil moisture sensor (I2C, address 0x36)
- LM393 light module (a light-dependent resistor plus a comparator, with a digital output)
- Jumper wires

## Wiring

Both sensors run from the Pi's 3.3 V rail.

| Sensor pin | Pi pin |
|---|---|
| Soil sensor 3.3 V | pin 1 |
| Soil sensor GND | pin 6 |
| Soil sensor SDA | pin 3 (GPIO2) |
| Soil sensor SCL | pin 5 (GPIO3) |
| Light module 3.3 V | pin 17 |
| Light module GND | pin 9 |
| Light module DO | pin 11 (GPIO17) |

- The light module has no analog output and there is no ADC. It reports only bright or dark.
- The blue potentiometer on the light module sets the brightness level where the output flips. Set it once and leave it alone: moving it changes the threshold, and data from before and after cannot be compared.

## Set up the Pi

1. Enable I2C: run `sudo raspi-config`, choose **Interface Options**, then **I2C**, and enable it.
2. Install the command-line tools used below:
   ```
   sudo apt install i2c-tools gpiod
   ```

## Check the hardware

Run these on the Pi. If any step does not match, stop and see [Troubleshooting](#troubleshooting).

1. **Soil sensor is on the bus.**
   ```
   sudo i2cdetect -y 1
   ```
   The grid should show `36`.

2. **Read the soil sensor.**
   ```
   i2ctransfer -y 1 w2@0x36 0x0F 0x10 r2
   ```
   This prints two bytes, a 16-bit value with the high byte first (`0x02 0x91` is 0x0291, which is 657). Higher means wetter. Readings from our build, using ten reads per condition:

   | Condition | Reading |
   |---|---|
   | Probe in air | about 320 |
   | Very dry soil | about 565 |
   | Wet soil | about 977 |

   Your numbers will differ with your sensor, soil and insertion depth. Push the probe in up to the white line and wait about ten seconds before reading. The first read after moving the probe is often off, so take several.

   Do not use a glass of water as the wet reference. In our build it read about 522, lower than dry soil, and we have not explained why. Use wet soil.

3. **Read the light module.**
   ```
   gpioget -c gpiochip4 17
   ```
   On our module the output is `active` when it is dark and `inactive` when it is lit. Cover the sensor and then shine a lamp on it to confirm yours behaves the same way. Turn the blue pot until the module's DO LED flips at the light level you want to count as the boundary.

4. **Power is healthy.**
   ```
   vcgencmd get_throttled
   ```
   It should print `throttled=0x0`. Anything else means the Pi saw an under-voltage or throttling event, which can shift the soil reading or flip the light output.

## Build, test and deploy

Run these on a Linux machine (WSL works). The CMake presets only configure on Linux.

You need `cmake` 3.21 or newer, a C17 compiler and `make`. Cross-compiling and deploying also need `gcc-aarch64-linux-gnu`, `ssh` and `rsync`, and `make memcheck` needs `valgrind`. On Debian or Ubuntu:

```
sudo apt install build-essential cmake rsync valgrind gcc-aarch64-linux-gnu
```

| Command | What it does |
|---|---|
| `make` | Builds for this machine. |
| `make test` | Builds, then runs the tests. |
| `make asan` | Builds with AddressSanitizer and UBSan, then runs the tests. |
| `make memcheck` | Runs the tests under Valgrind. |
| `make soakcheck` | A 1-hour miniature of the 48-hour soak test. For now it runs the fake sensor feeder, not the full system. It prints each process's memory and file counts, then PASS or the failed checks. |
| `make deploy PI=pi@<host>` | Cross-compiles statically for the Pi 5 and copies the binaries to `~/greenthumb/bin` on the Pi. |

A change counts as done only when `make`, `make test` and `make asan` all pass.

Today this builds only `gt-supervisor` and a test helper. The other programs are built automatically once their source files exist.
## Troubleshooting

- **`36` is missing from `i2cdetect -y 1`:** check that I2C is enabled in `raspi-config` and that SDA (pin 3) and SCL (pin 5) are wired the right way round. Also check 3.3 V and GND on the soil sensor.
- **`get_throttled` is not `0x0`:** use a proper Pi 5 power supply and a short, good cable.
- **The light output never changes:** check 3.3 V, GND and the DO wire on pin 11, then turn the blue pot. The module's DO LED should flip when the light crosses the threshold.
- **Reporting a hardware problem:** include the output of `dmesg | tail -50`, `i2cdetect -y 1`, a raw sensor read, and the GPIO17 level. A description alone is not enough to diagnose it.

## Not built yet

These are the parts still to come. The issue numbers are on the repository's issue tracker.

- Sensor readers (soil over I2C, light on GPIO17) and the hub that collects their readings
- Storage layer in `src/store`: segment files, CRC, crash recovery and range reads (#37, #36, #35, #34, #33)
- Test tools: fake sensor feeder, log checker, kill loop (#41, #40, #39)
- Analysis: dry-down detection and time-until-dry (#27), light-adjusted prediction (#26)
- LAN page (#25) and the "needs water" state on it (#43)
- Sensor calibration tests and crash-safety runs (#32, #29, #30, #31)

## More reading

- [`PROBLEMMEMO.md`](PROBLEMMEMO.md): who it is for and the problem it solves
- [`DESIGN.md`](DESIGN.md): failure modes, storage format, constraints and the evaluation plan
- Code layout: `include/` holds the contracts shared between components, `src/supervisor/` the supervisor, `tests/` the tests, and `cmake/` the Pi 5 cross-compile toolchain.
