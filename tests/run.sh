#!/bin/sh
# Tests: the SR-JV80 reader on a card built here with known contents (both data-line variants),
# the engine on it (pitch, zones, release, EFX, internal waves, filter, chorus, reverb, CPU),
# then the SDK host simulator on the native plugin with that card in roms/.
set -e
cd "$(dirname "$0")/.."
SDK=${SDK:-../mpc-plugin-builder-sdk}
mkdir -p build/tests
c++ -std=c++17 -O2 -Wall -Wextra -Isrc tests/card_test.cpp src/srjv.cpp src/engine.cpp src/fx.cpp -o build/tests/card_test
./build/tests/card_test build/tests/test-card

# the native plugin, in a plugin folder of its own with the test card in roms/
if [ -f build/native/xp-08.so ]; then
  P=build/tests/plugin
  rm -rf "$P" && mkdir -p "$P/roms"
  cp plugin.json "$P/" && cp build/native/xp-08.so "$P/"
  cp build/tests/test-card-v1.bin "$P/roms/SR-JV80-TEST.bin"
  c++ -std=c++17 -O2 -I"$SDK/framework" tests/plugin_test.cpp -ldl -lpthread -o build/tests/plugin_test
  ./build/tests/plugin_test "$P/xp-08.so"
  "$SDK/.sdk-tools/host_sim" "$P/xp-08.so" test --seconds 2 --notes 57,60,64 --wav build/test.wav | tail -3
else
  echo "(no native build: run python3 tools/build.py first for the plugin tests)"
fi

# real boards, when there are dumps in the project's or the built plugin's roms/: every patch played at C4
c++ -std=c++17 -O2 -Isrc tests/inspect.cpp src/srjv.cpp src/engine.cpp src/fx.cpp -o build/tests/inspect
for d in roms "build/Stephane Schmitz - VST - XP-08/roms"; do
  ls "$d" 2>/dev/null | grep -qiv readme || continue
  echo "== real boards in $d"
  find "$d" -maxdepth 1 -type f ! -iname '*readme*' -print0 | sort -z | xargs -0 ./build/tests/inspect | grep -v '^    '
  break
done
