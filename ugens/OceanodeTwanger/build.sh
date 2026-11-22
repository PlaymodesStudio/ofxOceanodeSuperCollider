#!/bin/bash
# Builds OceanodeTwanger.scx (TwangerVoice), universal (arm64 + x86_64).
#   ugens/OceanodeTwanger/build.sh [path to the SuperCollider source]
# Install: copy OceanodeTwanger.scx and OceanodeTwanger.sc into a folder of the
# SuperCollider extensions (and of the app's data/Supercollider/Extensions).
# Primary arithmetic as in OceanodeSteamPipe: no fast-math.
set -e
cd "$(dirname "$0")"
SC="${1:-$HOME/Documents/SC/supercollider}"
clang++ -std=c++17 -O3 -ffp-contract=off -bundle -undefined dynamic_lookup \
    -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
    -I"$SC/include/plugin_interface" -I"$SC/include/common" -I"$SC/common" \
    OceanodeTwanger.cpp -o OceanodeTwanger.scx
# ad-hoc signature: macOS kills scsynth loading an unsigned / stale plugin (exit 137)
codesign -f -s - OceanodeTwanger.scx
echo "built $(pwd)/OceanodeTwanger.scx"
