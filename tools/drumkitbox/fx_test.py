#!/usr/bin/env python3
"""DrumKitBox's own effects (fx_test.scd): boxEchoDrift's repeats climb with
Pitch +7 (each repeat's pitch higher than the last) and boxGrainDelay keeps
sounding after the dry blip (a cloud), both finite and below +6 dBFS.

    python3 tools/drumkitbox/fx_test.py
"""
import os, subprocess, sys, tempfile
import numpy as np
from render_test import read_float_wav, SCLANG, DEFS, HERE

def main():
    out = tempfile.mkdtemp(prefix="drumkitbox_fx_")
    subprocess.run([SCLANG, os.path.join(HERE, "fx_test.scd"), os.path.abspath(DEFS), out],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    failures = 0
    a, sr = read_float_wav(os.path.join(out, "echo.wav"))
    m = a.mean(axis=1)
    def peakfreq(t0, t1):
        x = m[int(t0 * sr):int(t1 * sr)]; x = x * np.hanning(len(x))
        s = np.abs(np.fft.rfft(x, 8 * len(x))); return float(np.fft.rfftfreq(8 * len(x), 1 / sr)[np.argmax(s)])
    reps = [peakfreq(0.1 + 0.4 * k, 0.1 + 0.4 * k + 0.12) for k in (1, 2, 3)]
    ok = np.isfinite(a).all() and np.abs(a).max() < 2 and reps[1] > reps[0] * 1.3 and reps[2] > reps[1] * 1.3
    print(f"{'ok  ' if ok else 'FAIL'} echo drift  repeats at {reps[0]:.0f} -> {reps[1]:.0f} -> {reps[2]:.0f} Hz (Pitch +7)")
    failures += 0 if ok else 1
    a, sr = read_float_wav(os.path.join(out, "grain.wav"))
    m = a.mean(axis=1)
    cloud = float(np.sqrt(np.mean(m[int(0.5 * sr):int(1.5 * sr)] ** 2)))
    ok = np.isfinite(a).all() and np.abs(a).max() < 2 and cloud > 1e-3
    print(f"{'ok  ' if ok else 'FAIL'} grain delay cloud rms {cloud:.4f} after the blip, peak {np.abs(a).max():.2f}")
    failures += 0 if ok else 1
    print("PASS" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
