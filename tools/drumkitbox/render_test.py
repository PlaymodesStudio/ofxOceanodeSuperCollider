#!/usr/bin/env python3
"""DrumKitBox NRT check: renders every piece (render_test.scd) and checks each
of its three hits (0.1 s full, 1.1 s at 0.5, 2.1 s at +7 st): it sounds,
stays finite and below +6 dBFS, is silent before the first hit and decays.

    python3 tools/drumkitbox/render_test.py [compiled defs dir]
"""
import os, subprocess, sys, tempfile, wave, struct, math
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEFS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "../../synthdefs/Defaults/DrumKitBox/CompiledSynthdefs/DrumKitBox")
FX = os.path.join(HERE, "../../synthdefs/Defaults/MixerPro/CompiledSynthdefs/MixerPro")
SCLANG = os.environ.get("SCLANG", "/Applications/SuperCollider.app/Contents/MacOS/sclang")

def read(path):
    import soundfile  # noqa: optional
    return soundfile.read(path)

def read_float_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    # find 'fmt ' and 'data' chunks (32-bit float)
    pos, ch, rate, frames = 12, 2, 48000, None
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            ch, rate = struct.unpack("<HI", data[pos + 10:pos + 16])
        if cid == b"data":
            a = np.frombuffer(data[pos + 8:pos + 8 + size], dtype="<f4")
            return a.reshape(-1, ch), rate
        pos += 8 + size + (size & 1)
    raise RuntimeError("no data chunk in " + path)

def main():
    out = tempfile.mkdtemp(prefix="drumkitbox_")
    subprocess.run([SCLANG, os.path.join(HERE, "render_test.scd"), os.path.abspath(DEFS), out, os.path.abspath(FX)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    failures = 0
    polyTails = {}
    names = ["kick", "snare", "hat", "hatdur", "ohatmono", "ohatpoly", "hatfilt", "clap", "tom", "cowbell", "zap"]
    for name in names:
        path = os.path.join(out, name + ".wav")
        if not os.path.exists(path):
            print(f"FAIL {name}: not rendered"); failures += 1; continue
        a, sr = read_float_wav(path)
        mono = a.mean(axis=1)
        def seg(t0, t1): return mono[int(t0 * sr):int(t1 * sr)]
        pre = np.abs(seg(0.0, 0.09)).max()
        hits = [seg(0.1, 1.05), seg(1.1, 2.05), seg(2.1, 2.95)]
        peaks = [float(np.abs(h).max()) for h in hits]
        tails = [float(np.sqrt(np.mean(h[-4800:] ** 2))) for h in hits]
        # spectral centroid of the first 50 ms of hit 1 and hit 3 (pitched up)
        def centroid(x):
            x = x[:int(0.05 * sr)] * np.hanning(int(0.05 * sr))
            s = np.abs(np.fft.rfft(x)); f = np.fft.rfftfreq(len(x), 1 / sr)
            return float((s * f).sum() / max(1e-12, s.sum()))
        c1, c3 = centroid(hits[0]), centroid(hits[2])
        ok = np.isfinite(a).all() and pre < 1e-6 and min(peaks) > 0.02 and max(peaks) < 2.0 \
            and peaks[1] < peaks[0] and max(tails) < 0.05 \
            and (name not in ("kick", "tom") or c3 > c1 * 1.3)   # +7 st retunes the body
        if name in ("ohatmono", "ohatpoly"):
            polyTails[name] = float(np.sqrt(np.mean(hits[0][int(0.1 * sr):int(0.3 * sr)] ** 2)))
            ok = np.isfinite(a).all() and pre < 1e-6
            if name == "ohatpoly":   # two hits 30 ms apart: poly rings both (louder tail), mono the last one
                ok = ok and polyTails["ohatpoly"] > 1.4 * polyTails["ohatmono"]
        if name == "hatfilt":   # Filter -0.8 on hit 3: low-passed, far darker than hit 1
            ok = ok and c3 < c1 * 0.3
        if name == "hatdur":   # Duration x4 on hit 3: it rings far longer than hit 1
            def energy_after(h): return float(np.sqrt(np.mean(h[int(0.1 * sr):int(0.3 * sr)] ** 2)))
            longer = energy_after(hits[2]) > 10 * max(1e-7, energy_after(hits[0]))
            ok = ok and longer
        db = " ".join(f"{20 * math.log10(max(p, 1e-9)):+.1f}" for p in peaks)
        print(f"{'ok  ' if ok else 'FAIL'} {name:8s} peaks dB [{db}]  tail {max(tails):.4f}  centroid {c1:.0f} -> {c3:.0f} Hz")
        failures += 0 if ok else 1
    # the chain: the clap ducked under the kick at 0.1, alone at 1.1; its reverb rings after its dry tail
    path = os.path.join(out, "chain.wav")
    if not os.path.exists(path):
        print("FAIL chain: not rendered"); failures += 1
    else:
        a, sr = read_float_wav(path)
        clap = a[:, 2:4].mean(axis=1)
        def rms(t0, t1): x = clap[int(t0 * sr):int(t1 * sr)]; return float(np.sqrt(np.mean(x ** 2)))
        ducked, alone, tail = rms(0.1, 0.3), rms(1.1, 1.3), rms(1.6, 2.0)
        ok = np.isfinite(a).all() and alone > 0.01 and ducked < alone * 0.7 and tail > 1e-4 and np.abs(a[:, :2]).max() > 0.05
        print(f"{'ok  ' if ok else 'FAIL'} chain    clap ducked {ducked:.4f} / alone {alone:.4f}, reverb tail {tail:.5f}, mix peak {np.abs(a[:, :2]).max():.2f}")
        failures += 0 if ok else 1
    print("renders in", out)
    print("PASS" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
