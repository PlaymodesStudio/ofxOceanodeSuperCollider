#!/usr/bin/env python3
"""BakeSpan (Timeline Span bakes) under scsynth -N.

The file is 10 s of white noise. The transport starts 1 s before the span
(silence), the span then plays from its first frame; drift corrections come
every 2 s off the block grid; a seek lands 2 s into the file; stop. Every
region is compared sample by sample with the file.
"""
import math, pathlib, random, struct, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from nrt_test import message, bundle, write_wav, DEFS, SR
from precision_test import render

def case(leadSeconds, bufferLate):
    """leadSeconds: Play this long before the span; bufferLate: the file is
    read 0.3 s after Play (a preset loaded while the transport plays)."""
    work = pathlib.Path(tempfile.mkdtemp(prefix='bake-span-'))
    rnd = random.Random(3)
    N = 10 * SR
    data = [(rnd.uniform(-0.5, 0.5), rnd.uniform(-0.5, 0.5)) for _ in range(N)]
    write_wav(work / 'span.wav', data)
    T0, S0 = 0.1, int(0.1 * SR)          # Play (block aligned)
    lead = int(leadSeconds * SR)         # the span starts this long after Play
    seekAt, seekTo = 6.1 + leadSeconds, 2 * SR   # seek: 2 s into the file
    stopAt = 9.1 + leadSeconds
    aid, hard = 1, 1
    def anchor(t, pos, isHard, run=1.0):
        return bundle(t, [message('/n_set', 1004, 'anchorId', float(aid), 'anchorHard', float(hard),
                                  'anchorPos', float(pos), 'run', run, 'tbpm', 120.0)])
    load = message('/b_allocRead', 5, str(work / 'span.wav'))
    packets = [bundle(0, ([] if bufferLate else [load]) + [
                          message('/d_load', str(DEFS / 'BakeSpan2.scsyndef')),
                          message('/s_new', 'BakeSpan2', 1004, 0, 0, 'buf', 5.0, 'frames', float(N),
                                  'bakedbpm', 120.0, 'out', 0.0)])]
    packets.append(anchor(T0, -lead, True))
    if bufferLate: packets.append(bundle(T0 + 0.3, [load]))
    t = T0 + 2.0007
    while t < seekAt:
        aid += 1
        packets.append(anchor(t, round((t - T0) * SR) - lead, False))   # what C++ would send: on time
        t += 2.0007
    aid += 1; hard += 1
    packets.append(anchor(seekAt, seekTo, True))
    aid += 1
    packets.append(anchor(stopAt, seekTo + round((stopAt - seekAt) * SR), False, 0.0))
    end = 9.5 + leadSeconds
    packets.sort(key=lambda b: struct.unpack('>Q', b[12:20])[0])
    out = render(work, packets, end, 2, 'span')

    ok = True
    def compare(name, first, last, fileAt):
        worst = 0.0
        for i in range(first, last):
            for c in range(2):
                want = data[fileAt(i)][c] if fileAt(i) is not None else 0.0
                worst = max(worst, abs(out[c][i] - want))
        good = worst < 1e-6
        print('%-34s max error %.2e %s' % (name, worst, '' if good else '<-- FAIL'))
        return good
    print('-- lead %.0f s%s' % (leadSeconds, ', buffer read 0.3 s after Play' if bufferLate else ''))
    ok &= compare('before Play', 0, S0, lambda i: None)
    ok &= compare('Play, before the span: silent', S0 + (lead - SR if lead > SR else 0), S0 + lead, lambda i: None)
    ok &= compare('the span from its first frame', S0 + lead, int(seekAt * SR),
                  lambda i: i - S0 - lead)
    ok &= compare('after the seek', int(seekAt * SR), int(stopAt * SR),
                  lambda i: seekTo + i - int(seekAt * SR))
    tailMax = max(abs(out[0][i]) for i in range(int((stopAt + 0.05) * SR), int(end * SR) - 64))
    print('%-34s peak %.2e' % ('stopped', tailMax)); ok &= tailMax < 1e-6
    return ok

def main():
    ok = case(1.0, False)
    ok &= case(250.0, False)     # Play long before the span: anchors far below zero
    ok &= case(1.0, True)
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
