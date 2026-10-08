#!/bin/bash
# deploy_synthdef_compiler.sh — copy the SynthDef sources and the bundled sclang
# into an app's data folder, so the app can compile missing channel counts.
#
#   tools/deploy_synthdef_compiler.sh <app>/bin/data
#
#   synthdefs/  -> data/Supercollider/SynthdefSources/
#   sclang/     -> data/Supercollider/Sclang/
#
# Compiled binaries are not touched. Re-run after editing any source.
set -euo pipefail

DATA="${1:?usage: $0 <app>/bin/data}"
ADDON="$(cd "$(dirname "$0")/.." && pwd)"
SC="$DATA/Supercollider"

[ -d "$SC" ] || { echo "No Supercollider folder in $DATA"; exit 1; }

rsync -a --delete --exclude 'CompiledSynthdefs/' --exclude '.DS_Store' \
    "$ADDON/synthdefs/" "$SC/SynthdefSources/"
rsync -a --delete --exclude 'build_sclang.sh' --exclude '.DS_Store' \
    "$ADDON/sclang/" "$SC/Sclang/"

echo "Sources: $(find "$SC/SynthdefSources" -name '*.scd' | wc -l | tr -d ' ') files -> $SC/SynthdefSources"
echo "Compiler: $SC/Sclang/osx/sclang"
