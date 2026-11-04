# Project Green Thumb

A small device that watches one houseplant's soil moisture and light, so the owner knows when to water instead of guessing. A Raspberry Pi 5 reads a capacitive soil moisture sensor and a bright/dark light module, keeps weeks of history in its own crash-safe log, and shows a "needs water" state on a page served only to the local network (against a dry threshold that is not calibrated yet).

**Status:** hardware bring-up, the build system, the supervisor (restarts with backoff, heartbeat deadlines, crash-loop DEGRADED), the IPC layer, the two sensor processes and the hub (`gt-hub`, the only writer of the store) work in tests. The sensor drivers have been checked on the Pi (see [`HARDWARE-CHECKS.md`](HARDWARE-CHECKS.md)). The LAN page (`gt-interface`) runs on the Pi and loads on a phone, fed by a fake sensor feeder (see [View the page](#view-the-page)). The dry-down analysis (finding dry-downs, estimating the hours until the soil is dry, and a second estimate adjusted for light) works in tests on synthetic data (see [Analyze the history](#analyze-the-history)). The hub has not yet run on the Pi with the real sensors, and the rest of the analysis is still being written (see [Not built yet](#not-built-yet)), so the whole device has not been run end to end yet. This README covers wiring the hardware, checking that it works, building, deploying and starting the device, and viewing the page. The start steps in [Quick start](#quick-start) have not been tried on the Pi yet.

## Ground rules

- The product makes no network calls except serving its own LAN page. No cloud services, no LLM APIs, no pretrained models. The analysis code is written by us. `make test` checks the network rule with `tools/nonetcheck.sh` (a scan of `src/` and of the built `gt-interface`) and with a run-time check that every socket the running page daemon holds is local or its own.
- Each sensor runs in its own process under a supervisor, so one failing sensor does not take down the rest.

## Quick start

**Not tried yet.** These steps come from the code and the systemd unit; nothing here has run on the Pi.

You need a Raspberry Pi 5 with the sensors wired, and a Linux machine (WSL works) to build on and deploy from. The Pi user must be able to use `sudo`.

1. Wire the sensors: [Parts](#parts) and [Wiring](#wiring).
2. Set up the Pi and check the hardware: [Set up the Pi](#set-up-the-pi) and [Check the hardware](#check-the-hardware). Do not go on until the soil sensor and the light module both read.
3. Check that you can log in to the Pi from the Linux machine. SSH must be enabled on the Pi (`sudo raspi-config`, **Interface Options**, **SSH**), and both machines must be on the same network.
   - On the Pi, run `whoami` (that is your `<user>`) and `hostname -I` (the first address is your `<ip>`, such as `192.168.1.50`).
   - On the Linux machine, run `ssh <user>@<ip> echo ok`. It must print `ok` and must not ask for a password. If it asks for one, run `ssh-copy-id <user>@<ip>` once, then try again.
   - Use the IP address, not a `.local` name. WSL cannot look up names like `greenthumb.local`, and the address can change after a reboot unless your router reserves it.
   - If `ssh-copy-id` says `Permission denied (publickey)`, the Pi only accepts keys. See [Troubleshooting](#troubleshooting).
4. On the Linux machine, install the build tools ([Build, test and deploy](#build-test-and-deploy)), clone this repository and run `make test`.
5. Open `systemd/greenthumb.service` and replace `dbautista` with your Pi username in `User=`, `WorkingDirectory=` and `ExecStart=`. Skip this if your Pi user is `dbautista`.
6. Set `PI` to your Pi login, then deploy, install the service and start it with one command:
   ```
   PI=<user>@<ip>
   make deploy PI=$PI && scp systemd/greenthumb.service $PI:/tmp/ && ssh -t $PI 'getent group i2c gpio && sudo cp /tmp/greenthumb.service /etc/systemd/system/ && sudo systemctl daemon-reload && sudo systemctl enable greenthumb && sudo systemctl restart greenthumb && systemctl status greenthumb --no-pager'
   ```
   For example `PI=pi@192.168.1.50`. Each part runs only if the one before it worked, so a failed build never reaches the Pi. If it stops with no message, a group is missing: run `getent group i2c gpio` on the Pi, which must print both lines.
7. Check it: [Run on the Pi](#run-on-the-pi) lists what you should see. Then open `http://<ip>:8080/` on a phone or laptop on the same network.

After a code change, redeploy and restart without reinstalling the service:

```
make deploy PI=$PI && ssh -t $PI 'sudo systemctl restart greenthumb && systemctl status greenthumb --no-pager'
```

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
| `make deploy PI=<user>@<ip>` | Cross-compiles statically for the Pi 5 and copies the binaries to `~/greenthumb/bin` on the Pi. |

A change counts as done only when `make`, `make test` and `make asan` all pass.

Today this builds `gt-supervisor`, `gt-hub`, `gt-sensor-soil`, `gt-sensor-light`, `gt-interface` and the test helpers. The other programs are built automatically once their source files exist.

## Run on the Pi

**Not tried yet.** These steps come from the code and the systemd unit; nothing here has run on the Pi. Replace the "expect" lines with real output once someone has run it.

[Quick start](#quick-start) does steps 1 to 3 below in one command. Here is what that command does, one step at a time:

1. Deploy from your Linux machine: `make deploy PI=<user>@<ip>`. The binaries land in `~/greenthumb/bin` on the Pi. This copies only the binaries, not the service file.
2. On the Pi, check the hardware first ([Check the hardware](#check-the-hardware)), then check the groups the unit needs: `getent group i2c gpio` must print both lines, or systemd refuses to start it.
3. Copy `systemd/greenthumb.service` from the repository to the Pi and install it. It names the user `dbautista` and `/home/dbautista/greenthumb/bin`; edit those if your Pi user or deploy path is different. `restart` is used instead of `enable --now` so a service that is already running picks up new binaries.
   ```
   scp systemd/greenthumb.service <user>@<ip>:/tmp/
   sudo cp /tmp/greenthumb.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable greenthumb
   sudo systemctl restart greenthumb
   ```
4. Check it. Expect `systemctl status greenthumb` to say `active (running)`, `journalctl -u greenthumb` to show `start child=` for `gt-hub`, `gt-sensor-soil`, `gt-sensor-light` and `gt-interface` and then `state=running` for each, `/run/greenthumb/hub.sock` to exist, `curl -s localhost:8080/status.json` to answer, and `/var/lib/greenthumb/store` to hold a segment file that grows by one record a minute.
5. Break something. `sudo pkill -9 gt-hub`: expect `died signal=9` then `restart=1 delay=1s` in the journal, and a gap in the sequence numbers where the hub was down. Unplug the soil sensor: expect INVALID soil readings, and `giving_up` after five failed cycles.
6. Stop it: `sudo systemctl stop greenthumb`. Expect `stopped` in the journal and exit status 0.

The 48-hour soak is not wired up yet: `tools/soakcheck.sh` still drives the feeder, not these processes.

## View the page

`gt-interface` serves the LAN page: an HTML page at `/` and the same state as JSON at `/status.json`. With no hub yet, feed it with the fake sensor feeder. On the Pi, from the repository after `make`, in two terminals:

```
build/tools/gt-feeder --dir /tmp/gt-demo --rate 1
build/gt-interface --dir /tmp/gt-demo
```

Then open `http://greenthumb.local:8080/` on a phone or laptop on the same network (we tried this on an iPhone in Safari). `curl -s localhost:8080/status.json` on the Pi shows the JSON. The values are the feeder's made-up numbers, not sensor readings, and the page says so (next list).

- **Fake data is labeled:** every record `gt-feeder` writes carries the `SYNTHETIC` flag (`include/record.h`), and the hub never sets it. The page shows a dark "SYNTHETIC DATA: n of m readings..." banner when any reading in the last 24 hours is flagged, and `/status.json` has `"source"` (`none`, `live`, `synthetic` or `mixed`) and `"synthetic_records"`. The feeder logs `start source=SYNTHETIC`. `gt-analyze` prints a `WARNING: n of m records are SYNTHETIC` line, `gt-csv` has a last `synthetic` column (`yes` or `no`), and `gt-logcheck` ends its summary with `synthetic=N`. The label never changes a judgment: flagged records are still analyzed, shown and checked. Not yet checked on the Pi (see `HARDWARE-CHECKS.md`).
- **Address and port:** with no flags it listens on every interface, port 8080. `--port N` and `--bind IPV4` change that.
- **Who is served:** only private IPv4 addresses (127/8, 10/8, 172.16/12, 192.168/16 and 169.254/16). Anything else is closed without an answer.
- **The store:** `--dir PATH` says where to read it (read only). The default, `/var/lib/greenthumb/store`, is where the hub is meant to write.
- **What it shows:** the newest valid soil value and light state, how old they are, and a count of the last 24 hours of readings. "Needs water" is `yes`, `no` or `unknown`: `yes` when the newest good soil value is at or below the dry threshold (`--dry N`, default 565, **not calibrated**, see below), and `no` again only once it is 48 counts above it, so noise does not flip it back and forth. It says `unknown`, never `no`, when there is no good recent reading. "Time until dry" still says "not enough data"; it needs about four weeks of real history from the hub.
- **When it says degraded:** it lists the reasons, for example no new reading for 3 minutes, a store it cannot read, invalid readings, an under-voltage event, a clock that is ahead, or low disk space. It never shows OK for a state it cannot judge.
- **Started by the supervisor:** `gt-supervisor` starts `./gt-interface` with the hub and the sensors (with no `--dir` it reads `/var/lib/greenthumb/store`, where the hub writes); to run it by hand as above, use `--dir`. Other flags (`--heartbeat-ms`, `--refresh-ms`) are for the tests.

## Analyze the history

`gt-analyze` reads a store, finds the dry-down cycles and says how long until the soil is dry. It only reads, so it is safe while a writer is running.

```
build/tools/gt-analyze --dir /var/lib/greenthumb/store
```

- **What it prints:** the records it read and how many it skipped (INVALID, STUCK, time-unset and corrupt ones are never used), each cycle with its slope, fit and light (`light_h` is bright hours per day), the newest soil value, and either `prediction hours=...` with a range, or `prediction_withheld reason=...` and why.
- **The light-adjusted estimate (#26):** a second line, `prediction_light hours=...` or `prediction_light_withheld reason=...`. It fits the drying rate of the finished dry-downs against their bright hours per day and reads the current rate off that line, so a plant in a brighter spot is predicted to dry sooner. It needs at least 5 finished dry-downs and is withheld, with the reason, when their light barely differs (`no_light_spread`), a cycle has too little valid light (`light_coverage`), the current light is outside what it has seen (`outside_range`), or the fitted rate is not falling. The soil-only line above is unchanged. Light is the bright/dark flag only, and temperature is not measured, so a light effect may really be a temperature effect.
- **Comparing the two (test C3):** `--compare` predicts each past dry-down both ways 12 hours before it reached the dry threshold, from all the other cycles, and prints `compare paired=... soil_median_err_h=... light_median_err_h=... verdict=pass|fail|insufficient`. It needs at least 5 cycles that can be scored both ways, otherwise the verdict is `insufficient`.
- **When it holds back:** `insufficient_cycles` (fewer than 3 accepted finished dry-downs), `no_recent_data` (the newest good sample is more than 30 minutes old), `not_drying` (the soil was just watered) or `not_falling`.
- **The dry threshold is a guess:** `--dry N` sets the raw soil value that counts as dry. The default, 565, is the "very dry soil" reading from our build. It is not calibrated, because the calibration test (S1) was not run, and the output says so.
- **Other options:** `--days N` is how much history to read (default 28) and `--now MS` sets the clock, for replaying old data. `--compare` is described above.
- **Not yet tried on real data:** the hub exists now, but it has not recorded a real history yet (that takes days of a plant drying out). It has only been run on synthetic data and on the fake feeder's data, where it correctly says there is not enough to predict. That includes the light-adjusted estimate and the C3 comparison: their tests use made-up cycles, so no result from them says anything about a real plant.

## Troubleshooting

- **`36` is missing from `i2cdetect -y 1`:** check that I2C is enabled in `raspi-config` and that SDA (pin 3) and SCL (pin 5) are wired the right way round. Also check 3.3 V and GND on the soil sensor.
- **The first soil read is `0xffff` (65535):** you probably ran `i2cdetect` just before. In our tests this happened on the first read after `i2cdetect` every time (4 of 4) and never otherwise (10 of 10). Read again; the next reads are normal. Do not run `i2cdetect` right before a measurement.
- **The soil reading jumps after you moved the probe:** see step 2 above. Fix the probe in place and wait about ten seconds after placing it.
- **`get_throttled` is not `0x0`:** use a proper Pi 5 power supply and a short, good cable.
- **The light output never changes:** check 3.3 V, GND and the DO wire on pin 11, then turn the blue pot. The module's DO LED should flip when the light crosses the threshold.
- **The page does not load on a phone:** the phone must be on the same network as the Pi, and the Pi only answers private addresses, so a guest or public network is refused. `greenthumb.local` worked on an iPhone; some phones cannot resolve `.local` names, so try `http://<pi-ip>:8080/` with the address from `hostname -I`. On the Pi, `pgrep -a gt-interface` shows whether it is running and `ss -ltn | grep 8080` whether it is listening.
- **`Permission denied` or a login error from `make deploy`, `scp` or `ssh`:** the message tells you which problem it is.
  - `Could not resolve hostname greenthumb.local`: your machine cannot look up `.local` names (WSL cannot). Use the Pi's IP address from `hostname -I` in `PI=`.
  - `Permission denied (publickey,password)`: the Pi was reached but the login was refused. The user or password is wrong. Use the name from `whoami` on the Pi, and test with `ssh <user>@<ip> echo ok`.
  - `Permission denied (publickey)` with no `password` in the brackets: the Pi accepts only SSH keys, so `ssh-copy-id` cannot work. Add your key by hand. On the Linux machine, print it with `cat ~/.ssh/id_ed25519.pub` (run `ssh-keygen -t ed25519` first if that file is missing). On the Pi, run `mkdir -p ~/.ssh && chmod 700 ~/.ssh`, then `echo 'PASTE_THE_LINE_HERE' >> ~/.ssh/authorized_keys`, then `chmod 600 ~/.ssh/authorized_keys`. Copy only the `.pub` file, never the private key.
  - `<user> is not in the sudoers file`: the Pi user cannot use `sudo`. Use a user that can.
- **Reporting a hardware problem:** include the output of `dmesg | tail -50`, `i2cdetect -y 1`, a raw sensor read, and the GPIO17 level. A description alone is not enough to diagnose it.

## Not built yet

These are the parts still to come. The issue numbers are on the repository's issue tracker.

- The whole device running on the Pi: the supervisor starting the hub and both sensors with the real sensors (`systemd/greenthumb.service` is written but not yet installed or tried on the Pi), then the 48-hour soak with a harness that measures the real processes (`tools/soakcheck.sh` still drives the feeder)
- Supervisor: the `events.log` file, and killing orphaned children when it starts
- Analysis: dry-down detection and time-until-dry (#27) is written and tested on synthetic data, and still has to be run on real recorded history; the light-adjusted prediction and its C3 comparison (#26) are written and tested on synthetic data, and still need at least 5 real dry-downs
- Calibrating the "needs water" state on the LAN page (#43): it is built and tested, but the dry threshold is a guess (test S1 was not run) and the check against 20 real weight or finger tests (C4) has not been done
- Sensor calibration tests and crash-safety runs (#32, #29, #30, #31)

## More reading

- [`PROBLEMMEMO.md`](PROBLEMMEMO.md): who it is for and the problem it solves
- [`DESIGN.md`](DESIGN.md): failure modes, storage format, constraints and the evaluation plan
- Code layout: `include/` holds the contracts shared between components, `src/supervisor/` the supervisor, `tests/` the tests, and `cmake/` the Pi 5 cross-compile toolchain.
