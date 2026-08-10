#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
APP_DIR="$SCRIPT_DIR/dist/Reaktor Patch Extractor.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
RESOURCES_DIR="$CONTENTS_DIR/Resources"
CACHE_DIR="/tmp/reaktor-patch-extractor-module-cache"
ARM_BINARY="$CACHE_DIR/ReaktorPatchExtractor-arm64"
X86_BINARY="$CACHE_DIR/ReaktorPatchExtractor-x86_64"

mkdir -p "$MACOS_DIR"
mkdir -p "$RESOURCES_DIR"
mkdir -p "$CACHE_DIR"
CLANG_MODULE_CACHE_PATH="$CACHE_DIR" SWIFT_MODULECACHE_PATH="$CACHE_DIR" swiftc -O \
    -target arm64-apple-macosx13.0 \
    -framework AppKit \
    -framework UniformTypeIdentifiers \
    -lz \
    "$SCRIPT_DIR/CoreCellParser.swift" \
    "$SCRIPT_DIR/ReaktorPatchExtractor.swift" \
    -o "$ARM_BINARY"
CLANG_MODULE_CACHE_PATH="$CACHE_DIR" SWIFT_MODULECACHE_PATH="$CACHE_DIR" swiftc -O \
    -target x86_64-apple-macosx13.0 \
    -framework AppKit \
    -framework UniformTypeIdentifiers \
    -lz \
    "$SCRIPT_DIR/CoreCellParser.swift" \
    "$SCRIPT_DIR/ReaktorPatchExtractor.swift" \
    -o "$X86_BINARY"
lipo -create "$ARM_BINARY" "$X86_BINARY" -output "$MACOS_DIR/ReaktorPatchExtractor"
cp "$SCRIPT_DIR/Info.plist" "$CONTENTS_DIR/Info.plist"
cp "$SCRIPT_DIR/module_catalog.json" "$RESOURCES_DIR/module_catalog.json"
cp "$SCRIPT_DIR/core_module_catalog.json" "$RESOURCES_DIR/core_module_catalog.json"
codesign --force --deep --sign - "$APP_DIR"

print "Built: $APP_DIR"
