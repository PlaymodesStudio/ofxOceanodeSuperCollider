#!/bin/bash
# Builds OceanodeWood.scx (WoodPipe), universal (arm64 + x86_64).
#   ugens/OceanodeWood/build.sh [path to the SuperCollider source]
# Install: copy OceanodeWood.scx and OceanodeWood.sc into a folder of the
# SuperCollider extensions (and of the app's data/Supercollider/Extensions).
# Float arithmetic follows the SteamPipe voice it is taken from: no fast-math.
set -e
cd "$(dirname "$0")"
SC="${1:-$HOME/Documents/SC/supercollider}"
clang++ -std=c++17 -O3 -ffp-contract=off -bundle -undefined dynamic_lookup \
    -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
    -I"$SC/include/plugin_interface" -I"$SC/include/common" -I"$SC/common" \
    OceanodeWood.cpp -o OceanodeWood.scx
# ad-hoc signature: macOS kills scsynth loading an unsigned / stale plugin (exit 137)
codesign -f -s - OceanodeWood.scx
echo "built $(pwd)/OceanodeWood.scx"
