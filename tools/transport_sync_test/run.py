#!/usr/bin/env python3
"""Compile the production transport follower against the real transport types."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='transport-sync-test-') as directory:
    out = pathlib.Path(directory)
    (out / 'ofxSCServer.h').write_text(r'''
#pragma once
#include <cstdint>
struct ofxSCServer {
    inline static uint64_t calls=0, tag=0;
    inline static double offset=0;
    // Deliberate conversion jitter: replacements must reuse the original tag.
    static uint64_t timetagForSteadyTimeUs(uint64_t t){return t+ ++calls;}
    struct ScopedTimetag {uint64_t old;ScopedTimetag(uint64_t t):old(tag){tag=t;}~ScopedTimetag(){tag=old;}};
    struct ScopedNRTOffset {double old;ScopedNRTOffset(double t):old(offset){offset=t;}~ScopedNRTOffset(){offset=old;}};
};
''')
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-I', str(out), '-I', str(root / 'src'), '-I', str(root.parent / 'ofxOceanode/src/Managers'), str(pathlib.Path(__file__).with_name('test.cpp')), '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
