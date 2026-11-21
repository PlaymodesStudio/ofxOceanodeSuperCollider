#!/usr/bin/env python3
"""How exactly BakeSync reads a synced loop at real lengths.

A 5 kHz sine is put in the loop buffer; BakeSync plays it with the transport
at the baked tempo (one anchor at the start, then drift corrections every
4 beats, as scTransportSync sends them, some off the block grid). The output
is compared with the file at the same position; the residual against the
signal, in dB, is the read error. Also: the first 5 ms after Play (no fade),
and a 2-channel def.
"""
import math, pathlib, struct, subprocess, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from nrt_test import message, bundle, read_wav, write_wav, SCSYNTH, DEFS, SR

def render(work, packets, seconds, channels, name):
    score = work / (name + '.osc'); out = work / (name + '_render.wav')
    score.write_bytes(b''.join(packets) + bundle(seconds, [message('/c_set', 0, 0)]))
    subprocess.run([SCSYNTH, '-N', str(score), '_', str(out), str(SR), 'WAVE', 'float', '-o', str(channels)],
                   capture_output=True, text=True, timeout=600)
    samples, ch = read_wav(out)
    return [samples[i::ch] for i in range(ch)]

def run(work, seconds_loop, channels, bpm=120.0):
    beats = seconds_loop * bpm / 60.0
    L = int(round(seconds_loop * SR))
    import random
    rnd = random.Random(1)
    # white noise: any misplaced read shows (a sine would hide whole periods)
    frames = [tuple(rnd.uniform(-0.5, 0.5) for c in range(channels)) for i in range(L)]
    write_wav(work / 'p_loop.wav', frames)
    write_wav(work / 'p_intro.wav', frames)       # same content: the switch is invisible
    name = 'BakeSync%d' % channels
    packets = [bundle(0, [message('/b_allocRead', 1, str(work / 'p_intro.wav')),
                          message('/b_allocRead', 2, str(work / 'p_loop.wav')),
                          message('/d_load', str(DEFS / (name + '.scsyndef'))),
                          message('/s_new', name, 1003, 0, 0, 'introbuf', 1.0, 'loopbuf', 2.0,
                                  'frames', float(L), 'loopbeats', float(beats), 'bakedbpm', bpm, 'out', 0.0)])]
    # the synth exists before Play, as in the app: Play at T0 (block-aligned)
    T0 = 0.1
    S0 = int(T0 * SR)
    total = T0 + seconds_loop * 1.5
    aid = 1
    t = T0
    while t < total:
        beat = (t - T0) * bpm / 60.0
        packets.append(bundle(t, [message('/n_set', 1003, 'anchorId', float(aid), 'anchorPos', math.fmod(beat, beats),
                                          'anchorIntro', 1.0 if aid == 1 else 0.0, 'run', 1.0, 'tbpm', bpm)]))
        aid += 1
        t += 4 * 60.0 / bpm + 0.0007      # every 4 beats, off the block grid (drift-like)
    chans = render(work, packets, total, channels, 'prec%d_%d' % (channels, int(seconds_loop)))
    worst = -200.0
    for c in range(channels):
        sig = res = 0.0
        for i in range(S0 + SR, int(total * SR) - 64, 7):
            want = frames[(i - S0) % L][c]
            sig += want * want; res += (chans[c][i] - want) ** 2
        worst = max(worst, 10 * math.log10(res / sig + 1e-30))
    attack = max(abs(chans[0][S0 + i] - frames[i][0]) for i in range(int(0.005 * SR)))
    # the offset, where it is off: best lag at a few places
    lags = []
    for t0 in (0.01, 1.0, seconds_loop * 0.5, seconds_loop + 0.5):
        i0 = S0 + int(t0 * SR)
        lags.append(min(range(-80, 81), key=lambda lag: sum((chans[0][i] - frames[(i - S0 + lag) % L][0]) ** 2 for i in range(i0, i0 + 300))))
    print('   lags at 0.01 s, 1 s, mid, after the wrap:', lags)
    return worst, attack

def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix='bake-prec-'))
    ok = True
    for seconds, ch in ((8, 1), (8, 2), (32, 1)):
        residual, attack = run(work, seconds, ch)
        print('%2d s loop, %d ch: read error %.1f dB re the signal; first 5 ms max error %.4f'
              % (seconds, ch, residual, attack))
        ok &= attack < 1e-3 and residual < -60
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
