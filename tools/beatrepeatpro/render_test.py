#!/usr/bin/env python3
"""BeatRepeat Pro timing (render_test.scd): a repeat started on a beat keeps
that beat's kick transient intact and on time in every repetition, Auto Reset
recaptures land on the beat, and Gate < 1 also closes the live pass while it
is being captured.

    python3 tools/beatrepeatpro/render_test.py [compiled BeatRepeatPro defs dir]
"""
import os, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../drumkitbox"))
from render_test import read_float_wav  # noqa: E402

DEFS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "../../synthdefs/Defaults/BeatRepeatPro/CompiledSynthdefs/BeatRepeatPro")
SCLANG = os.environ.get("SCLANG", "/Applications/SuperCollider.app/Contents/MacOS/sclang")
SR, BEAT = 48000, 24000
ATTACK = 96                          # 2 ms

def onset(k, raw):
    """Sample of beat k's kick in the raw input (first non-zero sample)."""
    start = k * BEAT
    return start + int(np.argmax(np.abs(raw[start:start + 1000]) > 1e-6))

def attack(out, raw, at, source):
    """How much of beat `source`'s first 2 ms of input is at `at`, and how many
    samples early (-) or late (+) it is."""
    first = onset(source, raw)
    ref = raw[first:first + ATTACK]
    lag = min(range(-8, 9), key=lambda d: np.sum((out[at + d:at + d + ATTACK] - ref) ** 2))
    return float(np.dot(out[at:at + ATTACK], ref) / np.dot(ref, ref)), lag

def fmt(values):
    return " ".join(f"{v:.3f}{'' if lag == 0 else f'({lag:+d})'}" for v, lag in values)

def exact(values, tolerance=0.002):
    return all(abs(v - 1) < tolerance and lag == 0 for v, lag in values)

def main():
    out = tempfile.mkdtemp(prefix="beatrepeatpro_")
    subprocess.run([SCLANG, os.path.join(HERE, "render_test.scd"), os.path.abspath(DEFS), out],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    failures = 0

    def check(ok, text):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + text)
        failures += 0 if ok else 1

    def load(name):
        a, sr = read_float_wav(os.path.join(out, name + ".wav"))
        assert sr == SR
        return a[:, 0], a[:, 2]

    # Go on beat 2 (level 0.8), quarter-note repeat: every later beat is that kick.
    o, raw = load("repeat")
    values = [attack(o, raw, onset(k, raw), 2) for k in range(3, 7)]
    check(exact(values), "quarter repeat keeps the kick's first 2 ms, on time: " + fmt(values))

    # Gate 0.5: the live pass closes half way through the capture.
    o, raw = load("gate")
    go = onset(2, raw)
    live = float(np.sqrt(np.mean(o[go + 2000:go + 10000] ** 2)))
    closed = float(np.sqrt(np.mean(o[go + 13000:go + 23000] ** 2)))
    check(live > 0.05 and closed < 0.002,
          f"gate 0.5 during capture: open rms {live:.4f}, closed rms {closed:.5f}")
    o, raw = load("gaterepeat")
    values = [attack(o, raw, onset(k, raw), 2) for k in range(3, 6)]
    check(exact(values), "gated repeats keep the kick's first 2 ms, on time: " + fmt(values))

    # Auto Reset every beat (Beats mode, then Repetitions mode with 2 eighths):
    # each beat is recaptured live on time, its eighth repeat follows intact.
    for name in ("auto", "autoloops"):
        o, raw = load(name)
        beats = [attack(o, raw, onset(k, raw), k) for k in range(3, 7)]
        halves = [attack(o, raw, onset(k, raw) + BEAT // 2, k) for k in range(2, 7)]
        check(exact(beats) and exact(halves),
              f"{name}: recaptured beats {fmt(beats)} / their repeats {fmt(halves)}")

    # Mix 0 is an exact bypass; after Go is released the output is dry again.
    o, raw = load("bypass")
    check(np.array_equal(o, raw), "mix 0 is an exact bypass")
    o, raw = load("release")
    off = int(2.6 * SR) + 30
    error = float(np.abs(o[off + 600:] - raw[off + 600:]).max())
    check(error < 1e-6, f"released Go is dry again after the declick (max error {error:.2e})")

    # Pitch Mode, Reverse, Rate with pitch-stepped Adapt Length and Auto Reset:
    # bounded, finite and repeating (not silent).
    for name in ("pitch", "reverse", "rate"):
        o, raw = load(name)
        tail = float(np.sqrt(np.mean(o[int(1.6 * SR):] ** 2)))
        check(np.isfinite(o).all() and np.abs(o).max() < 1.5 and tail > 1e-3,
              f"{name}: finite, peak {np.abs(o).max():.2f}, rms after capture {tail:.4f}")

    print("PASS" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
