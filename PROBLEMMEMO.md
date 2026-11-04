# Problem memo -- <problemSolvers> (<Duke Bautista>, <Andrew Faga>)

## The user
This device will sit next a houseplant owner who either has not the time to know when to water the plants or has killed plants from inconsistent watering.

## The problem
Checking moisture within the soil can be unreliable. Plants get overwatered, underwatered. Gets rid of plant anxiety.

## Why a device
We can automate of sensing the soil moisture. It can be physically sit next to the pot which is always monitering the soil.

## The sensors
Two sensors:
- Adafruit STEMMA capacitive soil moisture sensor (I2C)
- LM393 light module with an LDR (digital output: bright or dark)
These sensors are correlated with each other. Soil readings will determine if water is needed while the light sensor
tells us whether the plant is in bright or dark conditions, which is another factor in how soon it needs water.

## The mechanisms
D: Custom Storage Layer
- Uses history of moisture and light over weeks.
- Why we need history: how soon the soil will be dry can only be estimated from
  a full dry-down (watering until dry), which takes days. How much the light
  changes that rate needs several of these cycles. The latest reading alone
  cannot tell the owner that.
- Why our own log: the owner will not babysit the device, and a home power cut
  can stop the Pi mid-write on its SD card. The record log must survive that
  without the owner doing anything. Each record is a fixed 24 bytes with its own
  checksum, appended and synced to disk, so after a crash we know exactly which
  records are good and lose at most the one being written. A plain text or CSV
  file can end in a half-written line and cannot tell damaged data from good.
E: Multi-Process Architecture
- The device runs unattended for days. The two sensors fail in different ways
  (the soil sensor can return garbage or hang the I2C bus, the light module's
  input can float), and one failing must not stop the other sensor or the log.
- Each sensor runs in its own process, one hub process is the only writer to
  the store, and a supervisor process restarts whatever dies (with backoff, and
  a DEGRADED state if one keeps crashing). systemd restarts the supervisor
  itself.
- A restart loses only the readings taken while the process was down, and the
  gap shows in the sequence numbers.

## The risk
Sensors miscalibrating or corrosion. Corrosion is common for cheap soil moisture sensors. 