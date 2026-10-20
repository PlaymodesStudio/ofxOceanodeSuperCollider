// AnalogBox's UGens: see OceanodeAnalog.cpp.
// MorphOsc: the LFOs' Width / Skew / Round shape as a band-limited oscillator
// (2x oversampled, polyBLEP / polyBLAMP, halfband), with phase modulation and hard sync.
MorphOsc : UGen {
    *ar { arg freq = 440.0, pm = 0.0, skew = 0.0, round = 0.5, pw = 0.5, centered = 1, syncFreq = 0.0, mul = 1.0, add = 0.0;
        ^this.multiNew('audio', freq, pm, skew, round, pw, centered, syncFreq).madd(mul, add)
    }
}
// AnalogFilter: ZDF SVF / ladder, 2 or 4 poles (mode 0 SVF2, 1 SVF4, 2 Ladder2, 3 Ladder4),
// type 0 LP, 1 BP, 2 HP, 3 notch; drive (input gain into tanh), sat (tanh in the loop), feedback.
// mode 4 Comb: one period of freq, res its gain; type 0 feedback +, 1 feedback -, 2 feedforward +,
// 3 feedforward -; feedback damps the loop.
AnalogFilter : UGen {
    *ar { arg in = 0.0, freq = 1000.0, res = 0.0, mode = 3, type = 0, drive = 1.0, sat = 0.0, feedback = 0.0, mul = 1.0, add = 0.0;
        ^this.multiNew('audio', in, freq, res, mode, type, drive, sat, feedback).madd(mul, add)
    }
    checkInputs { ^this.checkValidInputs }
}
// ReaktorComb: the original PolyComb's comb: one period of freq, feedback fb through a
// parabolic saturator at level (full saturation there; fb may exceed 1), out: dry and
// the delayed signal crossfaded by mix, through the same saturator at outLevel.
ReaktorComb : UGen {
    *ar { arg in = 0.0, freq = 440.0, fb = 0.9, level = 1.0, mix = 1.0, outLevel = 1.0, mul = 1.0, add = 0.0;
        ^this.multiNew('audio', in, freq, fb, level, mix, outLevel).madd(mul, add)
    }
    checkInputs { ^this.checkValidInputs }
}

// ReaktorMulti4: the original PolyComb's filters (Reaktor's Multi/LP 4-Pole): a 4-pole
// ladder, res 0..1 (1: self-oscillation), its outputs mixed from the stages; type as
// PolyComb's: 0 LP4, 1 HP2, 2 BP2, 3 notch, 4 LP2, 5 LP4, 6 HP2, 7 HP4, 8 BP2, 9 BP4, 10 BL4, 11 notch
ReaktorMulti4 : UGen {
    *ar { arg in = 0.0, freq = 1000.0, res = 0.0, type = 0, mul = 1.0, add = 0.0;
        ^this.multiNew('audio', in, freq, res, type).madd(mul, add)
    }
    checkInputs { ^this.checkValidInputs }
}
