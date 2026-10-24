#!/bin/sh
# Builds and runs the MetroBox engine tests (no openFrameworks needed).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/metrobox_test"
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    "$here/test.cpp" "$here/../../src/nodes/boxSynths/metroBox/scMetroBoxEngine.cpp" -o "$out"
"$out"
