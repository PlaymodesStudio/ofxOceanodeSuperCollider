#!/bin/sh
# Builds and runs the ArpeggiatorBox engine tests (no openFrameworks needed).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/arpeggiatorbox_test"
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter "$here/test.cpp" -o "$out"
"$out"
