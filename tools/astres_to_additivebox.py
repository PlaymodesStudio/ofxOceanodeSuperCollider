#!/usr/bin/env python3
"""Turn the snapshots of the ADDITIVE_ASTRES macro into AdditiveBox presets.

    python3 tools/astres_to_additivebox.py <macro folder> <output folder>

The macro (LIQUIDTILES_BIENAL .../Macros/AUDIO/SYNTH/ADDITIVE_ASTRES) builds an
additive synth out of ~270 Oceanode nodes driving Additive1080_. Each snapshot
stores its Router values; this maps them to AdditiveBox controls. The mapping
is an approximation of the macro's node network (see the notes per control),
a starting point to tune by ear. The macro's effects (comb, tank reverb,
chorus, Vadim filter) are not part of the synth and are left out, and so is
the macro's channel count (13, or 2): Outputs is the speaker setup, which a
preset leaves as it is.

Timing: the macro's phasors run at 120 BPM (one beat = 0.5 s).
"""
import json, math, os, sys

NAMES = [
    "Astres 00 Moving", "Astres 01 Chord", "Astres 02 Chord BP", "Astres 03 Unison C1",
    "Astres 04 Low Odd", "Astres 05 Sweep", "Astres 06 Chord II", "Astres 07 Unison F1",
    "Astres 08 Unison E1", "Astres 09 Stereo Sub",
]
BEAT = 0.5


def resample(values, n):
    """Oceanode Vector Resize (resample on): a vector stretched over n voices."""
    if not isinstance(values, list):
        values = [values]
    return [float(values[min(len(values) - 1, int(i * len(values) / n))]) for i in range(n)]


def first(values):
    return float(values[0] if isinstance(values, list) else values)


def convert(rv):
    get = lambda key, default: rv.get(key, {}).get("value", default)
    pitch = get("pitch[]", [48.0])
    pitch = pitch if isinstance(pitch, list) else [pitch]
    V = min(16, len(pitch))
    pitch = [float(p) for p in pitch[:V]]
    partials = int(round(first(get("PartialNum", 30))))
    each = lambda key, default: resample(get(key, [default]), V)

    hi_freq = first(get("HiFreq[]", 0.6))
    hi_damp = each("HiDamp[]", 0.0)
    bp = each("BP[]", 0.0)
    # EVEN/ODD switcher: round(v * 2) -> 0 odd only, 1 even only, 2 all
    even_odd = [{0: -1.0, 1: 1.0}.get(int(round(v * 2)), 0.0) for v in each("Even/Odd/All[]", 1.0)]
    pan_spread = first(get("panSpread[]", 1.0))
    env_time = first(get("EnvTimeScale[]", 6.17)) * BEAT      # envelope phasor cycle (s)

    p = {
        "Voices": V,
        "Pitch": pitch,
        "Levels": [min(1.0, v * 0.5) for v in each("Levels[]", 1.0)],   # macro 1 = AdditiveBox 0.5
        "Glide": [0.0],
        "Volume": min(2.0, first(get("Volume", 1.0))),
        "Partials": max(1, min(256, partials)),
        # HIDAMP (high-partial damping) and LOUDNESS (HiFreq: floor of the
        # equal-loudness weighting) both darken the top: folded into a tilt
        "Tilt": [round(-2.0 - 7.0 * h - 5.0 * (1.0 - hi_freq), 2) for h in hi_damp],
        "Stretch": [1.0], "Inharm": [0.0], "Detune": [0.0], "Shift": [0.0],
        "Even_Odd": even_odd,
        # BP moves the emphasis of the damping curve up the series: a gentle
        # band-pass keytracked to each voice, centred on partial 1 + BP * (N - 1) / 2
        "Filter": [2 if b > 0 else 0 for b in bp],
        "Cutoff": [round(60.0 + 12.0 * math.log2(1.0 + b * (partials - 1) * 0.5), 2) for b in bp],
        "Keytrack": [1.0], "Slope": [9.0], "Res": [0.0],
        # LFO box: a wave across the partials of each voice (Indexer over
        # PartialNum), phasor Div 16 x Speed; depth fltLFOamt
        "Sweep": [first(get("fltLFOamt[]", 0.8))],
        "Sweep_Waves": [first(get("fltWaveNum", 1.0))],
        "Sweep_Rate": [round(first(get("fltLFOSpeed[]", 1.9)) / (16 * BEAT), 3)],
        "Sweep_Sharp": [0.0], "Sweep_Mode": [0], "Sweep_Phase": [0.0],
        # PARTIAL MASK: random quantised to 0/1 after pow p -> density 0.5 + 0.5 p
        "Density": [round(0.5 + 0.5 * m, 3) for m in each("partialMask[]", 0.0)],
        # RANDOM AMPS at pow 1 (every snapshot) is nearly flat
        "Rand_Amp": [0.0], "Mask_Glide": [0.05], "Seed": 0, "Reroll": True,
        # AMP ENVELOPE: attack 0, sustain 1, release a random share in
        # [decayMin, 1] of the envelope phasor's cycle
        "Attack": [0.005], "Decay": [0.5], "Sustain": [1.0],
        "Release": [round(env_time, 3)], "Curve": [-3.0],
        "Env_Rand": [round(1.0 - first(get("decayMin[]", 0.53)), 3)],
        # The macro's damping follows the release: the top dies first
        "Env_Tilt": [0.3],
        # VIBRATO: vibAmp is peak to peak in semitones; phasor Div 1 x vibMult
        "Vibrato": [round(first(get("vibAmp[]", 0.11)) * 0.5, 4)],
        "Vib_Rate": [round(first(get("vibMult[]", 2.67)) / BEAT, 3)],
        # Per-partial vibrato: depth up to vibPartials (peak to peak), phasor
        # Div 4 x 1..3 -> 0.5 .. 1.5 Hz
        "Partial_Vib": [round(first(get("vibPartials[]", 0.41)) * 0.5, 4)],
        "Partial_Vib_Rate": [1.0],
        "Shimmer": [first(get("Tremolo[]", 0.0))], "Shimmer_Rate": [4.0],
        # PAN: each voice at its PanAz position (-1..1 around the ring), or
        # its partials scattered at random (panSpread switch)
        "Pan": [round(((a + 1.0) * 0.5) % 1.0, 4) for a in each("PanAz[]", 0.0)],
        "Spread": [1.0 if pan_spread >= 0.5 else 0.0],
        "Pan_Mode": [0], "Drift": [0.0], "Drift_Rate": [0.1], "Width": [2.0],
    }
    return p


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    macro, out = sys.argv[1], sys.argv[2]
    snapshots = json.load(open(os.path.join(macro, "snapshots.json")))
    os.makedirs(out, exist_ok=True)
    for key in sorted(snapshots, key=int):
        name = NAMES[int(key)] if int(key) < len(NAMES) else "Astres %02d" % int(key)
        with open(os.path.join(out, name + ".json"), "w") as f:
            json.dump(convert(snapshots[key]["routerValues"]), f, indent=4)
        print("wrote", name)


if __name__ == "__main__":
    main()
