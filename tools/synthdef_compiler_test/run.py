#!/usr/bin/env python3
"""Test the real on-demand C++ compiler against a simulated post-sync data folder.
Only OF event/logging and server OSC transport are stubbed; builds use bundled sclang.
"""
import pathlib
import shutil
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
imgui = root.parent / 'ofxImGui/libs/imgui/src'
with tempfile.TemporaryDirectory(prefix='synthdef_compiler_test_') as folder:
    tmp = pathlib.Path(folder)
    for name in ('scSynthdefCompiler.cpp', 'scSynthdefCompiler.h'):
        shutil.copy(root / 'src' / name, tmp / name)
    (tmp / 'ofMain.h').write_text(r'''#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
inline std::string testData;
inline std::string ofToDataPath(const std::string& p, bool) { return (std::filesystem::path(testData)/p).string(); }
template<class T> std::string ofToString(T v){ return std::to_string(v); }
inline std::string ofTrim(const std::string& s){ auto a=s.find_first_not_of(" \n\r\t"); return a==std::string::npos ? "" : s.substr(a,s.find_last_not_of(" \n\r\t")-a+1); }
inline std::ostream& ofLogError(const char*){ return std::cerr; }
inline std::ostream& ofLogNotice(const char*){ return std::cout; }
inline float ofGetElapsedTimef(){ return 1; }
inline uint64_t ofGetFrameNum(){ return 1; }
struct ofEventArgs {};
struct ofEventListener {};
struct TestEvent { template<class T, class U> ofEventListener newListener(T*, U){ return {}; } };
struct TestEvents { TestEvent update; };
inline TestEvents& ofEvents(){ static TestEvents e; return e; }
''')
    (tmp / 'serverManager.h').write_text(r'''#pragma once
#include <string>
class serverManager {
public:
    std::string loaded;
    bool areRequiredSynthdefsLoading() const { return false; }
    void loadSynthdefFile(const std::string& file){ loaded=file; }
};
''')
    (tmp / 'imconfig.h').write_text('#pragma once\n')
    exe = tmp / 'test'
    subprocess.run(['clang++', '-std=c++17', '-O0', '-DIMGUI_DISABLE_OSX_FUNCTIONS',
                    '-DIMGUI_DISABLE_WIN32_FUNCTIONS', '-I'+str(tmp), '-I'+str(imgui),
                    str(pathlib.Path(__file__).with_name('test.cpp')), str(tmp/'scSynthdefCompiler.cpp'),
                    *[str(imgui/name) for name in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(tmp/'data'), str(root)], check=True)
