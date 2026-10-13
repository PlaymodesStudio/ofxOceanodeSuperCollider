#!/usr/bin/env python3
"""Demo presets for the SC PulsarBox node (src/nodes/scPulsarBox.h).

Writes <name>.json into synthdefs/Defaults/PulsarBox/Presets (the source) and,
if it exists, the OceanAudio app's data/Supercollider/PulsarBox/Presets. Keys
are the parameters' escaped names; a preset resets every control to its
default first, so only what differs is listed here.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON = os.path.dirname(HERE)
TARGETS = [
    os.path.join(ADDON, "synthdefs/Defaults/PulsarBox/Presets"),
    os.path.join(ADDON, "../../apps/Playmodes/OceanAudio/bin/data/Supercollider/PulsarBox/Presets"),
]
NUPG = "Supercollider/PulsarBox/nuPG_TABLES/"


def layer(letter, hz=None, ratio=None, cycles=1.0, level=1.0, delay=0.0, pan=0.0,
          pulsaret="Sine", envelope="Hann"):
    d = {
        f"Formant_Mode_{letter}": 1 if ratio is not None else 0,
        f"Cycles_{letter}": cycles,
        f"Level_{letter}": level,
        f"Delay_{letter}": delay,
        f"Pan_{letter}": pan,
        f"Pulsaret_{letter}": pulsaret,
        f"Envelope_{letter}": envelope,
    }
    if hz is not None:
        d[f"Formant_{letter}"] = hz
    if ratio is not None:
        d[f"Ratio_{letter}"] = ratio
    return d


def euclid(hits, steps):
    return [1 if (i * hits) % steps < hits else 0 for i in range(steps)]


def preset(voices=1, fund=(110.0,), levels=(0.5,), **extra):
    p = {"Voices": voices, "Fundamental": list(fund), "Levels": list(levels)}
    p.update(extra)
    return p


PRESETS = {
    "01 Basic Pulsar": preset(
        fund=[110], Formants=1, Volume=1.4, **layer("A", hz=880, cycles=1)),
    "02 Vowel Ah": preset(
        fund=[130], Formants=3, Volume=1.2, Rate_Jitter=0.008,
        **layer("A", hz=730, cycles=3, envelope="Gauss"),
        **layer("B", hz=1090, cycles=3, level=0.55, envelope="Gauss"),
        **layer("C", hz=2440, cycles=4, level=0.3, envelope="Gauss")),
    "03 Formant Choir": preset(
        voices=4, fund=[98, 123.5, 146.8, 185], levels=[0.45], Formants=3, Volume=0.9,
        Rate_Jitter=0.012, Attack=0.8, Release=1.5,
        **layer("A", hz=300, cycles=4, envelope="Gauss"),
        **layer("B", hz=870, cycles=4, level=0.4, envelope="Gauss"),
        **layer("C", hz=2240, cycles=5, level=0.2, envelope="Gauss")),
    "04 Euclid Clicks": preset(
        fund=[8], Formants=1, Volume=1.0, Sieve=euclid(5, 8),
        **layer("A", hz=3200, cycles=6, envelope="Expodec")),
    "05 Glitch Rain": preset(
        fund=[14], Formants=2, Volume=1.0, Probability=0.55, Formant_Jitter=14, Level_Jitter=0.6,
        Pan_Mode=2, Pan_Jitter=0.5,
        **layer("A", hz=2400, cycles=3, envelope="Expodec"),
        **layer("B", hz=6000, cycles=8, level=0.5, delay=0.37, pulsaret="Noise", envelope="Rexpodec")),
    "06 Buzzy Bass": preset(
        fund=[55], Formants=2, Volume=0.9,
        **layer("A", ratio=6, cycles=2, pulsaret="Saw", envelope="Trapezoid"),
        **layer("B", ratio=13, cycles=2, level=0.35, pulsaret="Square", envelope="Expodec")),
    "07 Burst Machine": preset(
        fund=[22], Formants=2, Volume=1.0, Burst=3, Rest=5,
        **layer("A", hz=1200, cycles=4, envelope="Expodec"),
        **layer("B", hz=4800, cycles=6, level=0.4, envelope="Expodec")),
    "08 Cycle Spinner": preset(
        fund=[16], Formants=1, Volume=1.0, Pan_Mode=1, Sieve=[1, 1, 0, 1, 1, 0, 1, 0],
        **layer("A", hz=1800, cycles=5, pulsaret="Triangle", envelope="Gauss")),
    "09 Glottal": preset(
        fund=[104], Formants=2, Volume=1.2, Rate_Jitter=0.01,
        **layer("A", hz=520, cycles=1, pulsaret=NUPG + "GlottalCycles.wav", envelope="Hann"),
        **layer("B", hz=1600, cycles=2, level=0.4, pulsaret="Sine", envelope=NUPG + "Gaussian48.wav")),
    "10 Dense Cloud": preset(
        fund=[190], Formants=2, Volume=0.8, Rate_Jitter=0.05, Formant_Jitter=3,
        **layer("A", hz=900, cycles=24, envelope="Hann"),
        **layer("B", hz=2700, cycles=30, level=0.5, envelope="Tukey")),
    "11 Metallic Sinc": preset(
        fund=[70], Formants=2, Volume=1.1,
        **layer("A", ratio=7.13, cycles=3, pulsaret="Sinc", envelope="Expodec"),
        **layer("B", ratio=17.4, cycles=5, level=0.45, pulsaret="Sinc", envelope="Expodec")),
    "12 Formant Arpeggio": preset(
        fund=[3], Formants=4, Volume=1.0, Pan_Mode=3,
        **layer("A", hz=600, cycles=10, delay=0.0, envelope="Expodec"),
        **layer("B", hz=900, cycles=10, delay=0.25, level=0.9, envelope="Expodec"),
        **layer("C", hz=1200, cycles=10, delay=0.5, level=0.8, envelope="Expodec"),
        **layer("D", hz=1800, cycles=10, delay=0.75, level=0.7, envelope="Expodec")),
    "13 Noise Hats": preset(
        fund=[9], Formants=1, Volume=0.9, Sieve=euclid(7, 16), Level_Jitter=0.4,
        **layer("A", hz=7500, cycles=40, pulsaret="Noise", envelope="Expodec")),
    "14 nuPG Percussion": preset(
        fund=[5], Formants=2, Volume=1.0, Sieve=euclid(3, 8), Formant_Jitter=2,
        **layer("A", hz=180, cycles=8, pulsaret=NUPG + "FMPercussive.wav", envelope=NUPG + "ExpSharpFast.wav"),
        **layer("B", hz=2600, cycles=6, level=0.35, delay=0.5, pulsaret=NUPG + "Jx3pSaws.wav", envelope="Expodec")),
}


def main():
    for target in TARGETS:
        target = os.path.normpath(target)
        if not os.path.isdir(os.path.dirname(target)):
            continue
        os.makedirs(target, exist_ok=True)
        for name, data in PRESETS.items():
            with open(os.path.join(target, name + ".json"), "w") as f:
                json.dump(data, f, indent=4)
        print(f"{len(PRESETS)} presets -> {target}")


if __name__ == "__main__":
    main()
