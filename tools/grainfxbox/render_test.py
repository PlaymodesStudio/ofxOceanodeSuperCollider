#!/usr/bin/env python3
"""GrainFxBox offline checks (scsynth NRT, no app): one engine's real chain
(recorder -> control -> voice -> output -> dry) on a stereo test input,
L = 440 Hz, R = 660 Hz.

  1. Source placement (1:1), grains, wet only: L stays 440, R stays 660.
  2. Wet Mode Gated + Gate Motion tape stop (1 -> 0 over 1 s, Grainless):
     before the gate the dry passes; halfway through the stop the tape plays
     at half speed (220 / 330 Hz) with the dry muted; then silence.
  3. Stutter (Freeze + Repeat, grains) on a rising sweep: while the gate is
     up the output keeps repeating the quarter second before it.

Compiles scGrainBoxPoly.scd (reduced: Ctl_8, 2 channels) with the bundled
sclang into the output folder.   tools/grainfxbox/render_test.py [--output DIR]
"""
import argparse, pathlib, shutil, subprocess, sys
import numpy as np
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

SCLANG = ROOT / 'sclang/osx/sclang'
SCSYNTH = '/Applications/SuperCollider.app/Contents/Resources/scsynth'
SR = 48000
IN, GATE, MIX, CLK, PHASE, CTL = 16, 20, 32, 50, 60, 100   # buses (audio / control)


def compile_defs(work):
    s = (ROOT / 'synthdefs/Defaults/GrainBox/scGrainBoxPoly.scd').read_text()
    s = s.replace('caps = [8, 32, 128, 500]', 'caps = [8]').replace('(2..16).do', '(2..2).do').replace('(1..16).do', '(2..2).do')
    (work / 'gb.scd').write_text(s)
    probes = ('SynthDef(\\GBTestSrc, { Out.ar(%d, [SinOsc.ar(440, 0, 0.25), SinOsc.ar(660, 0, 0.25)]) }).writeDefFile("%s");\n'
              'SynthDef(\\GBTestSweep, { Out.ar(%d, SinOsc.ar(Line.ar(200, 1000, 4)) * 0.25 ! 2) }).writeDefFile("%s");\n'
              'SynthDef(\\GBTestGate, { Out.ar(%d, K2A.ar(\\gate.kr(0))) }).writeDefFile("%s");\n' % (IN, work, IN, work, GATE, work))
    (work / 'run.scd').write_text('"%s".load;\n%s"COMPILED".postln;\n0.exit;\n' % (work / 'gb.scd', probes))
    (work / 'config.yaml').write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    home = work / 'home'; home.mkdir(exist_ok=True)
    r = subprocess.run([str(SCLANG), '-l', str(work / 'config.yaml'), str(work / 'run.scd')], capture_output=True, text=True,
                       timeout=900, stdin=subprocess.DEVNULL, env={'HOME': str(home), 'PATH': '/usr/bin:/bin'})
    assert 'COMPILED' in r.stdout and 'ERROR' not in r.stdout, r.stdout[-3000:]
    return sorted((work / 'CompiledSynthdefs/GrainBoxPoly').glob('*.scsyndef')) + sorted(work.glob('GBTest*.scsyndef'))


def render(work, defs, name, voice, ctl_args, dry_args, gate_events, dur, src='GBTestSrc'):
    frames = 4 * SR
    t0 = [message('/d_recv', p.read_bytes()) for p in defs]
    t0 += [message('/b_alloc', 0, frames, 1), message('/b_alloc', 1, frames, 1), message('/g_new', 1, 0, 0)]
    score = bundle(0, t0)
    ctl = ['ctl', CTL, 'clk', CLK, 'bufnum0', 0, 'bufnum1', 1, 'bufs', '$bufs', 'live', 1.0, 'phasebus', PHASE,
           'envbuf', -1.0, 'bpm', 120.0, 'inpoint', 0.0, 'outpoint', 1.0, 'vis', 0.0] + ctl_args
    ctl_msg = message('/s_new', 'GrainBoxPolyCtl_8', 1003, 1, 1, *[a for a in ctl if a != '$bufs' and a != 'bufs'])
    score += bundle(0.01, [
        message('/s_new', src, 1000, 1, 1),
        message('/s_new', 'GBTestGate', 1001, 1, 1),
        message('/s_new', 'GrainBoxPolyRecC_2', 1002, 1, 1, 'in', IN, 'phasebus', PHASE, 'vis', 0.0, 'tfrz', CTL + 100),
        message('/n_setn', 1002, 'bufs', 2, 0.0, 1.0),
        ctl_msg,
        message('/n_setn', 1003, 'bufs', 2, 0.0, 1.0),
        message('/s_new', voice, 1004, 1, 1, 'out', MIX, 'ctl', CTL, 'clk', CLK, 'idx', 0),
        message('/s_new', 'GrainBoxPolyOut_2', 1005, 1, 1, 'in', MIX, 'out', 0, 'ctl', CTL, 'srck', 2.0),
        message('/s_new', 'GrainBoxPolyDry_2', 1006, 1, 1, 'in', IN, 'out', 0, *dry_args)])
    for t, g in gate_events:
        score += bundle(t, [message('/n_set', 1001, 'gate', float(g))])
    score += bundle(dur, [message('/c_set', 0, 0.0)])
    osc = work / (name + '.osc'); osc.write_bytes(score)
    wav = work / (name + '.wav')
    r = subprocess.run([SCSYNTH, '-N', str(osc), '_', str(wav), str(SR), 'WAV', 'float', '-o', '2', '-i', '0',
                        '-c', '4096', '-a', '1024'], capture_output=True, text=True, timeout=300)
    assert wav.exists(), r.stdout[-2000:] + r.stderr[-2000:]
    data, ch, rate = read_wav(wav)
    return np.array(data, dtype=np.float64).reshape(-1, ch)


def level(x, a, b):
    seg = x[int(a * SR):int(b * SR)]
    return float(np.sqrt(np.mean(seg ** 2)) + 1e-12)


def tone(x, a, b, f):
    seg = x[int(a * SR):int(b * SR)]
    n = np.arange(len(seg))
    return float(abs(np.sum(seg * np.exp(-2j * np.pi * f * n / SR))) * 2 / len(seg))


def peak(x, a, b):
    seg = x[int(a * SR):int(b * SR)] * np.hanning(int(b * SR) - int(a * SR))
    spec = np.abs(np.fft.rfft(seg, 1 << 17))
    return float(np.argmax(spec) * SR / (1 << 17))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=pathlib.Path, default=pathlib.Path('/tmp/grainfxbox-render'))
    a = ap.parse_args()
    shutil.rmtree(a.output, ignore_errors=True); a.output.mkdir(parents=True)
    defs = compile_defs(a.output)
    ok = True

    def check(cond, text):
        nonlocal ok
        ok &= bool(cond)
        print(('PASS ' if cond else 'FAIL ') + text)

    # 1. Source placement, grains (40 ms interval, overlap 2, 200 ms behind), wet only
    grains = ['autotrig', 1.0, 'trigdur', 1.0, 'overlap', 2.0, 'autotrigbeatdiv', '$div',
              'amp', 1.0, 'pitch', 0.0, 'duration', 0.1, 'position', 0.05, 'chance', 1.0, 'levels', 1.0]
    args = []
    for i, v in enumerate(grains):
        args.append(12.5 if v == '$div' else v)   # 25 grains/s at 120 bpm = 12.5 per beat
    x = render(a.output, defs, 'source', 'GrainBoxPolyVC_2', args, ['dry', 0.0], [], 3.0)
    L, R = x[:, 0], x[:, 1]
    l440, l660, r440, r660 = tone(L, 1, 2.5, 440), tone(L, 1, 2.5, 660), tone(R, 1, 2.5, 440), tone(R, 1, 2.5, 660)
    print('  L: 440 %.3f 660 %.3f   R: 440 %.3f 660 %.3f' % (l440, l660, r440, r660))
    check(l440 > 0.05 and r660 > 0.05, 'Source: both channels sound')
    check(l440 > 20 * l660 and r660 > 20 * r440, 'Source: L keeps 440 Hz, R keeps 660 Hz (1:1, > 26 dB apart)')

    # 2. Wet Mode Gated + tape stop (Grainless): gate 1.5 .. 3.5 s
    tape = ['amp', 1.0, 'position', 0.0, 'levels', 1.0, 'tmotion', 1.0, 'tstart', 1.0, 'tend', 0.0,
            'ttime', 1.0, 'tcurve', 0.0, 'livelen', 3.98, 'gatebus', GATE, 'tenvbus', -1.0]
    x = render(a.output, defs, 'tapestop', 'GrainBoxPolyDC_2', tape, ['dry', 1.0, 'gated', 1.0, 'gatebus', GATE],
               [(1.5, 1), (3.5, 0)], 4.0)
    L, R = x[:, 0], x[:, 1]
    # before the gate: dry (0.25) + the tape at 1 sample's delay (0.25), in phase
    print('  before: L %.3f   during 1.85-2.15: L peak %.0f Hz R peak %.0f Hz   after stop: %.4f' %
          (level(L, 0.5, 1.4), peak(L, 1.85, 2.15), peak(R, 1.85, 2.15), level(L, 2.7, 3.4)))
    check(abs(level(L, 0.5, 1.4) - 0.354) < 0.03, 'Gated: before the gate the dry and the in-step tape (one sample apart) pass')
    check(abs(peak(L, 1.85, 2.15) - 220) < 25 and abs(peak(R, 1.85, 2.15) - 330) < 35,
          'Tape stop: half way the tape is at half speed (220 / 330 Hz), each channel its own')
    check(tone(L, 1.85, 2.15, 440) < 0.02, 'Gated: the dry (440 Hz) is muted while the gate is up')
    check(level(L, 2.7, 3.4) < 0.003 and level(R, 2.7, 3.4) < 0.003, 'Tape stop: silent once stopped')
    check(level(L, 3.7, 3.95) > 0.1, 'Gate down: the dry is back')
    # 3. Stutter: Freeze + Repeat every 0.25 s at speed 1, grains; gate 1.5 .. 3.5 s
    stut = args + ['tmotion', 1.0, 'tstart', 1.0, 'tend', 1.0, 'ttime', 0.25, 'trepeat', 1.0, 'tfreeze', 1.0,
                   'livelen', 3.98, 'gatebus', GATE, 'tenvbus', -1.0]
    stut[stut.index('position') + 1] = 0.0
    x = render(a.output, defs, 'stutter', 'GrainBoxPolyVC_2', stut, ['dry', 1.0, 'gated', 1.0, 'gatebus', GATE],
               [(1.5, 1), (3.5, 0)], 4.0, src='GBTestSweep')
    L = x[:, 0]
    early, late = peak(L, 1.6, 1.85), peak(L, 3.1, 3.35)
    print('  sweep at the gate ~500 Hz; looped 1.6-1.85 s: %.0f Hz, 3.1-3.35 s: %.0f Hz (live would be ~830)' % (early, late))
    check(440 < early < 510 and 440 < late < 510, 'Stutter: the frozen quarter second before the gate repeats')
    check(level(L, 1.6, 3.4) > 0.1, 'Stutter: it sounds')
    print('ALL PASS' if ok else 'SOME CHECKS FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
