#!/bin/bash
# Builds OceanodePhysical.scx (PhysicalVoice), universal (arm64 + x86_64).
#   ugens/OceanodePhysical/build.sh [path to the SuperCollider source]
# Install: copy OceanodePhysical.scx and OceanodePhysical.sc into a folder of the
# SuperCollider extensions (and of the app's data/Supercollider/Extensions).
set -e
cd "$(dirname "$0")"
SC="${1:-$HOME/Documents/SC/supercollider}"
clang++ -std=c++17 -O3 -ffast-math -fno-finite-math-only -bundle -undefined dynamic_lookup \
    -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
    -I"$SC/include/plugin_interface" -I"$SC/include/common" -I"$SC/common" \
    OceanodePhysical.cpp -o OceanodePhysical.scx
# ad-hoc signature: macOS kills scsynth loading an unsigned / stale plugin (exit 137)
codesign -f -s - OceanodePhysical.scx
echo "built $(pwd)/OceanodePhysical.scx"
