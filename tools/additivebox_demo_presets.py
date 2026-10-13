#!/usr/bin/env python3
"""Write the "Demo" presets of the AdditiveBox node.

    python3 tools/additivebox_demo_presets.py <output folder>

Each preset only lists what differs from the defaults: AdditiveBox resets every
control to its default before loading a preset (Gate, Trig, Transpose and
Outputs excepted). Keys are the controls' escaped names (spaces -> "_").

Presets whose sound starts on a note (plucks, envelopes that evolve) need a
gate: press Retrig in the window, or patch Gate / Trig.
"""
import json, math, os, sys

# Index values of the dropdowns (scAdditiveBox.h)
FLAT, SAW, SQUARE, TRIANGLE, SINE, PULSE, ORGAN, HOLLOW, RANDOM, DRAWN = range(10)
F_OFF, F_LP, F_BP, F_HP, F_NOTCH, F_COMB, F_DRAWN = range(7)
F2_OFF, F2_VOWEL, F2_DRAWN = range(3)
SINE_LFO, TRI_LFO, SAW_LFO, SQUARE_LFO, SH_LFO = range(5)
RANDOM_PAN, SPECTRUM_PAN, ALTERNATE_PAN = range(3)


def drawn_eq(points):
    """33 points (dB) over 20 Hz .. 20 kHz from [(hz, dB), ...], log-interpolated."""
    out = []
    for i in range(33):
        hz = 20.0 * 1000.0 ** (i / 32.0)
        if hz <= points[0][0]:
            out.append(points[0][1]); continue
        if hz >= points[-1][0]:
            out.append(points[-1][1]); continue
        for (f0, d0), (f1, d1) in zip(points, points[1:]):
            if f0 <= hz <= f1:
                t = math.log(hz / f0) / math.log(f1 / f0)
                out.append(round(d0 + (d1 - d0) * t, 2)); break
    return out


def table(levels):
    """64 harmonics from a list (the rest silent)."""
    return [float(x) for x in levels] + [0.0] * (64 - len(levels))


PRESETS = {
    "Demo 01 Pluck Harp": {
        "Voices": 3, "Pitch": [55, 62, 67],
        "Wave_A": [SAW], "Tilt": [0], "Partials": 48,
        "Filter": [F_LP], "Cutoff": [55], "Keytrack": [1], "Slope": [24], "Res": [0.25],
        "Env2_Attack": 0.001, "Env2_Decay": 0.4, "Env2_Sustain": 0.0, "Env2_Release": 0.3, "Env2_Cutoff": 0.6,
        "Attack": [0.002], "Decay": [2.0], "Sustain": [0], "Release": [1.5], "Env_Tilt": [0.7], "Env_Rand": [0.3],
        "Spread": [0.6], "Pan_Mode": [SPECTRUM_PAN],
        "Reverb_On": True, "Rev_Mix": 0.25, "Rev_Decay": 3.0,
    },
    "Demo 02 Glass Pad": {
        "Voices": 3, "Pitch": [48, 55, 64], "Levels": [0.45],
        "Wave_A": [ORGAN], "Wave_B": [TRIANGLE], "Tilt": [0], "Partials": 32, "Morph": [0.5],
        "LFO1_Shape": SINE_LFO, "LFO1_Rate": 0.07, "LFO1_Morph": 0.5,
        "Unison": 4, "Unison_Detune": 14, "Unison_Spread": 0.8, "Unison_Blend": 0.8,
        "Attack": [1.8], "Decay": [1.0], "Sustain": [1.0], "Release": [4.0],
        "Partial_Vib": [0.06], "Partial_Vib_Rate": [0.7], "Shimmer": [0.12], "Shimmer_Rate": [3.0],
        "Reverb_On": True, "Rev_Mix": 0.4, "Rev_Size": 45, "Rev_Decay": 7.0,
    },
    "Demo 03 Vowel Choir": {
        "Voices": 4, "Pitch": [45, 52, 57, 61], "Levels": [0.5],
        "Wave_A": [SAW], "Tilt": [-1], "Partials": 64,
        "Filter_2": [F2_VOWEL], "Vowel": [0, 1, 3, 4], "Formant_Width": [2.5],
        "Unison": 3, "Unison_Detune": 18, "Unison_Spread": 0.6,
        "Vibrato": [0.12], "Vib_Rate": [5.2], "Partial_Vib": [0.04],
        "Noise": [0.12], "Noise_Rate": [300], "Noise_Tilt": [0.6],
        "Attack": [0.5], "Release": [2.5],
        "Reverb_On": True, "Rev_Mix": 0.3,
    },
    "Demo 04 Phaser Drift": {
        "Voices": 2, "Pitch": [36, 43],
        "Wave_A": [SAW], "Tilt": [-1], "Partials": 128,
        "Sweep": [0.85], "Sweep_Waves": [3], "Sweep_Rate": [0.12], "Sweep_Sharp": [0.55], "Sweep_Mode": [1],
        "Drift": [0.1], "Attack": [2.0], "Release": [4.0],
        "Echo_On": True, "Echo_Delay": 0.42, "Echo_Feed": 0.4, "Echo_Mix": 0.25,
    },
    "Demo 05 Spectral Wash": {
        "Voices": 3, "Pitch": [50, 57, 62],
        "Wave_A": [SQUARE], "Wave_B": [RANDOM], "Tilt": [-2], "Partials": 64, "Morph": [0.2],
        "Harm_Blur": [0.5], "Time_Blur": [2.5],
        "Density": [0.55], "Mask_Glide": [1.5], "Shimmer": [0.3], "Shimmer_Rate": [2.0],
        "LFO2_Shape": SH_LFO, "LFO2_Rate": 0.5, "LFO2_Morph": 0.8,
        "Attack": [1.0], "Release": [6.0],
        "Reverb_On": True, "Rev_Mix": 0.55, "Rev_Decay": 10.0, "Rev_Size": 55,
    },
    "Demo 06 Breath Flute": {
        "Voices": 1, "Pitch": [74], "Levels": [0.6],
        "Wave_A": [TRIANGLE], "Tilt": [-2], "Partials": 16,
        "Noise": [0.35], "Noise_Rate": [450], "Noise_Tilt": [0.7],
        "Vibrato": [0.1], "Vib_Rate": [5.0],
        "Filter": [F_LP], "Cutoff": [84], "Keytrack": [1], "Slope": [12],
        "Attack": [0.08], "Decay": [0.3], "Sustain": [0.85], "Release": [0.35],
        "Reverb_On": True, "Rev_Mix": 0.2,
    },
    "Demo 07 Stretched Bell": {
        "Voices": 2, "Pitch": [60, 67],
        "Wave_A": [FLAT], "Tilt": [-3], "Partials": 40, "Stretch": [1.05], "Inharm": [0.0012],
        "Attack": [0.001], "Decay": [5.0], "Sustain": [0], "Release": [5.0], "Env_Tilt": [0.8], "Env_Rand": [0.4],
        "Partial_Vib": [0.02],
        "Reverb_On": True, "Rev_Mix": 0.3, "Rev_Decay": 5.0,
    },
    "Demo 08 Super Unison": {
        "Voices": 3, "Pitch": [48, 55, 60], "Levels": [0.45],
        "Wave_A": [SAW], "Tilt": [0], "Partials": 64,
        "Unison": 7, "Unison_Detune": 35, "Unison_Spread": 1.0, "Unison_Blend": 0.9,
        "Filter": [F_LP], "Cutoff": [98], "Slope": [12],
        "Attack": [0.02], "Release": [0.6],
        "Reverb_On": True, "Rev_Mix": 0.25,
    },
    "Demo 09 Fifths Harmonizer": {
        "Voices": 1, "Pitch": [43],
        "Wave_A": [PULSE], "Tilt": [-1], "Partials": 48,
        "Unison": 3, "Unison_Interval": 7, "Unison_Blend": 0.7, "Unison_Detune": 4, "Unison_Spread": 0.7,
        "Filter": [F_BP], "Cutoff": [74], "Keytrack": [1], "Slope": [18], "Res": [0.45],
        "LFO1_Shape": TRI_LFO, "LFO1_Rate": 0.2, "LFO1_Cutoff": 0.3,
        "Echo_On": True, "Echo_Delay": 0.375, "Echo_Feed": 0.45, "Echo_Mix": 0.3,
    },
    "Demo 10 Wobble Bass": {
        "Voices": 1, "Pitch": [31], "Levels": [0.6],
        "Wave_A": [SAW], "Tilt": [0], "Partials": 64,
        "Filter": [F_LP], "Cutoff": [50], "Keytrack": [1], "Slope": [36], "Res": [0.6], "Safe_Bass": [1.0],
        "LFO1_Shape": SINE_LFO, "LFO1_Rate": 3.0, "LFO1_Cutoff": 0.45,
        "Unison": 2, "Unison_Detune": 8, "Unison_Spread": 0.3,
        "Attack": [0.005], "Release": [0.2],
    },
    "Demo 11 Glitter Mask": {
        "Voices": 2, "Pitch": [72, 79],
        "Wave_A": [FLAT], "Tilt": [-2], "Partials": 128,
        "Density": [0.18], "Mask_Glide": [0.3], "Shimmer": [0.6], "Shimmer_Rate": [9.0],
        "Drift": [0.2], "Drift_Rate": [0.3],
        "LFO2_Shape": SH_LFO, "LFO2_Rate": 5.0, "LFO2_Pan": 0.6,
        "Attack": [0.2], "Release": [3.0],
        "Reverb_On": True, "Rev_Mix": 0.45, "Rev_Decay": 6.0,
        "Echo_On": True, "Echo_Mix": 0.3, "Echo_Delay": 0.33,
    },
    "Demo 12 Morph Evolve": {
        "Voices": 2, "Pitch": [41, 48],
        "Wave_A": [SAW], "Wave_B": [HOLLOW], "Tilt": [-1], "Partials": 64,
        "Env2_Attack": 4.0, "Env2_Decay": 4.0, "Env2_Sustain": 0.3, "Env2_Release": 3.0,
        "Env2_Morph": 1.0, "Env2_Cutoff": 0.25,
        "Filter": [F_LP], "Cutoff": [70], "Keytrack": [1], "Slope": [18],
        "Attack": [1.0], "Release": [3.0],
        "Reverb_On": True, "Rev_Mix": 0.3,
    },
    "Demo 13 Flanger Drone": {
        "Voices": 2, "Pitch": [36, 43],
        "Wave_A": [SAW], "Tilt": [-1.5], "Partials": 128,
        "Filter": [F_COMB], "Cutoff": [70], "Slope": [32], "Res": [0.9],
        "LFO1_Shape": TRI_LFO, "LFO1_Rate": 0.06, "LFO1_Cutoff": 0.25,
        "Attack": [2.0], "Release": [4.0],
        "Reverb_On": True, "Rev_Mix": 0.25,
    },
    "Demo 14 Drawn Organ": {
        "Voices": 4, "Pitch": [48, 52, 55, 59], "Levels": [0.45],
        "Wave_A": [DRAWN], "Wave_B": [DRAWN], "Tilt": [0], "Partials": 32, "Morph": [0.5],
        "Harmonics_A": table([1, .7, .5, 0, .4, 0, .3, .25, 0, 0, 0, .2, 0, 0, 0, .12]),
        "Harmonics_B": table([.3, 1, .2, .8, .1, .6, .1, .45, 0, .3, 0, .2]),
        "LFO1_Shape": SINE_LFO, "LFO1_Rate": 0.6, "LFO1_Morph": 0.45,
        "Vibrato": [0.07], "Vib_Rate": [6.5],
        "Unison": 2, "Unison_Detune": 6, "Unison_Spread": 0.6,
        "Attack": [0.01], "Release": [0.3],
        "Reverb_On": True, "Rev_Mix": 0.2,
    },
    "Demo 15 Drawn EQ Radio": {
        "Voices": 2, "Pitch": [57, 64],
        "Wave_A": [SAW], "Tilt": [0], "Partials": 96,
        "Filter_2": [F2_DRAWN],
        "EQ_Shape": drawn_eq([(20, -40), (300, -36), (500, -2), (1000, 6), (2500, 0), (3500, -30), (20000, -48)]),
        "Noise": [0.1], "Noise_Rate": [600], "Drift": [0.05],
        "Echo_On": True, "Echo_Mix": 0.2,
    },
    "Demo 16 Riser": {
        "Voices": 1, "Pitch": [36],
        "Wave_A": [SAW], "Tilt": [-1], "Partials": 128,
        "Env2_Attack": 8.0, "Env2_Decay": 0.0, "Env2_Sustain": 1.0, "Env2_Release": 1.0,
        "Env2_Pitch": 1.0, "Env2_Cutoff": 0.8, "Env2_Noise": 1.0,
        "Filter": [F_LP], "Cutoff": [40], "Slope": [24], "Res": [0.3],
        "Noise": [0.25], "Noise_Rate": [500],
        "Sweep": [0.6], "Sweep_Waves": [2], "Sweep_Rate": [0.5],
        "Attack": [0.5], "Release": [2.0],
        "Reverb_On": True, "Rev_Mix": 0.35,
    },
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, preset in PRESETS.items():
        with open(os.path.join(out, name + ".json"), "w") as f:
            json.dump(preset, f, indent=4)
        print("wrote", name)


if __name__ == "__main__":
    main()
