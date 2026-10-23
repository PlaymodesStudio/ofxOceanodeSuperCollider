#!/usr/bin/env python3
"""Render live-control cases: source switches, manual LFO fallback and piano chords."""
import json
import math
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'tools/aliasingbox'))
from render_test import read_wav

work = pathlib.Path(sys.argv[1]).resolve()
preset = work / 'data/Presets/GENERATIVE_MUSIC/196--SPACESYSTEM_GEIGER'

def read(folder, name):
    return json.loads((folder / (name + '.json')).read_text())

def write(folder, name, value):
    (folder / (name + '.json')).write_text(json.dumps(value, indent=2) + '\n')

# Every GUI binding must point to a saved, exposed parameter.
gui = read(preset, 'custom_guis')['panels'][0]
keys = [w for w in gui['layout']['widgets'] if w['type'] == 'PianoKeyboard']
assert len(keys) == 2
for widget in gui['layout']['widgets']:
    path = widget['parameterRef']['parameterPath']
    if path:
        node, parameter = path.split('/')
        assert parameter in read(preset, node), path
for folder in [preset, *(preset / ('Macro_' + str(i)) for i in range(1, 6))]:
    modules = read(folder, 'modules')
    assert not any('ChannelMapper' in k or 'ChannelRouter' in k for k in modules)
    assert all('Arithmetic' not in k for k in modules) if folder != preset else True
for i in range(1, 4):
    params = read(preset, 'Macro_' + str(i))
    assert all(k in params for k in ('On', 'Level', 'Chance', 'Range_ms', 'Min_ms',
                                     'Curve', 'Cut_Range', 'Cut_Min', 'Res_Range', 'Res_Min', 'Gate'))

for case in ('sources_off', 'no_notes', 'triad_manual', 'clocked'):
    target = work / 'control_cases' / case
    if target.exists(): shutil.rmtree(target)
    shutil.copytree(preset, target)
    shutil.copy2(work / 'tap.scsyndef', target / 'tap.scsyndef')
    if case == 'sources_off':
        for i in range(1, 4):
            name = 'Macro_' + str(i)
            params = read(target, name)
            params['On'] = [0.]
            write(target, name, params)
    elif case == 'clocked':
        for i in range(1, 4):
            name = 'Macro_' + str(i)
            params = read(target, name)
            params.update(Quantize_to_Clock=[1.], Chance=[1.])
            write(target, name, params)
    else:
        # Remove the external audio cables exactly as a player would, then
        # use the numeric macro inputs to set wet-only, dry reverb output.
        connections = read(target, 'connections')
        for source in connections.values():
            for output in source.values():
                for node, params in list(output.items()):
                    if node in ('Macro_4', 'Macro_5'):
                        params.pop('Level', None)
                        params.pop('DryWet', None)
                        if not params: del output[node]
        write(target, 'connections', connections)
        for i in (4, 5):
            name = 'Macro_' + str(i)
            params = read(target, name)
            params.update(Notes=[] if case == 'no_notes' else [60., 64., 67.],
                          DryWet=[1.], Level=[-10.], Mix=[0.])
            write(target, name, params)
    wav = target / 'test.wav'
    result = subprocess.run([sys.executable, str(HERE / 'render_preset.py'), str(target),
                    str(work / 'defs'), str(wav), '6', '48000', '120' if case == 'clocked' else '0'], check=True,
                   capture_output=True, text=True)
    audio, channels, sr = read_wav(wav)
    assert channels == 4 and all(math.isfinite(v) for v in audio)
    peak = max(map(abs, audio))
    if case in ('triad_manual', 'clocked'): assert peak > 1e-5, (case, peak)
    else: assert peak < 1e-8, (case, peak)
    print(case, 'OK, peak', peak, flush=True)
print('GUI bindings, source controls, channel routing and piano selection passed.')
