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
AnalogFilter : UGen {
    *ar { arg in = 0.0, freq = 1000.0, res = 0.0, mode = 3, type = 0, drive = 1.0, sat = 0.0, feedback = 0.0, mul = 1.0, add = 0.0;
        ^this.multiNew('audio', in, freq, res, mode, type, drive, sat, feedback).madd(mul, add)
    }
    checkInputs { ^this.checkValidInputs }
}
