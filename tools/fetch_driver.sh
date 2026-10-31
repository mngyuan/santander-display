#!/usr/bin/env bash
# Copies Elecrow's 2.13" e-paper driver files into the sketch folder.
# They're Elecrow's code, so they're fetched rather than committed here.
#
# The board ships with one of two panel controllers:
#   jd79661 (default)  newer boards (Elecrow's "arduino-v1.2" example)
#   ssd1680            older boards (Elecrow's "2.13_partial_refresh" example)
# Usage: tools/fetch_driver.sh [jd79661|ssd1680]
# Symptom of the wrong one: serial log stops at "epd: full" and the screen never changes.
set -euo pipefail
PANEL="${1:-jd79661}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$HERE/firmware/dock_display"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone --depth 1 https://github.com/Elecrow-RD/CrowPanel-ESP32-2.13-E-paper-HMI-Display-with-122-250.git "$TMP/crow"
OLD="$TMP/crow/example/arduino/Example Code/2.13_partial_refresh"
NEW="$TMP/crow/example/arduino-v1.2/main"

# Drawing code, fonts and SPI are the same for both panels. EPD.cpp comes from
# the old example because the v1.2 copy mirrors text itself; epd_io.cpp mirrors
# the whole frame instead, so every primitive stays the right way round.
for f in EPD.h EPD.cpp EPDfont.h spi.h spi.cpp; do
  cp "$OLD/$f" "$DEST/$f"
done
case "$PANEL" in
  jd79661) INIT="$NEW" ;;
  ssd1680) INIT="$OLD" ;;
  *) echo "unknown panel '$PANEL' (use jd79661 or ssd1680)" >&2; exit 1 ;;
esac
cp "$INIT/EPD_Init.h" "$INIT/EPD_Init.cpp" "$DEST/"
echo "Driver files ($PANEL) copied to $DEST"
