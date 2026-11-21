#!/bin/sh
# Offline tests of SC Bake: the loop maths (C++) and the SynthDefs (scsynth -N).
set -e
cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/scbake_loop_test"
clang++ -std=c++17 -O2 -Wall loop_test.cpp -o "$out"
"$out"
python3 nrt_test.py
python3 precision_test.py
