#!/usr/bin/env python3
"""Exercise the keyboard piano against real ImGui input and focus handling."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
imgui = root.parent / 'ofxImGui/libs/imgui/src'
with tempfile.TemporaryDirectory(prefix='keyboard_piano_test_') as tmp:
    tmp = pathlib.Path(tmp)
    (tmp / 'imconfig.h').write_text('#pragma once\n')
    # Only the OF parameter value/listener adapter is stubbed; all input,
    # focus, key repeats and window handling use the installed real ImGui.
    (tmp / 'ofParameter.h').write_text('''#pragma once
#include <functional>
#include <utility>
template<class T> class ofParameter {
    T value;
public:
    std::function<void(const T&)> changed;
    ofParameter(T v) : value(std::move(v)) {}
    const T& get() const { return value; }
    void set(const T& v) { value=v; if(changed) changed(value); }
};
''')
    output = tmp / 'test'
    subprocess.run(['clang++', '-std=c++17', '-O2', '-DIMGUI_DISABLE_OSX_FUNCTIONS',
                    '-DIMGUI_DISABLE_WIN32_FUNCTIONS', '-I'+str(tmp), '-I'+str(imgui),
                    '-I'+str(root / 'src'), str(pathlib.Path(__file__).with_name('test.cpp')),
                    *[str(imgui / name) for name in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp')],
                    '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)
