#!/usr/bin/env python3
"""AnimaMundi2_tips.ens snapshots -> AnimaBox factory sounds.

Reads the decoded snapshot values (reaktor/pitch/snapvals.json: V[control id][snapshot]) and the
Harmony tables, maps every control onto AnimaBox's (spec_*.md in reaktor/), writes
src/nodes/boxSynths/animaBox/scAnimaBoxData.h (the factory bank) and reaktor/factory.json.
The scale stored in each sound is the ensemble's FIRST harmony layer (the user's choice: AnimaBox
takes its scale from an outer node; the other layers are listed in the json for reference).

  python3 convert_snapshots.py
"""
import json, math, re, pathlib
HERE = pathlib.Path(__file__).resolve().parent
R = HERE / 'reaktor'
ROOT = HERE.parents[1]
J = json.load(open(R / 'pitch/snapvals.json')); V = J['V']; NAMES = J['names'][:19]

def g(i, s): return float(V[str(i)][s])
def on(i, s): return 1.0 if g(i, s) > 0 else 0.0

# ── Harmony tables (Chords / Scales Event Tables, their lists) ───────────────
rows = {}
for f, tag in (('chords_table.txt', 'C'), ('scales_table.txt', 'S')):
    for l in (R / 'pitch' / f).read_text().splitlines()[1:]:
        p = l.split(); rows[(tag, int(p[0]))] = [float(x) for x in p[1:]]
lists = {}
for f, tag in (('chords_list.txt', 'C'), ('scales_list.txt', 'S')):
    for l in (R / 'pitch' / f).read_text().splitlines()[1:]:
        m = re.match(r'\s*(\d+) (.*?)\s+([\d.]+) id=(\d+)', l)
        if m: lists[(tag, int(m.group(4)))] = (m.group(2).strip(), int(float(m.group(3))))
def csize(v): return 2 if v == 1 else 3 if v <= 14 else 4 if v <= 41 else 5 if v <= 82 else 6 if v <= 108 else 7
def ssize(v): return 5 if v <= 18 else 6 if v <= 28 else 7 if v <= 60 else 8 if v <= 63 else 12
USER = [[0, 3, 7, 8], [0, 3, 7, 8, 10], [0, 3, 7, 8, 10], [0, 3, 7, 8, 10]]
LY = [dict(chord=10966, scale=11076, ov=11222, user=11183, mode=11188, root=11193, oct=11197, ovQ=11226, cat=11231, mic=11238),
      dict(chord=11250, scale=11360, ov=11506, user=11467, mode=11472, root=11477, oct=11481, ovQ=11510, cat=11515, mic=11522),
      dict(chord=11534, scale=11644, ov=11790, user=11751, mode=11756, root=11761, oct=11765, ovQ=11794, cat=11799, mic=11806),
      dict(chord=11818, scale=11928, ov=12074, user=12035, mode=12040, root=12045, oct=12049, ovQ=12078, cat=12083, mic=12090)]
CAT = {1: 'Micro', 2: 'Overtones', 3: 'User', 4: 'Scales', 5: 'Chords'}
def layer(li, s):
    L = LY[li]; cat = CAT[int(g(L['cat'], s))]
    d = dict(cat=cat, root=int(round(g(L['root'], s))) % 12, oct=int(round(g(L['oct'], s))), mode=int(round(g(L['mode'], s))))
    if cat == 'Chords':
        name, v = lists[('C', int(g(L['chord'], s)))]; d['scale'] = rows[('C', min(v, 114))][:csize(v)]; d['name'] = name
    elif cat == 'Scales':
        name, v = lists[('S', int(g(L['scale'], s)))]; d['scale'] = rows[('S', min(v, 64))][:ssize(v)]; d['name'] = name
    elif cat == 'User':
        n = int(round(g(L['user'], s))); d['scale'] = USER[li][:n]; d['name'] = 'User'
    elif cat == 'Micro':
        N = max(1, int(round(g(L['mic'], s)))); d['scale'] = [round(k * 12.0 / N, 5) for k in range(N)]; d['name'] = '%d-EDO' % N
    else:
        d['overtones'] = int(g(L['ov'], s)) - 1; d['round'] = int(round(g(L['ovQ'], s))); d['name'] = 'Overtones'
    return d
BEATS = {6: 4.0, 1: 8.0, 15: 16.0, 26: 0.125}     # the clock's 'beat' list: 1 bar, 2 bars, 4 bars, 1/32

# ── The spatial LFOs (spec_lfo.md) -> the shared LFOs ─────────────────────────
LFO = {
    'BE': dict(len=7439, q=7452, scale=7454, bpm=7465, div=7467, smooth=7480, frn=7488, ph=7507, mode=7571, skew=7525,
               round=7530, pwm=7561, pow=7609, vibA=7595, vibF=7598, inv=7400, mod=7402, sym=7404, rnd=7410, iq=7417),
    'Q': dict(len=7719, q=7732, scale=7734, bpm=7745, div=7747, smooth=7760, frn=7768, ph=7787, mode=7851, skew=7805,
              round=7810, pwm=7841, pow=7889, vibA=7875, vibF=7878, inv=7680, mod=7682, sym=7684, rnd=7690, iq=7697)}
def lfo(prefix, ids, s, bands):
    x = {k: g(i, s) for k, i in ids.items()}
    L = x['len']; spread = L + 1 if L <= 0 else float(math.floor(15 * L + 1 + 0.5))
    mode = int(round(x['mode']))                           # 1 det, 2 random, 3 brownian, 4 monobrownian
    beats = max(0.125, x['div'] / max(0.01, x['bpm']))      # F = bps x BPM* / Div -> Beats per cycle
    phase = x['ph']
    if mode == 2:                                           # random: two new values per cycle -> twice the
        beats = max(0.125, beats / 2); spread *= 2; phase *= 2   # rate, offsets and phase in its cycles
    # the indexer centres voice i at (i + 0.5) / n of Spread, Reaktor's band v at v / n of Len:
    # half a band more phase. And its Invert 0 counts down, Reaktor's button 0 counts up.
    phase = (phase + 0.5 * spread / max(1, bands)) % 1.0
    # Reaktor's Slow Random (the rate wobble): control-rate (400 Hz) random +-1 through a one-pole at
    # Vibf: its swing is 0.577 sqrt(k / (2 - k)) of VibA, k = 1 - exp(-2 pi Vibf / 400); LFNoise2's ~0.47
    k = 1 - math.exp(-2 * math.pi * max(0.0001, x['vibF']) / 400.0)
    wander = x['vibA'] * 0.577 * math.sqrt(k / (2 - k)) / 0.47
    out = {
        prefix + 'Sync': True, prefix + 'Beats': [round(beats, 5)], prefix + 'Spread': round(spread, 5),
        prefix + 'Invert': 0.0 if x['inv'] > 0 else 1.0, prefix + 'Symmetry': int(round(x['sym'])),
        prefix + 'Random': round(min(1.0, x['rnd'] / max(1, bands)), 5),
        prefix + 'Quantize': 128 if x['iq'] <= 0 else max(1, int(round(bands / x['iq']))),
        prefix + 'Modulo': 128 if x['mod'] >= 120.5 else max(1, int(round(x['mod']))),
        prefix + 'Phase': round(phase, 5),
        prefix + 'Noise': False,
        prefix + 'Random_Type': 0 if mode == 1 else (1 if mode == 2 else 2),
        prefix + 'Random_Same': mode == 4,
        prefix + 'Skew': [round(-x['skew'], 5)], prefix + 'Round': [round(min(2.0, x['round']) / 2, 5)],
        prefix + 'Width': [round(0.5 - x['pwm'], 5)], prefix + 'Centered': True,
        prefix + 'Pow': [round(x['pow'], 5)], prefix + 'Gain': [1.0],
        prefix + 'Steps': 0 if x['q'] <= 0 else int(round(1 / x['q'])) + 1,
        prefix + 'Smooth': round(x['smooth'] / 1000.0, 5) if x['smooth'] > 0.01 else 0.0,
        prefix + 'Detune': [round(x['frn'], 5)],
        prefix + 'Wander': round(wander, 5), prefix + 'Wander_Rate': round(max(0.0001, x['vibF']), 5),
    }
    return out, x['scale']

def sound(s):
    B = int(round(g(5909, s)))
    l1 = layer(0, s)
    v = {'Bands': B, 'Key': g(10883, s), 'Mode': float(l1['mode']), 'Root': float(l1['root']), 'Octave': float(l1['oct']),
         'Scale_Input': 0.0}
    if l1['cat'] == 'Overtones':
        v.update({'Tuning': 1.0, 'Overtones': float(l1['overtones']), 'Overtone_Round': float(l1['round']), 'Scale': [0.0]})
    else:
        v.update({'Tuning': 0.0, 'Scale': [float(x) for x in l1['scale']]})
    v.update({
        'Transpose': g(10850, s), 'Fine_Transpose': g(10851, s),
        'Random_Transpose': g(10833, s), 'Random_Step': g(10849, s), 'Random_Abs': on(10855, s),
        'Random_Every': BEATS.get(int(g(10818, s)), 16.0),
        'Octave_Random': g(12100, s), 'Octave_Range': g(12107, s), 'Octave_Up_Only': on(12125, s),
        'Step': g(12297, s), 'Index_Cycles': g(12301, s), 'Index_Offset': g(12307, s), 'Repeat': g(12311, s),
        'Comb': g(12328, s), 'Index_Invert': on(12342, s), 'Symmetry': max(1.0, g(12351, s)), 'Index_Random': g(12365, s),
        'Index_Pow': g(12393, s), 'Index_Quantize': g(12381, s), 'Index_Skew': g(12414, s), 'Index_Round': g(12419, s),
        'Index_Edge': g(12454, s),
        'Band_Octave': g(5490, s), 'Semitone': g(5514, s), 'Fine': g(5463, s), 'Detune': g(5466, s), 'Detune_Extra': g(5481, s),
        'Vibrato': g(5497, s), 'Vibrato_Rate': g(5500, s), 'Vibrato_Poly': g(5508, s), 'Glide': g(12132, s),
        'Energy': 0.0, 'Reso': g(15607, s), 'Resonance_Fine': g(12817, s), 'Stereo_Detune': g(8049, s), 'Gain': g(8058, s),
        'Pan_Width': g(12726, s),
        'Energy_Damp': 1.0 if int(round(g(12815, s))) == 1 else 0.0, 'Damp_Curve': g(12788, s), 'Damp_Invert': on(12810, s),
        'Pluck': 1.0 if int(round(g(7641, s))) == 1 else 0.0, 'Pluck_Threshold': g(7634, s), 'Pluck_Attack': g(7637, s),
        'Pluck_Decay': g(7638, s), 'Pluck_Sustain': g(7639, s), 'Pluck_Release': g(7640, s),
        'Post_Type': float(int(round(g(15500, s))) - 1), 'Post_Cutoff': g(15585, s), 'Post_Reson': g(15587, s),
        'Post_Gain_Correction': g(15267, s),
    })
    be, beScale = lfo('LFO1_', LFO['BE'], s, B)
    q, qScale = lfo('LFO2_', LFO['Q'], s, B)
    v.update(be); v.update(q)
    v['animaModLfos'] = 2
    matrix = [[0, 0, round(beScale, 5)], [1, 1, round(qScale * g(15609, s) / 100.0, 5)]]
    layers = [layer(i, s) for i in range(4)]
    length = int(round(g(10701, s)))
    return {'name': NAMES[s], 'values': v, 'matrix': matrix,
            'reaktor': {'layers': layers[:length], 'layerBeats': BEATS.get(int(g(10818, s)), 16.0)}}

def main():
    snaps = [sound(s) for s in range(19)]
    (R / 'factory.json').write_text(json.dumps(snaps, indent=1))
    data = json.dumps([{k: x[k] for k in ('name', 'values', 'matrix')} for x in snaps], separators=(',', ':'))
    out = ROOT / 'src/nodes/boxSynths/animaBox/scAnimaBoxData.h'
    out.write_text('// Generated by tools/animabox/convert_snapshots.py from AnimaMundi2_tips.ens: do not edit.\n'
                   '#pragma once\nnamespace animaBoxData {\n'
                   'static const char* factory = R"animajson(' + data + ')animajson";\n}\n')
    for x in snaps:
        l = x['reaktor']['layers'][0]
        print('%-24s bands %3d  %-12s %-28s root %2d oct %d' % (x['name'], x['values']['Bands'], l['cat'], l.get('name', ''), l['root'], l['oct']))
if __name__ == '__main__': main()
