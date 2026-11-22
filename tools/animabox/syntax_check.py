#!/usr/bin/env python3
"""Syntax-check addon .cpp files with OceanAudio's exact Xcode flags (from the newest DerivedData
build log; see the memory note on single-file checks). Uses scSteamPipeBox.cpp's command line.
  syntax_check.py <file.cpp>..."""
import sys, gzip, glob, os, re, shlex, subprocess, pathlib
APP = pathlib.Path(__file__).resolve().parents[4] / 'apps/Playmodes/OceanAudio'
logs = sorted(glob.glob(os.path.expanduser('~/Library/Developer/Xcode/DerivedData/OceanAudio-*/Logs/Build/*.xcactivitylog')), key=os.path.getmtime)
cmd = None
for log in reversed(logs):
    text = gzip.open(log).read().decode('latin-1')
    for part in text.split('\r'):
        if 'cc.sh -x c++' in part and 'scSteamPipeBox.cpp' in part:
            cmd = part[part.index('cc.sh'):]; break
    if cmd: break
if not cmd: sys.exit('no compile command for scSteamPipeBox.cpp in the build logs')
args = shlex.split(cmd)[1:]
out, skip = [], 0
two = {'-MT', '-MF', '--serialize-diagnostics', '-index-store-path', '-index-unit-output-path', '-o'}
for a in args:
    if skip: skip -= 1; continue
    if a in two: skip = 1; continue
    if a in ('-c', '-MMD'): continue
    out.append(a)
src = [a for a in out if a.endswith('scSteamPipeBox.cpp')][0]
clang = '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
rc = 0
for f in sys.argv[1:]:
    full = [clang] + [str(pathlib.Path(f).resolve()) if a == src else a for a in out] + ['-fsyntax-only']
    r = subprocess.run(full, cwd=APP, capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines() if 'error' in l]
    print(f, 'OK' if r.returncode == 0 else 'FAILED')
    if r.returncode: print('\n'.join(r.stderr.splitlines()[:80])); rc = 1
sys.exit(rc)
