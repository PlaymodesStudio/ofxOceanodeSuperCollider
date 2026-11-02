#!/bin/sh
# Builds and runs the HarmonyBox engine tests (-v prints sample progressions).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
of="$here/../../../.."
out="${TMPDIR:-/tmp}/harmonybox_test"
clang++ -std=c++17 -O1 -Wall -Wno-unused-parameter -I"$of/libs/json/include" "$here/test.cpp" -o "$out"
"$out" "$@"
