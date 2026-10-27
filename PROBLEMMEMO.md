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
E: Multi-Process Architecture
- Runs unattended for days on end, which a single sensor failure or driver failure should not result in failing the whole system.
- We isolate each sensor with a "supervisor" process
    - The supervisor resets the sensor whenever it fails.

## The risk
Sensors miscalibrating or corrosion. Corrosion is common for cheap soil moisture sensors. 