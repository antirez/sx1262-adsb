#!/bin/sh
# Arduino copies sketches into its build tree. Stage the shared C sources so
# the T-Echo and the T3-S3 always compile the same decoder and radio program.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/build-techo"
sketch="$out/techo"
mkdir -p "$sketch" "$out/arduino-user"
export ARDUINO_DIRECTORIES_USER="$out/arduino-user"
libs=${LILYGO_T_ECHO:-"$out/LilyGO-T-Echo"}
if [ ! -f "$libs/lib/Adafruit-GFX-Library/Adafruit_GFX.h" ]; then
    git clone https://github.com/Xinyuan-LilyGO/T-Echo.git "$libs"
    git -C "$libs" checkout 48bef560d2590dbd1420e51ef6a7f588b80777a3
fi
cp "$root"/techo/*.c "$root"/techo/*.cpp "$root"/techo/*.h "$root"/techo/*.ino "$sketch/"
for name in radio.c radio.h radio_bus.h sampler.c decoder.c receiver.h selftest.h observation.c history.c history.h tuning.c tuning.h cpr.c cpr.h; do
    cp "$root/main/$name" "$sketch/"
done
cp -R "$root/main/demod" "$root/main/vendor" "$sketch/"
(cd "$sketch" && cmake -P "$root/models/assets.cmake")
arduino-cli compile --fqbn adafruit:nrf52:pca10056 --libraries "$libs/lib" \
    --build-path "$out/obj" --build-property "compiler.c.extra_flags=-DMAX_PENDING=12 -I\"$sketch\"" "$sketch"
python3 "$root/techo/uf2.py" "$out/obj/techo.ino.hex" "$out/techo-adsb.uf2"
