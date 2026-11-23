# TG1C status codes

What the status byte of a Continental TG1C sensor frame has been seen to mean,
for people reading a log and for apps that show a sensor's state.

Everything here was observed on one car, a Jaguar XF (X260), and one loose
spare sensor, between September and November 2026. It is a reading of
measurements, not a specification. The visual version is
`tg1c-status-codes.html` (published as the artifact "TG1C Status Codes").

## The frame

Nine bytes, sent at 433.92 MHz:

| Byte | Field | How to read it | How sure |
|---|---|---|---|
| 0-3 | Sensor ID | As printed, e.g. `0F3A6CB8` | Matches the ID stored in the car |
| 4 | Status (B4) | See below | Observed |
| 5 | Pressure (B5) | x 1.3725 kPa, gauge | The car's own readings are whole multiples of 1.3725 |
| 6 | Temperature (B6) | minus 50 °C | Agrees with the car to 1 °C |
| 7 | B7 | `01` or `02` in a wake reply; changes every frame while driving | Not decoded |
| 8 | Checksum | XOR of bytes 0-7 | Holds for every frame logged |

Example: `0F 3A 6C B8 86 A7 33 01 F2` is sensor `0F3A6CB8`, status `86`,
229 kPa, 1 °C.

## The codes

The first digit of the status byte says what kind of frame it is and what state
the sensor is in.

| Status | Name | What it is | Seen on |
|---|---|---|---|
| `87`, `86` | **Wake reply, parked** | A working sensor at rest answering a tool's wake. It listens about every 31 s. | Three sensors on the car, the spare, and the rear left after the fix |
| `66` | **Driving** | Sent by the sensor on its own while the wheel turns. B7 changes every frame. | Three sensors during one drive, 1 to 7 °C |
| `F7`, `F6` | **Wake reply, shipping mode** | A sensor still in its storage state answering a wake. It answers within a second but never transmits on its own, so the car reports "no signal". | The rear left sensor before the fix |
| `77`, `76`, `75` | **Command answer** | The block a sensor sends after the exit-shipping command: five short groups 1.4 s apart, about 6 s. | The spare (`77`) and the rear left (`75` then `76`) |
| `A6` | **Pressure changing** | Sent on its own, every ~5 s, while the tyre is being inflated or let down: one frame each time the pressure has moved about 4 counts (5.5 kPa, 0.8 psi). Each frame is sent twice. | All four wheels and the spare tyre, inflated and deflated at 4 to 7 °C (13 November) |
| `E6` | **Pressure settled** | Sent on its own about 30 s after the pressure stopped changing, with the final value; one to three of them, about 30 s apart. | The same session |
| `B7`, `74` | Not placed | Seen only in someone else's Abarth recordings: `B7` on four sensors being inflated, `74` in one frame at 2 °C. | Third-party recordings |

The second digit is not the state. It was 7 at 19 to 23 °C and 6 at 1 to 12 °C
on every sensor, and 5 in the first group of one command answer. `67` while
driving in warm weather is expected and has not been seen.

### For an app

Classify by the high digit of B4, and show the low digit only as part of the
raw byte:

| `B4 >> 4` | Show | Note |
|---|---|---|
| `8` | Parked (wake reply) | The normal resting state |
| `6` | Driving | Only heard without a wake |
| `F` | Shipping mode (wake reply) | The state the exit-shipping command fixes |
| `7` | Answering a command | Short-lived; not a state to report as a fault |
| `A` | Pressure changing | The sensor's own alarm for a moving pressure: worth showing |
| `E` | Pressure settled | After `A`, once the pressure is steady |
| anything else | Unknown | Show the byte |

- Classifying by the top two bits alone puts driving frames (`66`) in the same
  group as command answers (`77`). Use the whole high digit.
- The battery rules read from a workshop tool's firmware apply to wake replies.
  Applied to a driving frame they would call the battery low, because bit 7 is
  clear in every frame a sensor sends on its own. Do not show a battery reading
  for a driving frame.
- Pressure is B5 x 1.3725. The x 1.38 used in LF Lab up to v0.3.6 reads half a
  percent high.

## The bits

| Status | Bit 7 | Bits 6-4 | Bits 3-2 | Bits 1-0 |
|---|---|---|---|---|
| `87` / `86` parked | 1 | 000 | 01 | 11 / 10 |
| `66` driving | 0 | 110 | 01 | 10 |
| `F7` / `F6` shipping | 1 | 111 | 01 | 11 / 10 |
| `77` / `76` / `75` command answer | 0 | 111 | 01 | 11 / 10 / 01 |

- **Bit 7** is set in every direct reply to a wake and clear in every frame a
  sensor sent on its own timing. An observation across these rows.
- **Bits 6-4** changed together, from `111` to `000`, when the exit-shipping
  command worked. Which of the three is the shipping flag is not known.
- **Bits 3-2** have been `01` in every frame.
- **Bits 1-0** follow temperature or something that moves with it.

## How each state behaves

| State | Listens for a tool | Transmits on its own | The car |
|---|---|---|---|
| Shipping | About once a second | Never seen | "No signal" fault, no pressure shown |
| Parked | About every 31 s | Nothing in 45 minutes from the loose spare | Shows the last pressure |
| Driving | Not tested | Yes, status `66` | Shows live pressure |
| Pressure changing | Not tested | Yes, `A6` every ~5 s while it changes, then `E6` | Not checked |

## Where each fact comes from

The project's lab notes, which are not published.
[FINDINGS.md](FINDINGS.md) has the summary.

- Shipping to parked, and the command answer: sessions on the bench and at
  the wheel, 12 November 2026.
- Driving frames, the car's readings and the pressure scale: one drive, the
  same evening.
- Warm and cold wake replies: bench and wheel sessions, September to November
  2026. The Abarth values come from someone else's published recordings.
- Pressure changing and settled (`A6`, `E6`): 77 frames logged while the
  owner's tyres were being inflated, 13 November 2026.
- The command itself and the battery rules: read from a workshop tool's
  firmware.

## Not known

- The maker's names for these states. "Parked", "Driving" and "Shipping" are
  ours; the frame that fixed the sensor comes from a tool's "disable shipping
  mode" function.
- What B7 carries while driving.
- What the low digit means, and how to read battery state for this sensor type.
- Whether other cars' TG1C sensors use the same codes. The Abarth values
  suggest not entirely.
