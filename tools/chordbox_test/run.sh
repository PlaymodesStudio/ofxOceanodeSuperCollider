#!/bin/sh
# Builds and runs the ChordBox engine tests against OceanAudio's Pitchclass data.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
of="$here/../../../.."
out="${TMPDIR:-/tmp}/chordbox_test"
clang++ -std=c++17 -O1 -Wall -Wno-unused-parameter -I"$of/libs/json/include" "$here/test.cpp" -o "$out"
"$out" "$of/apps/Playmodes/OceanAudio/bin/data/Supercollider/Pitchclass/" "$@"
