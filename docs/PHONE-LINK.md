# The phone link

With Settings > Developer > Phone link on, TPMSFlipper sends what it hears to a
phone or computer over Bluetooth LE, and a paired phone can start what the
keys start. This page describes the link for anyone writing an app that
uses it. The reference is `flipper/tpmsflipper/wt_link.h` and `wt_remote.h`.

**Status.** The link is still on test, which is why it sits behind the
Developer menu and is off by default. Development builds have run it on a
Flipper with a Mac and with an iPhone test app. No phone app is published.
Version numbers below 1.0.0 on this page are those of the development
builds.

## Turning it on

On the Flipper: Settings, Developer, **Phone link: on** (before 0.5.0 the row
was in Settings itself). It is off by default.

While the monitor runs with the link on, TPMSFlipper's service *replaces*
the Flipper's own Bluetooth profile, the one qFlipper mobile and the Flipper
app use. Leaving the monitor puts the Flipper's own profile back. If
Bluetooth is switched off in the Flipper's own settings, the link cannot
start; the monitor shows the Bluetooth mark struck through and carries on
receiving.

On the monitor screen, under the car:

| Mark | Meaning |
|---|---|
| none | Phone link is off in Settings |
| ᛒ blinking | Advertising, no phone connected |
| ᛒ steady | A phone is connected |
| ᛒ struck through | Bluetooth could not start, or is off on the Flipper |

The 433.92 MHz side does not change: the monitor only receives on 433.92 MHz.
Bluetooth is a separate radio inside the Flipper's main chip.

## Connecting

| | |
|---|---|
| Advertised name | `TPMS` |
| Service UUID (advertised) | `9D6A2D74-91CD-4828-A353-85A41CA80437` |
| Readings characteristic | `9D6A2D74-91CD-4828-A353-85A41CA80438`, read + notify, 20 bytes |
| Pairing | none, no bonding |
| Address | the Flipper's own, with the third byte + 3 |

Scan for the service UUID, connect, subscribe to the readings characteristic.
The service UUID is in the advertising packet itself, so an iPhone app can
find the Flipper while it is in the background
(`scanForPeripherals(withServices:)` with the `bluetooth-central` background
mode).

The Flipper uses a different address for this service. That way a phone that
has paired with the Flipper's own profile does not mix the two up.

## What is sent, and when

Every value of the characteristic is one **20-byte record**, little-endian.
20 bytes fit the default ATT payload, so the phone needs no MTU negotiation.

- When a phone connects: a **hello** record, then a **sensor** record for
  every sensor in the monitor's table.
- A sensor record as soon as a new frame is heard from a sensor (within
  a quarter of a second).
- Every 10 seconds: a hello and every sensor again, with the *repeat* flag
  set. A phone that missed a notification catches up, and the age field
  keeps counting.

At most three records leave per quarter-second, so a full round of nine takes
under a second.

### Sensor record (type 0x01)

| Byte | Field |
|---|---|
| 0 | `0x01` |
| 1 | sequence number, +1 per record, wraps at 255. A gap means records were lost |
| 2–10 | the 9-byte TG1C frame as received: ID (4 bytes, big-endian as printed), B4 status, B5 pressure, B6 temperature, B7, B8 XOR of bytes 0–7 |
| 11 | signal strength, dBm, signed |
| 12 | place: 0–3 Summer FL FR RL RR, 4–7 Winter FL FR RL RR, 8 spare tyre, 9–12 spare sensors 1–4, 0xFF none |
| 13 | flags: bit 0 = a wheel of the set on the car; bit 1 = repeat (this frame was sent before) |
| 14–15 | age of the frame, seconds, saturating at 65535 |
| 16–17 | pressure, tenths of a kPa, **gauge** (B5 × 1.3725) |
| 18 | temperature, °C, signed (B6 − 50) |
| 19 | mode: 0 unknown, 1 park, 2 drive, 3 ship, 4 busy |

Pressure is gauge: a loose sensor on a bench reads about 0. The car's own
readings were whole multiples of 1.3725 kPa, and 176 counts (241.6 kPa)
showed as 35 psi on the dashboard. The mode is this project's reading of the
status byte (`docs/TG1C-STATUS-CODES.md`), an observation from one car, not a
specification.

### Hello record (type 0x02)

| Byte | Field |
|---|---|
| 0 | `0x02` |
| 1 | sequence number |
| 2 | protocol version, 1 |
| 3–5 | TPMSFlipper version, major, minor, patch |
| 6 | the set on the car: 0 Summer, 1 Winter |
| 7 | pressure unit chosen on the Flipper: 0 bar, 1 psi, 2 kPa |
| 8 | temperature unit: 0 °C, 1 °F |
| 9 | sensors in the monitor's table |
| 10–19 | zero |

An app should ignore record types it does not know, so later versions can
add some.

## Limits

- Range is that of Bluetooth LE from a Flipper: fine within a car.
- The Flipper's battery runs the 433.92 MHz receiver and Bluetooth together.
  On a long drive, power it from USB.
- Sensors send rarely while parked and about once a minute while driving, and
  from inside the car the Flipper does not hear every frame. The link
  forwards what the Flipper heard, no more.

## Protocol 2: a paired phone drives TPMSFlipper

From development build 0.4.0 the app runs the service for as long as the app is open with Phone
link on. A phone pairs once: the Flipper asks for pairing as soon as a phone
connects and shows a six-digit PIN to type on the phone. The bond is kept in
`/ext/apps_data/tpmsflipper/.bt.keys`, apart from the Flipper's own
pairings. A phone that does not pair is disconnected. Reference:
`flipper/tpmsflipper/wt_remote.h`.

| | |
|---|---|
| Commands characteristic | `9D6A2D74-91CD-4828-A353-85A41CA80439`, write with response, encrypted and authenticated |

### Commands (phone → Flipper), 20 bytes

Byte 0 the command, byte 1 a tag of the phone's choosing, echoed in the ack.

| Byte 0 | Command | Arguments |
|---|---|---|
| `0x81` | Scan the sensor at the coil | |
| `0x82` | "Not it": scan again, ignoring the sensor just read and every placed one | |
| `0x83` | Cancel what runs; back to the home screen | |
| `0x84` | Start the Live monitor | |
| `0x85` | Give a sensor a place | 2–5 ID, big-endian; 6 place (as the sensor record's byte 12), `0xFF` for none |
| `0x86` | The set on the car | 2: 0 Summer, 1 Winter |
| `0x87` | Send the thirteen places | |
| `0x88` | Exit ship mode | not in 0.4.0: answered "unsupported" |

A command does what the same keys would do, through the same code: the
Flipper's screen shows it, and Back on the Flipper still cancels.

### Records added (Flipper → phone), on the readings characteristic

| Type | Record | Bytes |
|---|---|---|
| `0x03` | Ack | 2 tag, 3 command, 4 answer: 0 done, 1 busy (something runs), 2 bad arguments, 3 unsupported |
| `0x04` | Status | 2 screen, 3 phase (0 idle, 1 scan, 2 sending, 3 settling, 4 checking, 5 monitor), 4 burst, 5 bursts, 6 flags (bit 0 LF on, bit 1 sensor heard, bit 2 a "Not it" scan), 7–8 elapsed s, 9–10 limit s, 11 signal dBm, 12 Flipper battery %, 13 sensors a "Not it" scan ignores |
| `0x05` | Scan result | 2 result (0 read, 1 no answer, 2 cancelled, 3 fault), 3–11 the frame, 12 signal dBm, 13 its place, 14–15 time to answer in 0.1 s, 16 frames, 17 a "Not it" scan |
| `0x06` | Place | 2 place (0–12), 3 count (13), 4–7 sensor ID (0 for empty), 8 has a reading, 9–11 B4 B5 B6, 12 year − 2000, 13 month, 14 day, 15 the set on the car |

The status goes when it changes, and every 2 s. The places go when a phone
connects, when asked for, and after any change. The hello (protocol 2) adds
byte 10, the Flipper's battery, and byte 11, flags (bit 0: an operation runs).
