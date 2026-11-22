#!/usr/bin/env python3
"""Write the OceanAudio demo preset: ChordBox (a two-chord progression, 4 bars each, like the
Init sound's two harmony layers) -> AnimaBox Scale; AnimaBox (the Init sound, Drone on) -> SC Output.
  make_oceanaudio_preset.py <OceanAudio bin/data/Presets/<bank>/<NN--name>> [chordbox json to copy]"""
import sys, json, pathlib
HERE = pathlib.Path(__file__).resolve().parent
NUM_SOURCES, NUM_TARGETS = 23, 3
def main():
    out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    src = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
    init = json.load(open(HERE / 'reaktor/factory.json'))[0]
    sound = dict(init['values'])
    m = [0.0] * (NUM_SOURCES * NUM_TARGETS)
    for s, t, a in init['matrix']: m[s * NUM_TARGETS + t] = a
    sound['animaModMatrix'] = m
    # a ChordBox chord (4 notes) climbs faster than the Init scale (5): a lower base keeps the top band in range
    sound['Octave'] = 3.0; sound['Bands'] = 30
    anima = {'Show': '1', 'Bands': str(sound['Bands']), 'Channels': '2', 'Gate': [0.0], 'Pitch': [60.0],
             'Scale': sound['Scale'], 'Master': '1', 'animaBoxDrone': True, 'expanded': True,
             'animaBox': sound, 'animaBoxPublished': [], 'animaBoxTimeline': []}
    chord = json.load(open(src)) if src else None
    if chord:
        chord['Show'] = '0'; chord['ChordBoxPublished'] = []; chord['ChordBoxTimeline'] = []
        st = chord.get('state', {})
        for e in st.get('entries', []): e['beatDuration'] = 16.0      # 4 bars, as the Init sound's layers
    files = {'SC_animaBox_1.json': anima, 'SC_Output_1.json': {'Chan': '0', 'Delay_Ms': '0', 'Server': '0', 'expanded': True}}
    if chord: files['SC_ChordBox_1.json'] = chord
    for name, data in files.items(): (out / name).write_text(json.dumps(data, indent=4))
    modules = {'SC animaBox': {'01': [360.0, 220.0]}, 'SC Output': {'01': [700.0, 260.0]}}
    if chord: modules['SC ChordBox'] = {'01': [60.0, 200.0]}
    (out / 'modules.json').write_text(json.dumps(modules, indent=4))
    conns = {'SC_animaBox_1': {'Out': {'SC_Output_1': {'In': None}}}}
    if chord: conns['SC_ChordBox_1'] = {'Output1': {'SC_animaBox_1': {'Scale': None}}}
    (out / 'connections.json').write_text(json.dumps(conns, indent=4))
    (out / '.oceanode-node-files.json').write_text(json.dumps({'localMacros': [], 'nodes': sorted(files), 'protectedNodes': [], 'version': 1}, indent=4))
    for name, data in {'comments.json': {'NumComments': 0}, 'custom_guis.json': {'panels': []}, 'midi.json': {},
                       'transport.json': {'bpm': 120.0}, 'scope_config.json': {'parameters': [], 'version': 2},
                       'preset_layouts.json': {'active': '', 'default': '', 'layouts': {}, 'version': 2}}.items():
        (out / name).write_text(json.dumps(data, indent=4))
    print('wrote', out, sorted(p.name for p in out.iterdir()))
if __name__ == '__main__': main()
