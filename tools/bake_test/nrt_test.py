#!/usr/bin/env python3
"""Offline render check of the Bake SynthDefs (synthdefs/Defaults/Bake).

1. BakeTap2 copies a private bus to the file raw, and works when its /d_load
   and /s_new share one packet at time zero (what writeNRTBakeScore writes).
2. BakeLoop2 loops a 2-channel buffer seamlessly, after its 20 ms fade-in.
Needs SuperCollider.app's scsynth and the addon's bundled sclang.
"""
import math, pathlib, struct, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFS = ROOT / 'synthdefs/Defaults/Bake/CompiledSynthdefs/Bake'
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
SR = 48000

def pad(b): return b + b'\0' * ((-len(b)) % 4)
def ostr(s): return pad(s.encode() + b'\0')
def message(address, *args):
    tags, data = ',', b''
    for v in args:
        if isinstance(v, str): tags += 's'; data += ostr(v)
        elif isinstance(v, int): tags += 'i'; data += struct.pack('>i', v)
        else: tags += 'f'; data += struct.pack('>f', v)
    return ostr(address) + ostr(tags) + data
def bundle(t, messages):
    b = b'#bundle\0' + struct.pack('>Q', round(t * 2**32))
    for m in messages: b += struct.pack('>I', len(m)) + m
    return struct.pack('>I', len(b)) + b
def read_wav(path):
    b = pathlib.Path(path).read_bytes(); p = 12
    while p < len(b):
        tag, size = struct.unpack_from('<4sI', b, p); p += 8
        if tag == b'fmt ': channels = struct.unpack_from('<H', b, p + 2)[0]
        if tag == b'data': return struct.unpack_from('<' + 'f' * (size // 4), b, p), channels
        p += size + (size & 1)
def write_wav(path, frames):
    ch = len(frames[0]); data = b''.join(struct.pack('<' + 'f' * ch, *f) for f in frames)
    hdr = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 3, ch, SR, SR * ch * 4, ch * 4, 32)
    pathlib.Path(path).write_bytes(hdr + b'data' + struct.pack('<I', len(data)) + data)

def render(work, packets, seconds, channels, name):
    score = work / (name + '.osc'); out = work / (name + '_render.wav')
    score.write_bytes(b''.join(packets) + bundle(seconds, [message('/c_set', 0, 0)]))
    subprocess.run([SCSYNTH, '-N', str(score), '_', str(out), str(SR), 'WAVE', 'float', '-o', str(channels)],
                   capture_output=True, text=True, timeout=120)
    samples, ch = read_wav(out)
    return [samples[i::ch] for i in range(ch)]

def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix='bake-nrt-'))
    # A test source: a 440 Hz sine (ch0) and 0.25 DC (ch1) on private bus 100
    src = work / 'src.scd'
    src.write_text('SynthDef(\\bakeTestSource, { Out.ar(100, [SinOsc.ar(440) * 0.5, DC.ar(0.25)]) })'
                   '.writeDefFile("%s");\n' % work +
                   'SynthDef(\\bakeTestRogue, { Out.ar(0, DC.ar([0.7, 0.7])) }).writeDefFile("%s"); 0.exit;\n' % work)
    cfg = work / 'sclang.yaml'
    cfg.write_text('includePaths:\n    - "%s"\nexcludeDefaultPaths: true\n' % (ROOT / 'sclang/SCClassLibrary'))
    subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(cfg), str(src)], cwd=work,
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
    ok = True

    # 1. Tap: source at the head, a rogue writer on the file's own channels
    #    (a second Output, a default out=0), tap loaded and created in one packet
    chans = render(work, [
        bundle(0, [message('/d_load', str(work / 'bakeTestSource.scsyndef')),
                   message('/d_load', str(work / 'bakeTestRogue.scsyndef')),
                   message('/s_new', 'bakeTestSource', 1000, 0, 0),
                   message('/s_new', 'bakeTestRogue', 1002, 1, 0)]),
        bundle(0, [message('/d_load', str(DEFS / 'BakeTap2.scsyndef')),
                   message('/s_new', 'BakeTap2', -1, 1, 0, 'in', 100.0, 'out', 0.0)])], 1.0, 2, 'tap')
    peak = max(abs(x) for x in chans[0][SR // 10:])
    dc = sum(chans[1][SR // 10:]) / len(chans[1][SR // 10:])
    print('tap: sine peak %.4f (want 0.5), dc %.4f (want 0.25)' % (peak, dc))
    ok &= abs(peak - 0.5) < 1e-3 and abs(dc - 0.25) < 1e-5

    # 2. Loop: a 1 s two-channel file played 3 times
    n = SR
    frames = [(math.sin(2 * math.pi * 3 * i / n) * 0.5, (i / n) - 0.5) for i in range(n)]
    write_wav(work / 'loop.wav', frames)
    chans = render(work, [
        bundle(0, [message('/b_allocRead', 0, str(work / 'loop.wav')),
                   message('/d_load', str(DEFS / 'BakeLoop2.scsyndef')),
                   message('/s_new', 'BakeLoop2', 1001, 0, 0, 'buf', 0.0, 'out', 0.0, 'levels', 1.0)])], 3.0, 2, 'loop')
    err = 0.0
    for k in (1, 2):                               # second and third pass
        for i in range(0, n, 97):
            err = max(err, abs(chans[0][k * n + i] - frames[i][0]), abs(chans[1][k * n + i] - frames[i][1]))
    print('loop: max error over passes 2-3 %.2e' % err)
    ok &= err < 1e-4
    # 3. Synced loop: 4 beats at 120 BPM = 2 s = 96000 frames. Intro file is
    #    0.1 DC, loop file a ramp 0..1 (its value is the loop's phase).
    L = 2 * SR
    write_wav(work / 'intro.wav', [(0.1,) for i in range(L)])
    write_wav(work / 'loopramp.wav', [(i / L,) for i in range(L)])
    def anchor(t, aid, pos, intro, run=1.0):
        return bundle(t, [message('/n_set', 1003, 'anchorId', float(aid), 'anchorPos', pos,
                                  'anchorIntro', float(intro), 'run', run, 'tbpm', 120.0)])
    chans = render(work, [
        bundle(0, [message('/b_allocRead', 1, str(work / 'intro.wav')),
                   message('/b_allocRead', 2, str(work / 'loopramp.wav')),
                   message('/d_load', str(DEFS / 'BakeSync1.scsyndef')),
                   message('/s_new', 'BakeSync1', 1003, 0, 0, 'introbuf', 1.0, 'loopbuf', 2.0,
                           'frames', float(L), 'loopbeats', 4.0, 'bakedbpm', 120.0, 'out', 0.0)]),
        anchor(0.0, 1, 0.0, 1),
        anchor(1.0, 2, 2.0, 0),          # a drift correction, on time: nothing changes
        anchor(5.0, 3, 1.0, 1),          # seek to beat 1: the intro again
        anchor(8.0, 4, 2.0, 0, 0.0),     # stop
    ], 9.0, 1, 'sync')
    c = chans[0]
    def at(t): return c[int(t * SR)]
    checks = [(0.5, 0.1, 'intro'), (1.9, 0.1, 'intro, after a soft anchor'),
              (3.0, 0.5, 'loop at half'), (4.5, 0.25, 'loop again'),
              (6.0, 0.1, 'intro after the seek'), (7.0, 0.25, 'loop after the seek'),
              (8.5, 0.0, 'stopped')]
    for t, want, what in checks:
        got = at(t)
        good = abs(got - want) < 2e-3
        ok &= good
        print('sync %-28s t=%.1f got %.4f want %.4f %s' % (what, t, got, want, '' if good else '<-- FAIL'))
    # the switch is sample accurate: last intro sample / first loop sample at 2 s
    print('sync switch at 2 s: %.4f %.4f' % (c[2 * SR - 2], c[2 * SR + 2]))
    ok &= abs(c[2 * SR - 2] - 0.1) < 1e-3 and c[2 * SR + 2] < 1e-3

    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
