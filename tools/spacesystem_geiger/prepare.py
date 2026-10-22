#!/usr/bin/env python3
"""Build the preset and its exact SynthDefs without needing an audio device.

    python3 prepare.py <output directory>

Then render with render_preset.py <output>/data/Presets/DEBUG/22--SPACESYSTEM_GEIGER
<output>/defs <output>/preview.wav 60 48000
"""
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
work = pathlib.Path(sys.argv[1]).resolve()
work.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(HERE / 'make_preset.py'), str(work / 'data')], check=True)
paths = [ROOT / p for p in ('sclang/SCClassLibrary', 'sclang/Extensions', 'synthdefs/Oceanode', 'ugens/OceanodeAnalog')]
conf = work / 'sclang_conf.yaml'
conf.write_text('includePaths:\n' + ''.join(f'    - "{p}"\n' for p in paths) + 'excludeDefaultPaths: true\n')
# Keep sclang's archive inside the build directory as well.
home = work / 'home'; home.mkdir(exist_ok=True)
import os
env = {**os.environ, 'HOME': str(home)}
sources = {}
for path in (ROOT / 'synthdefs').rglob('*.scd'):
    if 'Compatibility' in path.parts or 'WIP' in path.parts: continue
    match = re.search(r'~synthCreator.value\("([^"]+)"', path.read_text(errors='replace'))
    if match:
        sources[match[1]] = path
builds = set()
for p in (work / 'data/Presets').rglob('SC_*.json'):
    if p.stem.startswith('SC_Output'): continue
    name = re.sub(r'^SC_|\*_\d+$', '', p.stem)
    v = json.loads(p.read_text())
    builds.add((name, int(v['N_Chan']), (int(v['NumSpeakers']),) if 'NumSpeakers' in v else ()))
for name, n, variables in sorted(builds):
    r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(conf),
                        str(ROOT / 'synthdefs/Oceanode/compileExact.scd'), str(work / 'defs'),
                        str(sources[name]), str(n), *map(str, variables)], env=env,
                       capture_output=True, text=True, timeout=120)
    expected = work / 'defs' / name / (name + str(n) + ''.join('_' + str(v) for v in variables) + '.scsyndef')
    if r.returncode or not expected.exists() or 'COMPILE_EXACT_OK' not in r.stdout:
        raise RuntimeError(r.stdout + r.stderr)
    print(expected.name, flush=True)
tap = work / 'tap.scd'
tap.write_text('SynthDef(\\tap, { Out.ar(0, In.ar(\\from.kr(0), 4)) }).writeDefFile("' + str(work) + '"); 0.exit;\n')
r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(conf), str(tap)], env=env, capture_output=True, text=True, timeout=60)
assert (work / 'tap.scsyndef').exists(), r.stdout + r.stderr
print('Prepared', len(builds), 'SynthDef variants')

# Discovery metadata and common variants for the five new library nodes.
new_sources = [('ColorComb', '1,4,24'), ('OnePole', '1,4'), ('ReaktorFilter4', '1,4'),
               ('RandomMetro', '1,4'), ('TransportPulse', '1'), ('ReaktorSaturator', '1,4'), ('CrossfadeAB', '1,4')]
(work / 'metadata').mkdir(exist_ok=True)
r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(conf),
                    str(ROOT / 'synthdefs/Oceanode/syncCompile.scd'), 'stereo', str(work / 'metadata'),
                    *[str(sources[name]) + '::' + counts for name, counts in new_sources]],
                   env=env, capture_output=True, text=True, timeout=120)
assert r.returncode == 0 and r.stdout.count('OCEANODE_SYNC_OK') == len(new_sources), r.stdout + r.stderr
