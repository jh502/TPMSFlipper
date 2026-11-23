# Changelog

## 1.0.0

The first public build. Development builds were numbered 0.1.0 to 0.5.0.

- Scan wheel: wake one sensor at the valve and read pressure, temperature, ID
  and state. "Not it" rescans past a neighbouring sensor.
- Exit ship mode: read, send the command after a confirm, check the result.
- Live monitor: the four wheels of the set on the car, receive only.
- My wheels: two sets, the spare tyre and four loose sensors, each with its
  last reading.
- Settings: units, the set on the car, the radio band (433 or 315 MHz), and
  the list of cars expected to carry this sensor.
- Settings > Developer, behind a warning: the phone link (Bluetooth LE, off
  by default) and three lab tools that the app starts as separate apps: LF
  Lab, Status Watch and Tool Recorder.

What has run: the host tests of the main app, and the build of all four
apps. This build has not yet run on a Flipper. The last development build,
the same code under its working name, has: on 23 November 2026 its Developer
menu started all three lab tools. It has not scanned a sensor yet. Earlier
builds of the main app did, see the README.
