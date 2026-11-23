# Developer: here be dragons

TPMSFlipper's home screen is for reading and fixing a sensor. Everything on
this page sits behind Settings > Developer and a warning screen, because none
of it is needed for that and none of it is friendly.

It is here because the project wants its working shown. The command that
takes a sensor out of shipping mode was found with these tools.

## What is behind the menu

| Row | Folder | Sends | State |
|---|---|---|---|
| Phone link | in the main app | Bluetooth LE | On test. Off by default |
| LF Lab | `flipper/dev/lf_lab` | 125 kHz test frames | Used on two sensors, September to November 2026 |
| Status Watch | `flipper/dev/status_watch` | Nothing | Used on one car |
| Tool Recorder | `flipper/dev/tool_recorder` | Nothing | Used with one workshop tool |

Starting a tool from the menu was tried once, on 23 November 2026, with the
last development build on official firmware 1.5.1-rc: all three tools
started. It uses the firmware's own way of starting one app after another
(`loader_enqueue_launch`).

## Installing the tools

They are optional. Copy the three files from the release's `dev` folder to
`SD Card/apps_data/tpmsflipper/dev/`:

```
continental_tg1c_lf.fap     LF Lab
tpms_watch.fap              Status Watch
tpms_tool_recorder.fap      Tool Recorder
```

Kept there, they stay out of the Flipper's Apps menu. TPMSFlipper also looks
in `SD Card/apps/Sub-GHz/`, where a tool installed on its own would be. A row
whose file is missing says "Not on the SD card".

Pressing OK on a tool closes TPMSFlipper and starts the tool. Back in the
tool goes to the Flipper's menu, not back to TPMSFlipper.

## Phone link

With the row on, the app runs a Bluetooth LE service for as long as it is
open. While it runs it replaces the Flipper's own Bluetooth profile, the one
the Flipper phone app uses; closing TPMSFlipper puts that back.

- The Live monitor's readings go to a connected phone.
- A phone pairs once, with a PIN the Flipper shows. A paired phone can start
  a scan, a "Not it" rescan and the monitor, cancel, give a sensor a place
  and choose the set on the car. Each command goes through the same screen
  logic as a key press, and the Flipper's screen shows it.
- A phone cannot start exit ship mode.

No phone app is published. [PHONE-LINK.md](PHONE-LINK.md) describes the
service for anyone who wants to write one.

## LF Lab

The bench tool. It sends a 125 kHz frame as a train of bursts with the
receiver running, and logs what the sensor answers. It can send the ordinary
wake, the wake with one bit changed (a sweep of all 48), the wake diluted
among rejected frames, small groups of candidate frames, the park command,
and a CONNECT packet for the sensor chip's bootloader.

Read before using it:

- **It sends frames that are meant to be wrong.** A sensor might take one of
  them as a command nobody has seen. Use it only on a sensor you are prepared
  to lose.
- **It reaches every sensor near the coil.** Keep it away from wheels you
  care about.
- **It is tied to two sensor IDs.** The lab build knew its two sensors by ID
  and judged its results by their answers. Here those two IDs are
  placeholders (`TG1C_SPARE_ID` and `TG1C_BAD_ID` in `tg1c_seq.h`). Put your
  own sensor's ID there and rebuild, or the tool will send but will not
  recognise your sensor's answer.
- **Its notes are not here.** Comments name lab note files (`analysis/...`,
  `docs/TG1C-LF-LAB.md`). The notebook and the recordings are not published.
- **Its tests are not here** either: they replay those recordings.

## Status Watch

Receive only. It listens on 433.92 MHz, shows each sensor's status bytes and
flags a change from what it first learned. It writes a CSV log to the SD
card. It was the receiver while another tool did the sending.

## Tool Recorder

Receive only. It records the 125 kHz signal of another TPMS tool held against
the Flipper's back, as on/off timings, to the SD card. It was written to
capture a workshop tool's wake signal.

## Building

Each tool is an ordinary Flipper app:

```sh
cd flipper/dev/lf_lab && ufbt          # dist/continental_tg1c_lf.fap
cd flipper/dev/status_watch && ufbt    # dist/tpms_watch.fap
cd flipper/dev/tool_recorder && ufbt   # dist/tpms_tool_recorder.fap
```

The main app's test script checks that the main app itself can build only two
frames, the wake and exit ship mode. That check does not cover the LF Lab,
which exists to send others.
