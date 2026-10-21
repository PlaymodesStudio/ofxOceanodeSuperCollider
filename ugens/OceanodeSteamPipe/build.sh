#!/bin/bash
# Builds OceanodeSteamPipe.scx (SteamPipeVoice, SteamPipeOriginal), universal (arm64 + x86_64).
#   ugens/OceanodeSteamPipe/build.sh [path to the SuperCollider source]
# Install: copy OceanodeSteamPipe.scx and OceanodeSteamPipe.sc into a folder of the
# SuperCollider extensions (and of the app's data/Supercollider/Extensions).
set -e
cd "$(dirname "$0")"
python3 ../../tools/steampipebox/generate_ugen.py
SC="${1:-$HOME/Documents/SC/supercollider}"
clang++ -std=c++17 -O3 -ffp-contract=off -bundle -undefined dynamic_lookup \
    -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
    -I"$SC/include/plugin_interface" -I"$SC/include/common" -I"$SC/common" \
    OceanodeSteamPipe.cpp -o OceanodeSteamPipe.scx
# ad-hoc signature: macOS kills scsynth loading an unsigned / stale plugin (exit 137)
codesign -f -s - OceanodeSteamPipe.scx
echo "built $(pwd)/OceanodeSteamPipe.scx"
