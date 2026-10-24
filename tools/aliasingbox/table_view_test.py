#!/usr/bin/env python3
"""Exercise the real table renderer with ImGui, without opening a window."""
import pathlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[2]
imgui=root.parent/'ofxImGui/libs/imgui/src'
with tempfile.TemporaryDirectory(prefix='aliasing_table_gui_') as d:
 config=pathlib.Path(d);(config/'imconfig.h').write_text('#pragma once\n')
 command=['clang++','-std=c++17','-O2','-DIMGUI_DISABLE_OSX_FUNCTIONS','-DIMGUI_DISABLE_WIN32_FUNCTIONS',
  '-I'+d,'-I'+str(imgui),'-I'+str(root/'src'),str(pathlib.Path(__file__).with_suffix('.cpp'))]
 command += [str(imgui/name)for name in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp')]
 subprocess.run(command+['-o',str(config/'test')],check=True)
 subprocess.run([str(config/'test')],check=True,cwd=d)
