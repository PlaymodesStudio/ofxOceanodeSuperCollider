#!/bin/bash
set -e
cd "$(dirname "$0")"
SC="${1:-$HOME/Documents/SC/supercollider}"
clang++ -std=c++17 -O3 -ffp-contract=off -bundle -undefined dynamic_lookup \
 -arch arm64 -arch x86_64 -mmacosx-version-min=10.15 \
 -I"$SC/include/plugin_interface" -I"$SC/include/common" -I"$SC/common" \
 OceanodePrimitives.cpp -o OceanodePrimitives.scx
codesign -f -s - OceanodePrimitives.scx
