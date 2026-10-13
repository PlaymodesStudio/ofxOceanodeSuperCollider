#!/bin/bash
# Builds OceanodeSpectral.scx (PV_MaskFilter), universal (arm64 + x86_64).
#   ugens/OceanodeSpectral/build.sh [path to the SuperCollider source]
# Install: copy OceanodeSpectral.scx and PV_MaskFilter.sc into a folder of the
# SuperCollider extensions (and of the app's data/Supercollider/Extensions).
set -e
cd "$(dirname "$0")"
SC="${1:-$HOME/Documents/SC/supercollider}"
clang++ -std=c++17 -O3 -ffast-math -bundle -undefined dynamic_lookup \
    -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
    -I"$SC/include/plugin_interface" -I"$SC/include/common" -I"$SC/common" \
    PV_MaskFilter.cpp -o OceanodeSpectral.scx
echo "built $(pwd)/OceanodeSpectral.scx"
