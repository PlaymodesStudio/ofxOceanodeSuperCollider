#!/usr/bin/env python3
"""Run the production Box note/automation contracts for the shared voice family."""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[2]
for folder,cls in [('fmBox','scFMBox'),('aliasingBox','scAliasingBox'),('steamPipeBox','scSteamPipeBox'),('physicalBox','scPhysicalBox')]:
    env=dict(os.environ,BOX_OSC_SOURCE=str(root/f'src/nodes/boxSynths/{folder}/{cls}.cpp'),BOX_OSC_CLASS=cls)
    if folder in ('steamPipeBox','physicalBox'):env['BOX_OSC_PULSE']='1'
    print(folder,flush=True)
    subprocess.run([sys.executable,str(root/'tools/fmbox/osc_test.py')],env=env,check=True)
