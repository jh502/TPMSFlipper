#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host tests for TPMSFlipper (flipper/tpmsflipper).
#   sh tests/run-tests.sh             engine, settings file, log, screens, keys
#   sh tests/run-tests.sh --screens   also write the screenshots the docs use
#                                     into docs/img/
# Needs python3 and a C compiler. The screens are drawn with the Flipper's own
# fonts, read from the uFBT SDK's firmware image ($UFBT_HOME, default ~/.ufbt);
# without the SDK that part is skipped.
set -eu
cd "$(dirname "$0")/.."
build=$(mktemp -d /tmp/tpmsflipper.XXXXXX)
app=flipper/tpmsflipper
CC="cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I$app"

# Generated files are what their sources generate.
python3 $app/art/gen_icons.py --check
python3 $app/cars/gen_cars.py --check

# The exit-ship-mode frame can only leave through one path: wt_engine_ship,
# which the app starts only from the confirm screen.
[ "$(grep -c 'TG1C_PAYLOAD_E' $app/wt_engine.h)" = 1 ] || { echo "FAIL: the exit-ship-mode frame is built in more than one place"; exit 1; }
[ "$(grep -c 'wt_engine_ship(' $app/tpmsflipper.c)" = 1 ] || { echo "FAIL: wt_engine_ship has more than one caller"; exit 1; }
if grep -n 'TG1C_PAYLOAD_[BCD]\|tg1c_build_wake_variant\|tg1c_scan_bytes\|tg1c_connect_packet' $app/wt_*.h $app/tpmsflipper.c; then
  echo "FAIL: the app must send only the wake and the exit-ship-mode frame"; exit 1
fi
echo "PASS: the app builds two frames only, the wake and exit-ship-mode, the second from one call site"

# The main app starts the lab tools; it does not contain them.
if grep -n '#include "tg1c_seq\|#include "tg1c_probe\|#include "watch_' $app/*.c $app/*.h; then
  echo "FAIL: the main app must not include the lab tools' code"; exit 1
fi

# Engine, settings file and log on a simulated clock, receiver, coil and sensor.
$CC tests/tpmsflipper_engine.c -o "$build/engine"
"$build/engine"

# Screens and keys.
elf="${UFBT_HOME:-$HOME/.ufbt}/current/firmware.elf"
if [ -f "$elf" ]; then
  python3 tests/support/extract_fonts.py "$elf" "$build/wt_fonts_gen.h"
  $CC -Itests/support -I"$build" tests/tpmsflipper_ui.c -o "$build/ui"
  mkdir "$build/screens"
  "$build/ui" "$build/screens"
  if [ "${1:-}" = "--screens" ]; then
    shots="home scan-running scan-result scan-detail ship-confirm ship-sending ship-done ship-nochange monitor monitor-list assign settings scan-result-ship ship-intro ship-checking scan-fail scan-result-drive monitor-extra where spare-pick wheels wheels-ids wheels-winter wheels-spares settings-3 scan-result-wheel scan-new-running scan-new-fail cars car-1-0 settings-band-315 dragons developer"
    files=""
    for n in $shots; do files="$files $build/screens/$n.pbm"; done
    python3 tests/support/pbm2png.py docs/img $files
    sheet=""
    for n in home scan-running scan-result-wheel scan-result-ship where assign ship-confirm ship-done monitor wheels wheels-spares settings-3; do
      sheet="$sheet $build/screens/$n.pbm"
    done
    python3 tests/support/pbm2png.py --scale 2 --sheet docs/img/overview.png 3 $sheet
    echo "wrote docs/img/*.png ($(echo $shots | wc -w | tr -d ' ') screens and overview.png)"
  fi
else
  echo "SKIP: screens and keys need the Flipper's fonts from the uFBT SDK ($elf not found)"
fi
