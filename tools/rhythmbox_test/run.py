#!/usr/bin/env python3
"""Exercise production embedding and RhythmBox graph/anchor methods with IO doubles."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    return source[start:end] + '\n'

with tempfile.TemporaryDirectory(prefix='rhythmbox-test-') as directory:
    out = Path(directory)
    (out / 'ofMain.h').write_text('''#pragma once
#include <filesystem>
#include <iostream>
#include <string>
inline std::string ofToDataPath(const std::string& s, bool) { return std::filesystem::absolute(s).string(); }
inline std::ostream& ofLogWarning(const std::string&) { return std::cerr; }
''')
    (out / 'ofxOceanodeShared.h').write_text('''#pragma once
#include <string>
extern std::string presetPath;
struct ofxOceanodeShared { static std::string getCurrentPresetPath() { return presetPath; } };
''')
    (out / 'ofxSCServer.h').write_text('''#pragma once
#include <cstdint>
struct ofxSCServer {
 static uint64_t timetagForSteadyTimeUs(uint64_t t){return t;}
 struct ScopedTimetag{ScopedTimetag(uint64_t){}};
 struct ScopedNRTOffset{ScopedNRTOffset(double){}};
};
''')
    source = (ROOT / 'src/nodes/scRhythmBox.cpp').read_text()
    signatures = ['void scRhythmBox::' + name for name in
                  ['buildSynth(', 'createSynth(', 'free(', 'setOutputBus(', 'sendAnchor(']]
    signatures += ['int scRhythmBox::getBufnum(', 'int scRhythmBox::getOutputBusIndex(',
                   'std::vector<ofxSCServer*> scRhythmBox::activeGraphServers(']
    (out / 'production_methods.h').write_text('\n'.join(method(source, s) for s in signatures))
    preset_signatures = ['static int inferRhythmBoxTrackCount(', 'void scRhythmBox::loadBeforeConnections(', 'void scRhythmBox::presetHasLoaded(']
    (out / 'preset_methods.h').write_text('\n'.join(method(source, s) for s in preset_signatures))
    for name in ['embedding', 'graph_sync', 'presets']:
        executable = out / name
        subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        '-I', str(out), '-I', str(ROOT / 'src'),
                        '-I', str(ROOT.parent / 'ofxOceanode/src/Managers'),
                        '-I', str(ROOT.parent.parent / 'libs/json/include'),
                        str(HERE / (name + '.cpp')), '-o', str(executable)], check=True)
        subprocess.run([str(executable), str(out / 'assets')], check=True)
