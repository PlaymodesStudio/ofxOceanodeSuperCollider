#!/usr/bin/env python3
"""ReaktorSVF4 (OceanodeAnalog): impulse responses of the BP4 output at several cutoffs and
resonances (Res = 1 - 10^(-dB/40), the ensemble's mapping). From 20 dB the peak's height is the
resonance in dB minus 12 (within 1.5 dB); from 40 dB the peak sits on the cutoff (within 2 %)."""
import sys, pathlib, tempfile
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
CUTS = [100.0, 440.0, 2000.0, 6000.0]
DBS = [0.0, 20.0, 40.0, 60.0]
def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix='svf4'))
    chans = [(c, d) for c in CUTS for d in DBS]
    body = 'SynthDef(\\SVF4Probe, { var x = Impulse.ar(0); Out.ar(0, [%s]); }).writeDefFile("%s");' % (
        ', '.join('ReaktorSVF4.ar(x, %f, %f, 3)' % (c, 1 - 10 ** (-d / 40)) for c, d in chans), work)
    sc._run(work, body)
    score = sc.bundle(0, [sc.message('/d_recv', (work / 'SVF4Probe.scsyndef').read_bytes()), sc.message('/s_new', 'SVF4Probe', 1000, 0, 0)])
    score += sc.bundle(6.0, [sc.message('/c_set', 0, 0.0)])
    data, ch, sr = sc.render(score, work / 'svf4.wav', len(chans))
    x = np.array(data).reshape(-1, ch)
    ok = True
    for k, (c, d) in enumerate(chans):
        h = x[:, k]; h = h[:sr * 5]
        H = np.abs(np.fft.rfft(h, 1 << 20)); fr = np.fft.rfftfreq(1 << 20, 1 / sr)
        i = np.argmax(H); pk, g = fr[i], 20 * np.log10(H[i])
        # Reaktor's tables (linear between 0 and 1/10 Nyquist) tune low resonances flat;
        # from 40 dB the peak is on the cutoff. The height: resonance dB - 12 (1 dB per unit).
        good = d < 20 or (abs(g - (d - 12)) < 1.5 and (d < 40 or abs(pk / c - 1) < 0.02))
        ok &= good
        print('cut %6.0f  res %4.0f dB: peak at %8.1f Hz, %+6.1f dB%s' % (c, d, pk, g, '' if good else '   <-- off'))
    print('PASS' if ok else 'FAIL'); return 0 if ok else 1
if __name__ == '__main__': sys.exit(main())
