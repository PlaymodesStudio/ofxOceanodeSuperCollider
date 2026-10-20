// Reference resonator before independent vibrato was added. Test-only.
(1..32).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };
    var modIn = { |name| In.kr(NamedControl.kr(name, 0), n) };
    var base = { In.kr(\note.kr(0) + n, n) };
    SynthDef(("PCCombR" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        // Smooth pitch targets at audio rate so delay retuning has no GUI/control-block steps.
        var cnote = Lag.ar(K2A.ar(kv.(\combpitch, 0) + kv.(\combtranspose, 0) + (base.() * (1 - (\combmode.kr(0) > 0.5)))), 0.02) +
            K2A.ar((({ Rand(-1, 1) } ! n) * kv.(\combdetune, 0)) + modIn.(\mcpitch));
        var fc = cnote.clip(0, 135).midicps.clip(20, 20000);
        var fb = (kv.(\combfeed, 0.7) + modIn.(\mcfb)).clip(-1.2, 1.2);
        var level = \satlevel.kr(48, 0.03).clip(0, 48).dbamp;
        var mix = (kv.(\combmix, 1) + modIn.(\mcmix)).clip(0, 1);
        var wet = Sanitize.ar(ReaktorComb.ar(sig, fc, fb, level, mix, 1));
        Out.ar(\out.kr(0), LinXFade2.ar(sig, wet, \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);
};
