#!/bin/sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp "${TMPDIR:-/tmp}/audio_routing_test.XXXXXX")"
trap 'rm -f "$out"' EXIT
clang++ -std=c++17 -Wall -Wextra -pedantic -I"$here/../../src" "$here/test.cpp" -o "$out"
"$out"
