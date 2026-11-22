#!/bin/bash
# Builds OceanodeFlute.scx (FluteVoice), universal (arm64 + x86_64).
#   ugens/OceanodeFlute/build.sh [path to the SuperCollider source]
# Install: copy OceanodeFlute.scx and OceanodeFlute.sc into a folder of the
# SuperCollider extensions (and of the app's data/Supercollider/Extensions).
# No fast-math: test mode follows SC's own float arithmetic.
set -e
cd "$(dirname "$0")"
SC="${1:-$HOME/Documents/SC/supercollider}"
clang++ -std=c++17 -O3 -ffp-contract=off -bundle -undefined dynamic_lookup \
    -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
    -I"$SC/include/plugin_interface" -I"$SC/include/common" -I"$SC/common" \
    OceanodeFlute.cpp -o OceanodeFlute.scx
# ad-hoc signature: macOS kills scsynth loading an unsigned / stale plugin (exit 137)
codesign -f -s - OceanodeFlute.scx
echo "built $(pwd)/OceanodeFlute.scx"
