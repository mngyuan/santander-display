#!/usr/bin/env bash
# Build and run the desktop preview. Needs g++ and Python 3 with Pillow.
# Run tools/fetch_driver.sh first so EPD.cpp / EPDfont.h exist.
set -euo pipefail
cd "$(dirname "$0")"
SK=../../firmware/dock_display
mkdir -p out build
g++ -std=c++17 -O1 -I. -I"$SK" -o build/preview \
  preview.cpp "$SK/render.cpp" "$SK/gfx.cpp" "$SK/EPD.cpp"
./build/preview
python3 - <<'PY'
import glob, os
from PIL import Image
for pbm in sorted(glob.glob("out/*.pbm")):
    img = Image.open(pbm).convert("L")
    base = pbm[:-4]
    img.save(base + ".png")                                         # native 250x122
    img.resize((img.width * 3, img.height * 3), Image.NEAREST).save(base + "@3x.png")
    os.remove(pbm)
print("PNGs in tools/preview/out/")
PY
