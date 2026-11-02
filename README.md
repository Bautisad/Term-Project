# Project Green Thumb

A small device that watches one houseplant's soil moisture and light, so the owner knows when to water instead of guessing. A Raspberry Pi 5 reads a capacitive soil moisture sensor and a bright/dark light module, keeps weeks of history in its own crash-safe log, and will show a "needs water" state on a page served only to the local network.

**Status:** hardware bring-up, the build system, the supervisor, the IPC layer and the two sensor processes work in tests. The sensor drivers have been checked on the Pi (see [`HARDWARE-CHECKS.md`](HARDWARE-CHECKS.md)). The LAN page (`gt-interface`) runs on the Pi and loads on a phone, fed by a fake sensor feeder (see [View the page](#view-the-page)). The hub and analysis are still being written (see [Not built yet](#not-built-yet)), so there is nothing to run end to end with the real sensors yet. This README covers wiring the hardware, checking that it works, building and deploying what exists, and viewing the page. Run instructions for the whole device will be added when it runs.

## Ground rules

- The product makes no network calls except serving its own LAN page. No cloud services, no LLM APIs, no pretrained models. The analysis code is written by us. `make test` checks the network rule with `tools/nonetcheck.sh` (a scan of `src/` and of the built `gt-interface`) and with a run-time check that every socket the running page daemon holds is local or its own.
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
   The grid should show `36`. `i2cdetect` leaves the soil sensor returning `0xffff` (65535) on the next read, so expect that in step 2 and read again. The reads after it are normal.

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

   Your numbers will differ with your sensor, soil and insertion depth. Push the probe in up to the white line and wait about ten seconds before reading. The first read after moving the probe is often off, so take several. Fix the probe in place and do not move it between readings: in our test, pulling it out and pushing it back in ten times changed the reading by up to 137 counts, a third of the dry-to-wet range.

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

Today this builds `gt-supervisor`, `gt-sensor-soil`, `gt-sensor-light`, `gt-interface` and the test helpers. The other programs are built automatically once their source files exist.

## View the page

`gt-interface` serves the LAN page: an HTML page at `/` and the same state as JSON at `/status.json`. With no hub yet, feed it with the fake sensor feeder. On the Pi, from the repository after `make`, in two terminals:

```
build/tools/gt-feeder --dir /tmp/gt-demo --rate 1
build/gt-interface --dir /tmp/gt-demo
```

Then open `http://greenthumb.local:8080/` on a phone or laptop on the same network (we tried this on an iPhone in Safari). `curl -s localhost:8080/status.json` on the Pi shows the JSON. The values are the feeder's made-up numbers, not sensor readings.

- **Address and port:** with no flags it listens on every interface, port 8080. `--port N` and `--bind IPV4` change that.
- **Who is served:** only private IPv4 addresses (127/8, 10/8, 172.16/12, 192.168/16 and 169.254/16). Anything else is closed without an answer.
- **The store:** `--dir PATH` says where to read it (read only). The default, `/var/lib/greenthumb/store`, is where the hub is meant to write.
- **What it shows:** the newest valid soil value and light state, how old they are, and a count of the last 24 hours of readings. "Needs water" and "time until dry" say "not enough data" until the analysis exists.
- **When it says degraded:** it lists the reasons, for example no new reading for 3 minutes, a store it cannot read, invalid readings, an under-voltage event, a clock that is ahead, or low disk space. It never shows OK for a state it cannot judge.
- **Not started by the supervisor yet:** run it by hand as above. Other flags (`--heartbeat-ms`, `--refresh-ms`) are for the tests.

## Troubleshooting

- **`36` is missing from `i2cdetect -y 1`:** check that I2C is enabled in `raspi-config` and that SDA (pin 3) and SCL (pin 5) are wired the right way round. Also check 3.3 V and GND on the soil sensor.
- **The first soil read is `0xffff` (65535):** you probably ran `i2cdetect` just before. In our tests this happened on the first read after `i2cdetect` every time (4 of 4) and never otherwise (10 of 10). Read again; the next reads are normal. Do not run `i2cdetect` right before a measurement.
- **The soil reading jumps after you moved the probe:** see step 2 above. Fix the probe in place and wait about ten seconds after placing it.
- **`get_throttled` is not `0x0`:** use a proper Pi 5 power supply and a short, good cable.
- **The light output never changes:** check 3.3 V, GND and the DO wire on pin 11, then turn the blue pot. The module's DO LED should flip when the light crosses the threshold.
- **The page does not load on a phone:** the phone must be on the same network as the Pi, and the Pi only answers private addresses, so a guest or public network is refused. `greenthumb.local` worked on an iPhone; some phones cannot resolve `.local` names, so try `http://<pi-ip>:8080/` with the address from `hostname -I`. On the Pi, `pgrep -a gt-interface` shows whether it is running and `ss -ltn | grep 8080` whether it is listening.
- **Reporting a hardware problem:** include the output of `dmesg | tail -50`, `i2cdetect -y 1`, a raw sensor read, and the GPIO17 level. A description alone is not enough to diagnose it.

## Not built yet

These are the parts still to come. The issue numbers are on the repository's issue tracker.

- The hub that collects the sensor readings and writes the store, and supervisor heartbeat deadlines
- Analysis: dry-down detection and time-until-dry (#27), light-adjusted prediction (#26)
- The "needs water" state on the LAN page (#43), and starting the page from the supervisor (`./gt-interface` is not in its default list yet)
- Sensor calibration tests and crash-safety runs (#32, #29, #30, #31)

## More reading

- [`PROBLEMMEMO.md`](PROBLEMMEMO.md): who it is for and the problem it solves
- [`DESIGN.md`](DESIGN.md): failure modes, storage format, constraints and the evaluation plan
- Code layout: `include/` holds the contracts shared between components, `src/supervisor/` the supervisor, `tests/` the tests, and `cmake/` the Pi 5 cross-compile toolchain.
