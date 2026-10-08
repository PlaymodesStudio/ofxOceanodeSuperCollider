#!/bin/bash
# build_sclang.sh — rebuild the headless sclang bundled in sclang/osx/.
#
#   ./build_sclang.sh <supercollider source checkout> [work dir]
#
# Builds a universal (arm64 + x86_64), Qt-free sclang that only links macOS
# system frameworks, and refreshes sclang/SCClassLibrary from the same source
# (minus the Qt GUI classes), so binary and class library always match.
# The checkout is not modified: the source is exported to the work dir.
#
# The current binary was built from SuperCollider 4f6ba9951 (3.14.0-dev) and
# writes SynthDefs byte-identical to SuperCollider.app 3.14.0.
set -euo pipefail

SRC="${1:?usage: $0 <supercollider checkout> [work dir]}"
WORK="${2:-$(mktemp -d)}"
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$WORK/src" "$WORK/build"
git -C "$SRC" archive HEAD | tar -x -C "$WORK/src"
for m in external_libraries/nova-simd external_libraries/nova-tt external_libraries/yaml-cpp; do
    rm -rf "$WORK/src/$m"
    cp -R "$SRC/$m" "$WORK/src/$m"
done

# The bundled Boost has a typo that newer Clang rejects (fixed upstream).
sed -i '' 's/that_=x\.that;/that_=x.that_;/' "$WORK/src/external_libraries/boost/boost/thread/future.hpp"

cd "$WORK/build"
# Without Qt nothing pulls in Foundation, which the macOS filesystem code needs.
cmake "$WORK/src" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DCMAKE_EXE_LINKER_FLAGS="-framework Foundation -framework CoreServices -framework CoreFoundation" \
    -DSC_QT=OFF -DSC_IDE=OFF -DSUPERNOVA=OFF -DSC_ABLETON_LINK=OFF -DSC_HIDAPI=OFF \
    -DNO_LIBSNDFILE=ON -DSCLANG_SERVER=OFF -DNO_AVAHI=ON \
    -DENABLE_TESTSUITE=OFF -DINSTALL_HELP=OFF -DSC_EL=OFF -DSC_VIM=OFF -DSC_ED=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_Readline=ON \
    -DREADLINE_INCLUDE_DIR=READLINE-NOTFOUND -DREADLINE_LIBRARY=READLINE-NOTFOUND
cmake --build . --target sclang -j "$(sysctl -n hw.ncpu)"

mkdir -p "$HERE/osx"
cp lang/sclang "$HERE/osx/sclang"
rm -rf "$HERE/SCClassLibrary"
rsync -a --exclude scide_scqt "$WORK/src/SCClassLibrary" "$HERE/"
echo "Built $(lipo -info "$HERE/osx/sclang")"
