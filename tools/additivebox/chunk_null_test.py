#!/usr/bin/env python3
"""Null test: AdditiveBox's chunked voice against the monolithic one it replaced.

Compiles synthdefs/Defaults/AdditiveBox/additiveBox.scd and the reference in
fixtures/additiveBox_monolithic.scd (renamed OldBox*, with the sine banks' and
the Partial Vib random phases set to 0 in both so the two can be compared
sample by sample), then renders both graphs side by side in one NRT score with
the same controls: LFO lanes (Lane and LaneN, S&H, reset), modulation buses,
vibrato, unison, 1 / 2 / 5 outputs, Line, and the pause after the gate closes.
Checks the audio outputs (bit-identical up to 64 partials: one core per copy,
noise lanes in chunks of 32; float rounding above) and the display as the node
draws it from the tail's raw values.

    python3 tools/additivebox/chunk_null_test.py [--keep DIR]
"""
import argparse, math, pathlib, re, subprocess, sys, tempfile
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

SCLANG = ROOT / 'sclang/osx/sclang'
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
CAPS, CHUNKS, OUTS = [8, 64, 128], [8, 32, 64], [1, 2, 5]
CHUNK, NOISE_CHUNK = 64, 32   # as scAdditiveBox.h
NEW_VIS = 2 * 64 + 2           # the tail's display: D levels, D positions, level factor, pan offset
SR, DUR = 48000, 2.0


def prepare(source, old):
    s = source.read_text()
    s = s.replace('caps = [8, 16, 32, 64, 128, 256]', 'caps = %s' % CAPS)
    s = s.replace('chunks = [8, 16, 32, 64]', 'chunks = %s' % CHUNKS)
    s = s.replace('(1..maxOut).do', '%s.do' % OUTS)
    # deterministic phases (the only random draws on this path)
    s, n = re.subn(r'\{ Rand\(0, 2pi\) \} ! (np|nc)', r'(0 ! \1)', s)
    assert n == 2, n
    if old:
        s = s.replace('"AdditiveBox', '"OldBox')
        # its LaneN128 alone takes ~45 s to build: up to 64 is enough for the noise cases
        s = s.replace('[false, true].do { |withNoise|', '(if(np <= 64) { [false, true] } { [false] }).do { |withNoise|')
    return s


def compile_defs(work):
    (work / 'new').mkdir(); (work / 'old').mkdir()
    (work / 'new/additiveBox.scd').write_text(prepare(ROOT / 'synthdefs/Defaults/AdditiveBox/additiveBox.scd', False))
    (work / 'old/additiveBox.scd').write_text(prepare(pathlib.Path(__file__).parent / 'fixtures/additiveBox_monolithic.scd', True))
    probes = ''.join('SynthDef(\\ABProbe%d, { Out.ar(\\out.kr(0), K2A.ar(In.kr(\\in.kr(0), %d))) }).writeDefFile("%s");\n'
                     % (n, n, work) for n in sorted({2 * min(p, 64) for p in CAPS} | {NEW_VIS}))
    run = work / 'run.scd'
    run.write_text('"%s".load;\n"%s".load;\n%s"COMPILED".postln;\n0.exit;\n' % (work / 'new/additiveBox.scd', work / 'old/additiveBox.scd', probes))
    config = work / 'config.yaml'
    config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    home = work / 'home'; home.mkdir()
    r = subprocess.run([str(SCLANG), '-l', str(config), str(run)], capture_output=True, text=True, timeout=900,
                       stdin=subprocess.DEVNULL, env={'HOME': str(home), 'PATH': '/usr/bin:/bin'})
    assert 'COMPILED' in r.stdout, r.stdout[-3000:] + r.stderr[-2000:]
    defs = sorted((work / 'new/CompiledSynthdefs/AdditiveBox').glob('*.scsyndef'))
    defs += sorted((work / 'old/CompiledSynthdefs/OldBox').glob('*.scsyndef'))
    return defs + sorted(work.glob('ABProbe*.scsyndef'))


def setn(node, controls):
    args = []
    for name, value in controls.items():
        values = value if isinstance(value, list) else [value]
        args += [name, len(values)] + [float(x) for x in values]
    return message('/n_setn', node, *args)


def render(work, defs, case):
    P, O, U, noise, line = case['P'], case['O'], case['U'], case['noise'], case['line']
    C, LC = min(P, CHUNK), min(P, NOISE_CHUNK if noise else CHUNK)
    nd = min(P, 64)
    MOD, OLDBUS, NEWBUS, OLDVIS, NEWVIS, JUNK = 200, 1000, 2000, 3000, 3200, 3400
    GATEBUS = 1000   # audio, private
    k = np.arange(P)
    pa = list(1.0 / (k + 1) + 0.05 * np.sin(k * 1.7))
    pd = list(0.3 * np.cos(k * 0.9))
    fr = list((k + 1) * (1 + 0.002 * k))
    pb = list((k * 0.6180339) % 1.0)
    lanes = {}
    for l, (rate, sh, amt) in enumerate([(1.3, 0.0, [0.4, 0.2, 0.3]), (0.7, 6.0, [0.25, 0.1, 0.5])]):
        L = 'l%d' % l
        lanes.update({L + 'buf': 0, L + 'cbuf': 1, L + 'rate': rate, L + 'reset': 0, L + 'sh': sh, L + 'shg': 0.05,
                      L + 'amt': amt, L + 'noise': 1.0 if (noise and l == 1) else 0.0, L + 'nscale': 0.3,
                      L + 'init': 0.2 * l, L + 'seed': 2.5 + l, L + 'ph': list((k * (0.37 + 0.11 * l)) % 1.0)})
    common = {'modbus': MOD, 'voice': 0}
    src = dict(common, gate=1, pa=pa, pd=pd, morph=0.3, maskglide=0.05, fr=fr, pb=pb,
               gatebus=GATEBUS, rndbus=JUNK, velbus=JUNK + 1)
    vib = {'vibrate': 5.0, 'vibdepth': 0.3}

    def core(u):
        return dict(common, pitch=48 + 7.03 * u, pitchoff=0, glide=0, gate=1, shift=3.0, amp=0.5 if u == 0 else 0.3,
                    vca=0, panoff=0.1 * u, line=line, width=2.0)

    ev = [(0, [message('/d_recv', p.read_bytes()) for p in defs] +
           [message('/b_alloc', 0, 257, 1), message('/b_alloc', 1, 257, 1)])]
    t0 = 0.01
    x = np.arange(257) / 256
    first = [message('/b_setn', 0, 0, 257, *[float(v) for v in 0.5 + 0.5 * np.sin(2 * math.pi * x)]),
             message('/b_setn', 1, 0, 257, *[float(v) for v in x ** 1.5]),
             message('/c_setn', MOD, 160, *([0.0] * 160))]
    # modulation, voice 0: pitch, level, pan, P.level, P.pan, P.pitch
    for target, value in [(2, 0.2), (3, 0.1), (5, 0.05), (7, 0.2), (8, 0.03), (9, -0.1)]:
        first.append(message('/c_set', MOD + target * 16, value))
    first += [message('/g_new', 1000, 1, 0), message('/g_new', 2000, 1, 0)]
    nodes = {'old': [], 'new': []}

    def synth(graph, name, controls):
        nid = (1000 if graph == 'old' else 2000) + 1 + len(nodes[graph])
        nodes[graph].append((nid, controls))
        first.append(message('/s_new', name, nid, 1, 1000 if graph == 'old' else 2000))
        first.append(setn(nid, controls))
        return nid

    # the monolithic voice
    synth('old', 'OldBoxSrc%d' % P, dict(src, bus=OLDBUS))
    synth('old', ('OldBoxLaneN%d' if noise else 'OldBoxLane%d') % P, dict(common, bus=OLDBUS, **lanes))
    old_cores = [synth('old', 'OldBoxV%d_%d' % (P, O), dict(core(u), **vib, bus=OLDBUS, out=0,
                       vis=OLDVIS if u == 0 else JUNK, pauser=1 if u == 0 else 0, grp=1000)) for u in range(U)]
    # the chunked one
    synth('new', 'AdditiveBoxSrc%d' % P, dict(src, **vib, bus=NEWBUS))
    clock = {n: v for n, v in lanes.items() if not n.endswith('ph') and not n.endswith('amt')}
    synth('new', 'AdditiveBoxLaneClock', dict(clock, bus=NEWBUS, np=P))
    lane_ids = []
    for c in range(P // LC):
        chunk = dict(lanes, l0ph=lanes['l0ph'][c * LC:(c + 1) * LC], l1ph=lanes['l1ph'][c * LC:(c + 1) * LC])
        lane_ids.append(synth('new', ('AdditiveBoxLanesN%d' if noise else 'AdditiveBoxLanes%d') % LC,
                              dict(common, **chunk, bus=NEWBUS, np=P, first=c * LC)))
    new_cores = [synth('new', 'AdditiveBoxCore%d_%d' % (C, O), dict(core(u), bus=NEWBUS, np=P, first=c * C, out=O,
                       silbus=NEWBUS + 3 * P if u == 0 else JUNK))
                 for u in range(U) for c in range(P // C)]
    tail = synth('new', 'AdditiveBoxTail%d' % P, dict(common, bus=NEWBUS, vis=NEWVIS, gate=1, pauser=1, grp=2000))
    first.append(message('/s_new', 'ABProbe%d' % (2 * nd), 3001, 1, 0, 'in', OLDVIS, 'out', 2 * O))
    first.append(message('/s_new', 'ABProbe%d' % NEW_VIS, 3002, 1, 0, 'in', NEWVIS, 'out', 2 * O + 2 * nd))
    ev.append((t0, first))
    # the lanes' reset, then gate off and level 0: the voices pause once silent
    ev.append((0.6, [message('/n_set', nid, 'l0reset', 1.0, 'l1reset', 1.0) for nid in
                     [nodes['old'][1][0], nodes['new'][1][0]] + lane_ids]))
    off = [message('/n_set', nid, 'gate', 0.0) for nid, _ in nodes['old'] + nodes['new']]
    off += [message('/n_set', nid, 'amp', 0.0) for nid in old_cores + new_cores]
    ev.append((1.0, off))
    ev.append((DUR - 0.01, [message('/g_freeAll', 0)]))
    ev.append((DUR, [message('/c_set', 0, 0.0)]))
    score = b''.join(bundle(t, ms) for t, ms in ev)
    osc, wav = work / 'score.osc', work / 'out.wav'
    osc.write_bytes(score)
    channels = 2 * O + 2 * nd + NEW_VIS
    r = subprocess.run([SCSYNTH, '-N', str(osc), '_', str(wav), str(SR), 'WAV', 'float', '-o', str(channels), '-i', '0',
                        '-a', '2048', '-c', '4096', '-m', '65536', '-w', '8192', '-D', '0'], capture_output=True, text=True, timeout=300)
    assert r.returncode == 0 and 'FAILURE' not in r.stdout + r.stderr, r.stdout[-3000:] + r.stderr[-2000:]
    samples, ch, sr = read_wav(wav)
    a = np.asarray(samples, dtype=np.float64).reshape(-1, ch)
    old, new = a[:, :O], a[:, O:2 * O]
    vold, raw = a[:, 2 * O:2 * O + 2 * nd], a[:, 2 * O + 2 * nd:]
    # the display as the node draws it (scAdditiveBox::liveLevel / livePos)
    levels = raw[:, :nd] * raw[:, 128:129]
    pos = raw[:, nd:2 * nd] + raw[:, 129:130]
    if O == 2 or (O > 2 and line):
        pos = pos - 2 * np.floor(pos / 2)
        pos = np.where(pos > 1, 2 - pos, pos)
    return old, new, vold, np.concatenate([levels, pos], axis=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--keep', type=pathlib.Path, help='keep the work files here')
    args = parser.parse_args()
    cases = [dict(P=64, O=2, U=2, noise=True, line=0), dict(P=64, O=5, U=1, noise=False, line=1),
             dict(P=64, O=5, U=2, noise=True, line=0), dict(P=8, O=1, U=2, noise=True, line=0),
             dict(P=128, O=5, U=2, noise=False, line=1), dict(P=128, O=2, U=1, noise=False, line=0)]
    with tempfile.TemporaryDirectory(prefix='additivebox_chunks_') as tmp:
        work = args.keep or pathlib.Path(tmp)
        work.mkdir(parents=True, exist_ok=True)
        defs = compile_defs(work)
        failed = False
        for case in cases:
            old, new, vold, vnew = render(work, defs, case)
            sounding = slice(int(0.05 * SR), int(0.95 * SR))
            peak = np.max(np.abs(old[sounding]))
            # The historical core held its last gate level after note-off.
            # Compare the common held-note behavior; the current core's direct
            # gate closure is covered by morph_render_test.py.
            comparable = slice(0, int(1.0 * SR))
            diff = np.max(np.abs(old[comparable] - new[comparable]))
            # at the first sample of each block, where the probe's K2A ramp holds
            # the last block's value (mid-ramp a fold of a ramp is not a ramp of a fold)
            vdiff = np.max(np.abs(vold[comparable][::64] - vnew[comparable][::64]))
            # the display moves while the voice plays, freezes once it pauses
            moving = np.ptp(vold[int(0.2 * SR):int(0.9 * SR)], axis=0).max()
            frozen = np.ptp(vold[int(1.5 * SR):int(1.9 * SR)], axis=0).max()
            frozen_new = np.ptp(vnew[int(1.5 * SR):int(1.9 * SR)], axis=0).max()
            # up to 64 partials a copy is one core, as before: bit-identical; above, the
            # chunks' sums differ from one sum by float rounding only
            tolerance = 0.0 if case['P'] <= CHUNK else 1e-6 * peak
            ok = peak > 0.01 and diff <= tolerance and vdiff <= 1e-6 and moving > 1e-3 \
                 and frozen == 0 and frozen_new == 0
            failed |= not ok
            print('%s P=%d O=%d U=%d noise=%d line=%d: peak %.4f, audio diff %.2e (%.1f dB), display diff %.2e, '
                  'display moving %.3f, frozen after pause %g / %g' %
                  ('ok  ' if ok else 'FAIL', case['P'], case['O'], case['U'], case['noise'], case['line'], peak, diff,
                   20 * math.log10(max(diff, 1e-12) / peak), vdiff, moving, frozen, frozen_new))
        sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
