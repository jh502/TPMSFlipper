# TPMSFlipper 1.0.0

Built for official Flipper Zero firmware 1.5.1-rc, target 7, API 88.2. Copy
`tpmsflipper.fap` to `SD Card/apps/Sub-GHz/`, then on the Flipper: Apps,
Sub-GHz, TPMSFlipper. On another firmware version the Flipper may refuse to
load it; build it from the source in that case.

The first public build. What it does and how to use it is in the README at the
top of the repository.

**`dev/` is optional.** It holds the three lab tools that Settings > Developer
starts. To use them, copy the three files to
`SD Card/apps_data/tpmsflipper/dev/`. They do not show in the Apps menu.

| File | What it is | Version | Bytes |
|---|---|---|---|
| `tpmsflipper.fap` | The app | 1.0.0 | 69692 |
| `dev/continental_tg1c_lf.fap` | LF Lab: sends 125 kHz test frames | 0.3.6 | 58948 |
| `dev/tpms_watch.fap` | Status Watch: receive only | 0.2.1 | 15528 |
| `dev/tpms_tool_recorder.fap` | Tool Recorder: receive only | 0.1.1 | 10676 |

The app transmits on 125 kHz: the wake frame in a scan, and the
exit-ship-mode frame after you confirm it. Bluetooth runs only with
Settings > Developer > Phone link on; it is off by default.

What has run: the host tests of the app, and the build of all four files.
This build has not yet run on a Flipper. The last development build, the same
code under its working name, has: its Developer menu started all three lab
tools (23 November 2026). It has not scanned a sensor yet.

`SHA256SUMS` lists the checksums. Licence: GPL-2.0-or-later. `LICENSE` and
`MIT-flipper-zero-tpms.txt` are the two licence texts.
