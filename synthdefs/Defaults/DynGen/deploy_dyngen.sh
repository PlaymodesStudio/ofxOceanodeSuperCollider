#!/bin/bash
# deploy_dyngen.sh — Copy newly compiled DynGen SynthDefs to the Oceanode data folder.
#
# Run this AFTER evaluating dyngen.scd in SuperCollider.
# SuperCollider compiles the SynthDefs into ./CompiledSynthdefs/dyngen/
# This script copies them to the app data folder used by the running app.

SRC="$(dirname "$0")/CompiledSynthdefs/dyngen"
DST="/Users/santiagovilanova/PM Dropbox/PLAYMODES_STUDIO/2026/SILO/SOFT/SHARED_DATA/data/Supercollider/Synthdefs/Defaults/dyngen"
DST_OCEANODE_SCROLLER="/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/apps/Playmodes/OceanodeScroller/bin/data/Supercollider/Synthdefs/Defaults/dyngen"

if [ ! -d "$SRC" ]; then
    echo "ERROR: Source directory not found: $SRC"
    echo "Please evaluate dyngen.scd in SuperCollider first."
    exit 1
fi

COUNT=$(ls "$SRC"/*.scsyndef 2>/dev/null | wc -l)
if [ "$COUNT" -eq 0 ]; then
    echo "ERROR: No .scsyndef files found in $SRC"
    echo "Please evaluate dyngen.scd in SuperCollider first."
    exit 1
fi

mkdir -p "$DST"
mkdir -p "$DST_OCEANODE_SCROLLER"
cp "$SRC"/*.scsyndef "$DST/"
cp "$SRC"/*.scsyndef "$DST_OCEANODE_SCROLLER/"
echo "Deployed $COUNT SynthDefs to: $DST"
echo "Deployed $COUNT SynthDefs to: $DST_OCEANODE_SCROLLER"
echo "Done. Restart the app or send /d_loadDir in SuperCollider to reload."
