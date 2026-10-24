#!/usr/bin/env python3
"""Exercise production audio policy evaluation without the OF app/audio device."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='snapshot-morph-') as work:
    output=Path(work)/'test'
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
        '-I'+str(here/'stubs'),'-I'+str(root/'src/nodes'),
        '-I'+str(root.parents[1]/'libs/json/include'),
        str(here/'test.cpp'),'-o',str(output)],check=True)
    subprocess.run([str(output)],check=True)
