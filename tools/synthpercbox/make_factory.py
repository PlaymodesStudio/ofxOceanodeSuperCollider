#!/usr/bin/env python3
"""SynthPercBox factory pads from Bits_r6.ens -> src/nodes/boxSynths/synthPercBox/scSynthPercBoxData.h

  make_factory.py Bits_r6.ens bits_decoded.json r6.json

Each generator: its controls (key = the SynthDef's \\p_<key>, Reaktor snapshot id, range, step, kind),
its snapshot bank in file order = pads 1..n (the screenshots' dropdown numbers confirm the order).
Snapshot floats are normalised: value = lo + n (hi - lo), floored to the fader's step. Lists store a
1-based entry (sent 0-based). Toggles store 0.5 for on (sent 1). A control missing from a snapshot
takes the instrument's saved panel value (what it played with; Reaktor leaves it unchanged on recall),
else the previous pad's; the saved panel value (module record: f32 after a 01 00 00 00 word, before 00 00 00 3f).
Checked against the user's panel screenshots (asserts at the end).
"""
import json, math, struct, sys, bisect, pathlib

ens, decoded_path, report_path = sys.argv[1:4]
data = open(ens, 'rb').read()
dec = json.load(open(decoded_path))
rep = json.load(open(report_path))
u32 = lambda o: struct.unpack_from('<I', data, o)[0]
f32 = lambda o: struct.unpack_from('<f', data, o)[0]

# ── controls: key, sid, label, lo, hi, step, kind (k knob, l list, t toggle, e envelope part), section
#    lo/hi/step None: from the decoded module (sid 0 is dropped by the extractor: given here)
HPBPLP = 'HP|BP|LP'
GENS = [
 ('K1', 24044, 'MIDI 0-13', [
  ('bp', 0, 'Base', 0, 127, 1, 'k', 'Pitch'),
  ('p1on', 5, 'Penv1', None, None, None, 't', 'Pitch'), ('p1d', 1, 'Dec1', None, None, None, 'k', 'Pitch'), ('p1a', 2, 'Amt1', None, None, None, 'k', 'Pitch'),
  ('p2on', 6, 'Penv2', None, None, None, 't', 'Pitch'), ('p2d', 3, 'Dec2', None, None, None, 'k', 'Pitch'), ('p2a', 4, 'Amt2', None, None, None, 'k', 'Pitch'),
  ('bdec', 7, 'Decay', None, None, None, 'k', 'Body'), ('vol', 8, 'Vol', None, None, None, 'k', 'Body'), ('drive', 51, 'Drive', None, None, None, 'k', 'Body'),
  ('ftype', 43, 'Type', None, None, None, 'l:' + HPBPLP, 'Filter'), ('cut', 11, 'Cut', None, None, None, 'k', 'Filter'),
  ('res', 9, 'Reso', None, None, None, 'k', 'Filter'), ('env', 16, 'Env', None, None, None, 'k', 'Filter'),
  ('fa', 12, 'A', None, None, None, 'e', 'Filter'), ('fd', 13, 'D', None, None, None, 'e', 'Filter'),
  ('fs', 14, 'S', None, None, None, 'e', 'Filter'), ('fr', 15, 'R', None, None, None, 'e', 'Filter'),
  ('don', 42, 'On', None, None, None, 't', 'Delay'), ('dst', 18, '16ths', None, None, None, 'k', 'Delay'), ('dfine', 22, 'Fine', None, None, None, 'k', 'Delay'),
  ('dfeed', 17, 'Feed', None, None, None, 'k', 'Delay'), ('dpc', 19, 'Cut', None, None, None, 'k', 'Delay'), ('dres', 20, 'Reso', None, None, None, 'k', 'Delay'),
  ('dtype', 21, 'Type', None, None, None, 'l:' + HPBPLP, 'Delay'), ('dwet', 23, 'Wet', None, None, None, 'k', 'Delay'),
  ('level', 27, 'Level', None, None, None, 'k', 'Out'), ('pan', 26, 'Pan', None, None, None, 'k', 'Out')]),
 ('STOMPS', 2051, 'MIDI 14-27', [
  ('bf', 29, 'Base Hz', None, None, None, 'k', 'Pitch'), ('hf', 28, 'Sweep Hz', None, None, None, 'k', 'Pitch'),
  ('p1on', 32, 'Penv1', None, None, None, 't', 'Pitch'), ('p1d', 27, 'Dec1', None, None, None, 'k', 'Pitch'),
  ('p2on', 31, 'Penv2', None, None, None, 't', 'Pitch'), ('p2d', 30, 'Dec2', None, None, None, 'k', 'Pitch'),
  ('wave', 10, 'Wave', 0, 3, None, 'k', 'Osc'), ('pw', 60, 'PW', None, None, None, 'k', 'Osc'), ('drive', 45, 'Drive', None, None, None, 'k', 'Osc'),
  ('aa', 8, 'A', None, None, None, 'e', 'Amp'), ('ad', 6, 'D', None, None, None, 'e', 'Amp'), ('ar', 36, 'R', None, None, None, 'e', 'Amp'),
  ('ftype', 38, 'Type', None, None, None, 'l:' + HPBPLP, 'Filter'), ('cut', 39, 'Cut', None, None, None, 'k', 'Filter'),
  ('res', 37, 'Reso', None, None, None, 'k', 'Filter'), ('env', 44, 'Env', None, None, None, 'k', 'Filter'),
  ('fa', 40, 'A', None, None, None, 'e', 'Filter'), ('fd', 41, 'D', None, None, None, 'e', 'Filter'),
  ('fs', 42, 'S', None, None, None, 'e', 'Filter'), ('fr', 43, 'R', None, None, None, 'e', 'Filter'),
  ('don', 68, 'On', None, None, None, 't', 'Delay'), ('dst', 62, '16ths', None, None, None, 'k', 'Delay'), ('dfine', 66, 'Fine', None, None, None, 'k', 'Delay'),
  ('dfeed', 61, 'Feed', None, None, None, 'k', 'Delay'), ('dpc', 63, 'Cut', None, None, None, 'k', 'Delay'), ('dres', 64, 'Reso', None, None, None, 'k', 'Delay'),
  ('dtype', 65, 'Type', None, None, None, 'l:' + HPBPLP, 'Delay'), ('dwet', 67, 'Wet', None, None, None, 'k', 'Delay'),
  ('level', 23, 'Level', None, None, None, 'k', 'Out'), ('pan', 21, 'Pan', None, None, None, 'k', 'Out')]),
 ('NOIX', 2274, 'MIDI 28-41', [
  ('wave', 10, 'Wave', None, None, None, 'k', 'Noise'),
  ('aa', 8, 'A', None, None, None, 'e', 'Amp'), ('ad', 6, 'D', None, None, None, 'e', 'Amp'), ('ar', 36, 'R', None, None, None, 'e', 'Amp'),
  ('ftype', 13, 'Type', None, None, None, 'l:' + HPBPLP, 'Filter'), ('cut', 52, 'Cut', None, None, None, 'k', 'Filter'),
  ('res', 12, 'Reso', None, None, None, 'k', 'Filter'), ('env', 57, 'Env', None, None, None, 'k', 'Filter'),
  ('fa', 53, 'A', None, None, None, 'e', 'Filter'), ('fd', 54, 'D', None, None, None, 'e', 'Filter'),
  ('fs', 55, 'S', None, None, None, 'e', 'Filter'), ('fr', 56, 'R', None, None, None, 'e', 'Filter'),
  ('mfine', 19, 'ms', None, None, None, 'k', 'MicroDel'), ('mfeed', 14, 'Feed', None, None, None, 'k', 'MicroDel'),
  ('mpc', 16, 'Cut', None, None, None, 'k', 'MicroDel'), ('mres', 17, 'Reso', None, None, None, 'k', 'MicroDel'),
  ('mtype', 18, 'Type', None, None, None, 'l:' + HPBPLP, 'MicroDel'), ('mwet', 20, 'Wet', None, None, None, 'k', 'MicroDel'),
  ('dst', 38, '16ths', None, None, None, 'k', 'Delay'), ('dfine', 42, 'Fine', None, None, None, 'k', 'Delay'),
  ('dfeed', 37, 'Feed', None, None, None, 'k', 'Delay'), ('dpc', 39, 'Cut', None, None, None, 'k', 'Delay'), ('dres', 40, 'Reso', None, None, None, 'k', 'Delay'),
  ('dtype', 41, 'Type', None, None, None, 'l:' + HPBPLP, 'Delay'), ('dwet', 43, 'Wet', None, None, None, 'k', 'Delay'),
  ('son', 108, 'On', None, None, None, 't', 'Space'), ('sz', 58, 'Size', None, None, None, 'k', 'Space'), ('spos', 62, 'Pos', None, None, None, 'k', 'Space'),
  ('smod', 59, 'Mod', None, None, None, 'k', 'Space'), ('slo', 60, 'Lo', None, None, None, 'k', 'Space'), ('shi', 61, 'Hi', None, None, None, 'k', 'Space'),
  ('smix', 63, 'Mix', None, None, None, 'k', 'Space'),
  ('level', 23, 'Level', None, None, None, 'k', 'Out'), ('pan', 21, 'Pan', None, None, None, 'k', 'Out')]),
 ('CLIX', 1331, 'MIDI 43-56', [
  ('res', 0, 'Reso', 0, 0.99, 0.0099, 'k', 'Clix'), ('co', 1, 'Co', None, None, None, 'k', 'Clix'), ('ca', 2, 'Ca', None, None, None, 'k', 'Clix'),
  ('mode', 3, 'Mode', 0, 5, 1, 'l:HP2|BP2|LP2|HP4|BH4|BP4', 'Clix'), ('clev', 5, 'Level', None, None, None, 'k', 'Clix'),
  ('mfine', 178, 'ms', None, None, None, 'k', 'MicroDel'), ('mfeed', 174, 'Feed', None, None, None, 'k', 'MicroDel'),
  ('mpc', 175, 'Cut', None, None, None, 'k', 'MicroDel'), ('mres', 176, 'Reso', None, None, None, 'k', 'MicroDel'),
  ('mtype', 177, 'Type', None, None, None, 'l:' + HPBPLP, 'MicroDel'), ('mwet', 179, 'Wet', None, None, None, 'k', 'MicroDel'),
  ('dst', 168, '16ths', None, None, None, 'k', 'Delay'), ('dfine', 172, 'Fine', None, None, None, 'k', 'Delay'),
  ('dfeed', 167, 'Feed', None, None, None, 'k', 'Delay'), ('dpc', 169, 'Cut', None, None, None, 'k', 'Delay'), ('dres', 170, 'Reso', None, None, None, 'k', 'Delay'),
  ('dtype', 171, 'Type', None, None, None, 'l:' + HPBPLP, 'Delay'), ('dwet', 173, 'Wet', None, None, None, 'k', 'Delay'),
  ('son', 270, 'On', None, None, None, 't', 'Space'), ('sz', 264, 'Size', None, None, None, 'k', 'Space'), ('spos', 268, 'Pos', None, None, None, 'k', 'Space'),
  ('smod', 265, 'Mod', None, None, None, 'k', 'Space'), ('slo', 266, 'Lo', None, None, None, 'k', 'Space'), ('shi', 267, 'Hi', None, None, None, 'k', 'Space'),
  ('smix', 269, 'Mix', None, None, None, 'k', 'Space'),
  ('level', 138, 'Level', None, None, None, 'k', 'Out'), ('pan', 137, 'Pan', None, None, None, 'k', 'Out')]),
 ('CRAX', 4215, 'MIDI 59-72', [
  ('owave', 74, 'Wave', None, None, None, 'l:Saw|Pulse|Tri|Sine', 'Osc'), ('p', 0, 'P Hz', 0, 5000, 10, 'k', 'Osc'),
  ('plus', 2, '+ Hz', None, None, None, 'k', 'Osc'), ('pw', 1, 'PW', None, None, None, 'k', 'Osc'),
  ('nrnd', 64, 'Random', None, None, None, 'l:NoteRnd|Manual', 'Jitter'), ('rrate', 3, 'Rate', None, None, None, 'k', 'Jitter'),
  ('rnd', 4, 'Amount', None, None, None, 'k', 'Jitter'),
  ('lwave', 65, 'LFO', None, None, None, 'l:Saw|SawDn|Pulse|Tri|Sine|Rnd', 'LFO'), ('bpmr', 10, 'Rate', None, None, None, 'k', 'LFO'),
  ('width', 7, 'Width', None, None, None, 'k', 'LFO'), ('lamp', 8, 'Amp', None, None, None, 'k', 'LFO'),
  ('aa', 33, 'A', None, None, None, 'e', 'Amp'), ('ad', 34, 'D', None, None, None, 'e', 'Amp'), ('as', 104, 'S', None, None, None, 'e', 'Amp'), ('ar', 105, 'R', None, None, None, 'e', 'Amp'),
  ('ftype', 66, 'Type', None, None, None, 'l:' + HPBPLP, 'Filter'), ('cut', 39, 'Cut', None, None, None, 'k', 'Filter'),
  ('res', 40, 'Reso', None, None, None, 'k', 'Filter'), ('fenv', 106, 'Env', None, None, None, 'k', 'Filter'),
  ('fa', 91, 'A', None, None, None, 'e', 'Filter'), ('fd', 92, 'D', None, None, None, 'e', 'Filter'), ('fs', 102, 'S', None, None, None, 'e', 'Filter'), ('fr', 103, 'R', None, None, None, 'e', 'Filter'),
  ('dst', 68, '16ths', None, None, None, 'k', 'Delay'), ('dfine', 72, 'Fine', None, None, None, 'k', 'Delay'),
  ('dfeed', 67, 'Feed', None, None, None, 'k', 'Delay'), ('dpc', 69, 'Cut', None, None, None, 'k', 'Delay'), ('dres', 70, 'Reso', None, None, None, 'k', 'Delay'),
  ('dtype', 71, 'Type', None, None, None, 'l:' + HPBPLP, 'Delay'), ('dwet', 73, 'Wet', None, None, None, 'k', 'Delay'),
  ('level', 24, 'Level', None, None, None, 'k', 'Out')]),
 ('QUAD', 3069, 'MIDI 73-86', [
  ('csrc', 81, 'C from', None, None, None, 'l:Manual|Random|Ramps', 'Map'), ('cvar', 23, 'Var', None, None, None, 'k', 'Map'),
  ('off', 3, 'L/R', None, None, None, 'k', 'Map'),
  ('co', 77, 'Co', None, None, None, 'k', 'Ramps'), ('ca', 78, 'Ca', None, None, None, 'k', 'Ramps'),
  ('beatl', 79, 'Beats', None, None, None, 'k', 'Ramps'), ('shape', 80, 'Shape', 0, 2, 1, 'l:Saw|Sine|Rnd', 'Ramps'),
  ('qa', 21, 'A', None, None, None, 'e', 'Amp'), ('qd', 20, 'D', None, None, None, 'e', 'Amp'), ('qr', 22, 'R', None, None, None, 'e', 'Amp'),
  ('son', 60, 'On', None, None, None, 't', 'Space'), ('sz', 54, 'Size', None, None, None, 'k', 'Space'), ('spos', 58, 'Pos', None, None, None, 'k', 'Space'),
  ('smod', 55, 'Mod', None, None, None, 'k', 'Space'), ('slo', 56, 'Lo', None, None, None, 'k', 'Space'), ('shi', 57, 'Hi', None, None, None, 'k', 'Space'),
  ('smix', 59, 'Mix', None, None, None, 'k', 'Space'),
  ('level', 18, 'Level', None, None, None, 'k', 'Out')]),
]
NAME_BANK = {'K1': None, 'STOMPS': None, 'NOIX': None, 'CLIX': None, 'CRAX': None, 'QUAD': None}

# module objects by id
mods = {}
def walk(n):
    mods[n['object_id']] = n
    for c in n.get('children', []): walk(c)
for t in rep['top_level']: walk(t)
offs = sorted(int(m['offset'], 16) for m in mods.values())
inst_of = {}
def walk2(n, ins):
    if n['kind'] == 9: ins = n['object_id']
    inst_of[n['object_id']] = ins
    for c in n.get('children', []): walk2(c, ins)
for t in rep['top_level']: walk2(t, None)
ctl_by = {}   # (inst, sid) -> module
for oid, m in mods.items():
    if 'control_properties' in m:
        sid = m.get('snapshot_control_id', 0)
        ctl_by.setdefault((inst_of[oid], sid), m)

def panel_value(m):
    """saved normalised panel value of a control module (record rule found decoding CLIX/NOIX)"""
    o = int(m['offset'], 16); i = bisect.bisect_right(offs, o)
    end = offs[i] if i < len(offs) else len(data)
    k = o + 0x80
    while k < end - 12:
        if data[k:k+4] == b'\x01\x00\x00\x00' and data[k+8:k+12] == b'\x00\x00\x00\x3f':
            v = f32(k + 4)
            if 0.0 <= v <= 1.0: return v
        k += 1
    return None

# snapshot blocks (same scanner as decode_bits.py)
import re
blocks = []
for mt in re.finditer(rb'(.{4})([\x20-\x7e]{2,200})\x09\x00\x00\x00\x02\x00\x00\x00\x09\x00\x00\x00', data, re.S):
    if struct.unpack('<I', mt.group(1))[0] != len(mt.group(2)): continue
    o = mt.end(); got = None
    for q in range(o, o + 400):
        cnt = u32(q)
        if not (0 < cnt < 400): continue
        cur = q + 4; recs = {}; ok = True
        for _ in range(cnt):
            if cur + 16 > len(data): ok = False; break
            rid, size = u32(cur), u32(cur + 4)
            if size not in (0x20, 0x28, 0x4c) or u32(cur + 8) != 9 or u32(cur + 12) != 2: ok = False; break
            if size == 0x28:
                recs[rid] = ('f', f32(cur + 40)) if u32(cur + 16) == 2 else ('i', struct.unpack_from('<i', data, cur + 40)[0])
            cur += 8 + size
        if ok: got = recs; break
    if got is not None: blocks.append((mt.start(), mt.group(2).decode(), got))

# banks in file order: the second "sinebits" run of 6 is CLIX (the first is the ensemble's)
def bank(pred, start_after=0):
    return [b for b in blocks if b[0] > start_after and pred(b)]
banks = {}
banks['CLIX'] = [b for b in blocks if 0x13b000 < b[0] < 0x140000]
banks['STOMPS'] = [b for b in blocks if b[2] and max(b[2]) == 68]
banks['NOIX'] = [b for b in blocks if b[2] and max(b[2]) == 108]
banks['QUAD'] = [b for b in blocks if b[2] and max(b[2]) == 81]
banks['CRAX'] = [b for b in blocks if b[1].startswith('sinebeats')]
banks['K1'] = [b for b in blocks if b[2] and max(b[2]) == 51]
assert [len(banks[k]) for k in ['K1', 'STOMPS', 'NOIX', 'CLIX', 'CRAX', 'QUAD']] == [9, 10, 5, 6, 16, 9], {k: len(v) for k, v in banks.items()}

def floor_step(v, lo, hi, step):
    if not step: return v
    a, b = min(lo, hi), max(lo, hi)
    k = math.floor((v - lo) / (step if hi >= lo else -step) + 1e-4)
    out = lo + k * (step if hi >= lo else -step)
    return min(max(out, a), b)

out_gens = []
for name, inst, midi, controls in GENS:
    params = []
    for key, sid, label, lo, hi, step, kind, section in controls:
        m = ctl_by.get((inst, sid))
        if m is None and sid != 0: raise SystemExit(f'{name} sid {sid} ({key}) not found')
        cp = (m or {}).get('control_properties', {})
        if lo is None: lo = cp.get('range_endpoint_1', 0)
        if hi is None: hi = cp.get('range_endpoint_2', 1)
        if step is None: step = cp.get('step_or_resolution', 0)
        params.append(dict(key=key, sid=sid, label=label, lo=lo, hi=hi, step=step, kind=kind, section=section, module=m))
    def value(p, raw):
        t, v = raw
        if p['kind'].startswith('l'):
            return float(max(0, v - 1)) if t == 'i' else float(round(v))
        if p['kind'] == 't':
            return 1.0 if v > 0.25 else 0.0
        if t == 'i': return float(v)
        return floor_step(p['lo'] + v * (p['hi'] - p['lo']), p['lo'], p['hi'], p['step'])
    cur = {}; panel = {}
    for p in params:
        pv = panel_value(p['module']) if p['module'] else None
        if pv is not None and not p['kind'].startswith('l'): panel[p['key']] = value(p, ('f', pv))
        if pv is None: pv = 0.5
        if p['kind'].startswith('l'):
            n = len(p['kind'][2:].split('|'))
            cur[p['key']] = float(min(n - 1, round(pv * (n - 1)))) if p['kind'] != 'l' else 0.0
        else:
            cur[p['key']] = value(p, ('f', pv))
    pads = []
    for off, bname, recs in banks[name]:
        # a control the snapshot leaves out: the instrument's saved panel value (what it played
        # with), else what the previous pad left
        for p in params:
            if p['sid'] in recs: cur[p['key']] = value(p, recs[p['sid']])
            elif p['key'] in panel: cur[p['key']] = panel[p['key']]
        pads.append((bname, dict(cur)))
    out_gens.append((name, midi, params, pads))

G = {g[0]: g for g in out_gens}
def pad(name, i): return G[name][3][i - 1][1]
# ── checks against the screenshots ─────────────────────────────────────────────────────────
def near(a, b, tol=0.011): return abs(a - b) <= tol * max(1, abs(b))
crax6 = pad('CRAX', 6)
assert near(crax6['p'], 1870) and near(crax6['pw'], 0.92) and near(crax6['plus'], 2000) and near(crax6['bpmr'], 5), crax6
assert near(crax6['rrate'], 10000, 0.001) and near(crax6['width'], -0.22) and crax6['owave'] == 1 and crax6['ftype'] == 0, crax6
clix3 = pad('CLIX', 3)
assert near(clix3['co'], 120) and near(clix3['ca'], 50) and near(clix3['res'], 0.99) and near(clix3['mfine'], 9.21), clix3
assert near(clix3['dst'], 2.875) and near(clix3['dfeed'], 0.9) and near(clix3['sz'], 73) and near(clix3['smix'], 0.43), clix3
assert near(clix3['shi'], -5.25) and near(clix3['dfine'], 9.45) and clix3['son'] == 1, clix3
noix5 = pad('NOIX', 5)
assert near(noix5['mfine'], 14.88) and near(noix5['mfeed'], 0.3) and near(noix5['dst'], 2) and near(noix5['dfeed'], 0.7), noix5
assert near(noix5['sz'], 73) and near(noix5['smix'], 0.29) and noix5['son'] == 0, noix5
st2 = pad('STOMPS', 2)
assert near(st2['hf'], 2000) and near(st2['bf'], 33) and near(st2['pw'], 0.62) and near(st2['dfine'], 3.54) and near(st2['dfeed'], 0.9), st2
k1 = pad('K1', 1)
assert near(k1['bp'], 33) and near(k1['bdec'], 24.96) and near(k1['dfeed'], 0.6) and near(k1['dst'], 3), k1
q6 = pad('QUAD', 6)
assert near(q6['cvar'], 0.14) and near(q6['co'], -0.53, 0.02) and near(q6['ca'], 0.54, 0.02) and near(q6['beatl'], 16), q6
assert near(q6['sz'], 77) and near(q6['slo'], -12) and near(q6['smix'], 0.38) and near(q6['level'], -36), q6
print('screenshot checks: OK')

# ── C++ header ──────────────────────────────────────────────────────────────────────────────
def cfloat(x):
    s = repr(float(x))
    return s + 'f' if ('.' in s or 'e' in s) else s + '.0f'
lines = ['// Generated by tools/synthpercbox/make_factory.py from Bits_r6.ens: do not edit.',
         '// Each generator: its controls (key = the SynthDef\'s \\p_<key>) and its Reaktor snapshot bank (pads 1..n).',
         '#pragma once', '#include <vector>', '#include <string>', '',
         'namespace synthPercData {',
         'struct Param { const char* key; const char* label; const char* section; char kind; const char* options; float lo, hi, step; };',
         'struct Pad { const char* name; std::vector<float> values; };',
         'struct Generator { const char* name; const char* def; int firstNote; std::vector<Param> params; std::vector<Pad> pads; };',
         'inline const std::vector<Generator>& generators() {',
         '    static const std::vector<Generator> g = {']
DEFS = {'K1': 'SPK1', 'STOMPS': 'SPStomps', 'NOIX': 'SPNoix', 'CLIX': 'SPClix', 'CRAX': 'SPCrax', 'QUAD': 'SPQuad'}
FIRST = {'K1': 0, 'STOMPS': 14, 'NOIX': 28, 'CLIX': 43, 'CRAX': 59, 'QUAD': 73}
for name, midi, params, pads in out_gens:
    lines.append(f'        {{"{name}", "{DEFS[name]}", {FIRST[name]}, {{')
    for p in params:
        kind = p['kind'][0]; opts = p['kind'][2:] if p['kind'].startswith('l:') else ''
        lines.append(f'            {{"{p["key"]}", "{p["label"]}", "{p["section"]}", \'{kind}\', "{opts}", {cfloat(p["lo"])}, {cfloat(p["hi"])}, {cfloat(p["step"])}}},')
    lines.append('        }, {')
    for bname, vals in pads:
        lines.append(f'            {{"{bname}", {{' + ', '.join(cfloat(round(vals[p["key"]], 6)) for p in params) + '}},')
    lines.append('        }},')
lines += ['    };', '    return g;', '}', '}', '']
out = pathlib.Path(__file__).resolve().parents[2] / 'src/nodes/boxSynths/synthPercBox/scSynthPercBoxData.h'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('\n'.join(lines))
print('wrote', out)
for name, midi, params, pads in out_gens:
    print(name, len(params), 'params', len(pads), 'pads')

# the same data as JSON, for the render tests
jout = pathlib.Path(__file__).resolve().parent / 'factory.json'
jout.write_text(json.dumps([{'name': n, 'params': [{k: p[k] for k in ('key', 'label', 'lo', 'hi', 'step', 'kind', 'section')} for p in ps],
                              'pads': [{'name': bn, 'values': [vals[p['key']] for p in ps]} for bn, vals in pads]}
                             for n, midi, ps, pads in out_gens], indent=1))
print('wrote', jout)
