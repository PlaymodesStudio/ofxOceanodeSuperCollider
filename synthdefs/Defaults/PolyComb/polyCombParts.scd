// polyCombParts.scd — PolyComb's voice, as small synths (N voices, 1..16),
// so the node runs only the parts that are on. With the shared modulators
// (synthdefs/Defaults/Modulation) and the effects (MixerPro's inserts):
//
//   ModGate -> ModLfo / ModEnv (sources) -> ModRoute (one per matrix cell)
//   PCPitch -> [PCFm] -> PCOsc<Type> (one per oscillator in use, summed)
//   -> [PCFilter] -> [PCComb] -> [PCRing] -> [PCFilter (post)] -> [PCShaper]
//   -> PCAmp (amp envelope, velocity) -> PolyCombSpread<C> (speakers)
//   -> [mixerProEQ / Echo / Rvb <C>]
//
// Modulation arrives on control buses, one per target, each 16 channels (N
// used): \mpitch \mamt \mratio \mcut \mres \mcpitch \mcfb \mcmix \mrmix
// \mdrive \mpw \mamp \mpan. Inserts take \on (crossfade to dry; the node
// pauses them once faded).
(
var root, dir, filter;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "PolyComb";
File.mkdir(root); File.mkdir(dir);

// LP / HP / BP / notch; resonance crossfades into the 4-pole resonant versions
filter = { |sig, type, note, res|
    var fr = note.clip(0, 135).midicps.clip(20, 20000);
    var q = res.linlin(0, 1, 1.41421356, 0.02), rq = res.linlin(0, 1, 1, 0.02);
    var lp = XFade2.ar(LPF.ar(sig, fr), BLowPass4.ar(sig, fr, q), res * 2 - 1);
    var hp = XFade2.ar(HPF.ar(sig, fr), BHiPass4.ar(sig, fr, q), res * 2 - 1);
    var t = type.clip(0, 3);
    (lp * (t < 0.5)) + (hp * ((t >= 0.5) * (t < 1.5))) +
    (BPF.ar(sig, fr, rq) * ((t >= 1.5) * (t < 2.5))) + (BRF.ar(sig, fr, rq) * (t >= 2.5))
};

(1..32).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };
    var modIn = { |name| In.kr(NamedControl.kr(name, 0), n) };
    var insert = { |dry, wet| LinXFade2.ar(dry, wet, \on.kr(1, 0.03) * 2 - 1) };
    var note = { K2A.ar(In.kr(\note.kr(0), n)) };            // the note, with vibrato and pitch modulation
    var base = { In.kr(\note.kr(0) + n, n) };        // the note before them (key tracking, the comb)

    // The note: glide, transpose, a random detune per voice, vibrato (a rate per voice), modulation
    SynthDef(("PCPitch" ++ n).asSymbol, {
        // or an audio cable: its \pitch_k values (<= 0: as many as the voices) over the voices,
        // the rest the last one, or (\pitch_expand) them all again 12 semitones up each time
        var cable = A2K.kr(\pitch_ar.ar(60 ! n)).asArray;
        var k = \pitch_k.kr(-1);
        var kk = Select.kr(k < 1, [k.min(n), n]);
        var ex = \pitch_expand.kr(0) > 0.5;
        var spread = n.collect { |i|
            var idx = Select.kr(ex, [i.min(kk - 1), i.mod(kk)]);
            Select.kr(idx, cable) + (ex * (i / kk).floor * 12)
        };
        var p = Select.kr(\pitch_sel.kr(0) > 0.5, [kv.(\pitch, 60), spread]);
        // A short minimum lag removes GUI/map stepping; longer musical glide still applies.
        var d = Lag.kr(kv.(\transpose, 0), 0.02) + (({ Rand(-1, 1) } ! n) * kv.(\detune, 0));
        var b = Lag.kr(p, \glide.kr(0).max(0.02)) + d;
        // each voice its own vibrato rate: Rate * (1 ± a random part up to \vibrand)
        var rate = \vibrate.kr(5) * (1 + (({ Rand(-1, 1) } ! n) * \vibrand.kr(0).clip(0, 1)));
        var v = SinOsc.kr(rate, { Rand(0, 2pi) } ! n) * \vibamt.kr(0);
        // the note, the note before vibrato / modulation, and that before the glide (the resonators glide on their own)
        Out.kr(\out.kr(0), (b + v + modIn.(\mpitch)) ++ b ++ (p + d));
    }).writeDefFile(dir);

    // FM: a sine modulator (with feedback) at a ratio of the note -> Hz to add
    SynthDef(("PCFm" ++ n).asSymbol, {
        var freq = note.().clip(0, 135).midicps;
        var amt = (kv.(\fmamt, 0) + modIn.(\mamt)).clip(0, 2);
        var ratio = (kv.(\fmratio, 1) + modIn.(\mratio)).clip(0.0625, 32);
        Out.ar(\out.kr(0), SinOscFB.ar(freq * ratio, \fmfb.kr(0).clip(0, 1.5)) * freq * amt * 8);
    }).writeDefFile(dir);

    // One synth per oscillator type, all adding into the oscillator bus
    [
        // Saw / Pulse: band-limited, but those break under FM (the frequency
        // goes below 0 and their integrators drift); with FM on, the plain
        // ramp / pulse instead (run backwards right), without DC
        [\Saw,   { |f, fr| Select.ar(\fmon.kr(0) > 0.5, [Saw.ar(f), LeakDC.ar(LFSaw.ar(f))]) }],
        [\Tri,   { |f, fr| LFTri.ar(f) }],
        [\Pulse, { |f, fr|
            var w = (kv.(\pw, 0.5) + modIn.(\mpw)).clip(0.01, 0.99);
            Select.ar(\fmon.kr(0) > 0.5, [Pulse.ar(f, w), LeakDC.ar(LFPulse.ar(f, 0, w) * 2 - 1)]) }],
        [\Sine,  { |f, fr| SinOsc.ar(f) }],
        [\SSaw,  { |f, fr|
            var d = f * {
                var x = kv.(\ssdetune, 0.5).clip(0, 1);
                (10028.7312891634 * x.pow(11)) - (50818.8652045924 * x.pow(10)) + (111363.4808729368 * x.pow(9)) -
                (138150.6761080548 * x.pow(8)) + (106649.6679158292 * x.pow(7)) - (53046.9642751875 * x.pow(6)) +
                (17019.9518580080 * x.pow(5)) - (3425.0836591318 * x.pow(4)) + (404.2703938388 * x.pow(3)) -
                (24.1878824391 * x.pow(2)) + (0.6717417634 * x) + 0.0030115596
            }.value;
            HPF.ar([0, -0.11002313, -0.06288439, -0.01952356, 0.01991221, 0.06216538, 0.10745242].collect { |k|
                LFSaw.ar(f + (d * k), { Rand(0, 2) } ! n) }.sum * (1 / 7), fr) }],
        [\White, { |f, fr| WhiteNoise.ar(1 ! n) }],
        [\Pink,  { |f, fr| PinkNoise.ar(1 ! n) }],
        // Dust: a short burst per grain (a single-sample click had almost no energy), ±;
        // \dustoct: the grain rate this many octaves below the note (0 .. -10)
        [\Dust,  { |f, fr| Decay2.ar(Dust2.ar((fr * (2 ** \dustoct.kr(0, 0.03).clip(-10, 0))).clip(0.001, 10000)), 0.0002, 0.004) * 3 }],
        [\Imp,   { |f, fr| Impulse.ar(f) }],
    ].do { |o|
        SynthDef(("PCOsc" ++ o[0] ++ n).asSymbol, {
            var fr = note.().clip(0, 135).midicps;
            var f = fr + (In.ar(\fm.kr(0), n) * \fmon.kr(0));
            Out.ar(\out.kr(0), o[1].(f, fr) * kv.(\level, 1) * kv.(\oscamp, 0.5));
        }).writeDefFile(dir);
    };

    // A filter (the main one and the post filter)
    SynthDef(("PCFilter" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var cut = kv.(\cutoff, 100) + (\keytrack.kr(0) * (base.() - 60)) + modIn.(\mcut);
        Out.ar(\out.kr(0), insert.(sig, filter.(sig, \type.kr(0), cut, (kv.(\res, 0.3) + modIn.(\mres)).clip(0, 1))));
    }).writeDefFile(dir);

    // The comb resonator: follows the note (+ an interval) or a fixed note;
    // CombC for an exact pitch, negative feedback = odd harmonics
    SynthDef(("PCComb" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var cnote = kv.(\combpitch, 0) + kv.(\combtranspose, 0) + (base.() * (1 - (\combmode.kr(0) > 0.5))) +
            (({ Rand(-1, 1) } ! n) * kv.(\combdetune, 0)) + modIn.(\mcpitch);
        var delay = cnote.clip(0, 127).midicps.clip(20, 10000).reciprocal;
        var fb = (kv.(\combfeed, 0.7) + modIn.(\mcfb)).clip(-0.999, 0.999);
        var comb = CombC.ar(LPF.ar(sig, \combtone.kr(1).linexp(0, 1, 300, 20000)), 0.05, delay,
            delay * (0.001.log / fb.abs.max(0.0001).log) * fb.sign);
        comb = (LeakDC.ar(comb) * (1 - (fb.abs * 0.9))).tanh;
        Out.ar(\out.kr(0), insert.(sig, XFade2.ar(sig, comb, (kv.(\combmix, 1) + modIn.(\mcmix)).clip(0, 1) * 2 - 1)));
    }).writeDefFile(dir);

    SynthDef(("PCRing" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var ring = sig * SinOsc.ar((kv.(\ringpitch, 72) + kv.(\ringtranspose, 0) +
            (({ Rand(-1, 1) } ! n) * kv.(\ringdetune, 0))).clip(0, 135).midicps);
        Out.ar(\out.kr(0), insert.(sig, XFade2.ar(sig, ring, (kv.(\ringmix, 1) + modIn.(\mrmix)).clip(0, 1) * 2 - 1)));
    }).writeDefFile(dir);

    // The sine shaper (sineshaper.scd)
    SynthDef(("PCShaper" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var shaped = (sig * (kv.(\drive, 1) + modIn.(\mdrive)).clip(0, 30)).sin;
        Out.ar(\out.kr(0), insert.(sig, XFade2.ar(sig, shaped, kv.(\shapermix, 1).clip(0, 1) * 2 - 1)));
    }).writeDefFile(dir);

    // The VCA: the amp envelope (audio), velocity, amp modulation, level
    SynthDef(("PCAmp" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var vel = In.kr(\vel.kr(0), n);
        // the gate's level (held from its rising edge, 1 while droning); Vel Sens
        // blends in the velocity (\vel = level x Velocity)
        var lev = In.ar(\gate.kr(0), n);
        var vs = \velsens.kr(0.5).clip(0, 1);
        // Like vibrato, each voice has a random rate offset and initial phase.
        // Depth 0 is unity; depth 1 sweeps the amplitude from silence to unity.
        var rate = \tremrate.kr(5) * (1 + (({ Rand(-1, 1) } ! n) * \tremrand.kr(0).clip(0, 1)));
        var trem = 1 - ((SinOsc.kr(rate, { Rand(0, 2pi) } ! n) * 0.5 + 0.5) * \tremdepth.kr(0, 0.03).clip(0, 1));
        var amp = In.ar(\env.kr(0), n) * ((Gate.ar(lev, lev > 0) * (1 - vs)) + K2A.ar(vel * vs)) *
            K2A.ar((1 + modIn.(\mamp)).clip(0, 2) * trem);
        Out.ar(\out.kr(0), Sanitize.ar(LeakDC.ar(sig * amp) * \level.kr(0.8, 0.05)));
    }).writeDefFile(dir);
};

// PolyCombSpread<C>: the voices (\voices of the 16 on \in) onto C speakers,
// voice i at \pan[i] (0..1) + \mpan's modulation. 2 speakers: left..right;
// more: around the ring (0 and 1 both speaker 1) or, \line 1, along a line.
// Each voice's final position -> \vis (control, 16), for the window.
(1..16).do { |c|
    SynthDef(("PolyCombSpread" ++ c).asSymbol, {
        var v = \voices.kr(1).clip(1, 16);
        var sig = In.ar(\in.kr(0), 16).asArray;
        var pos = (NamedControl.kr(\pan, 0.5 ! 16) + Sanitize.kr(In.kr(\mpan.kr(0), 16))).asArray.collect { |p| p.lag(0.02) };
        var line = \line.kr(0) > 0.5;
        var comp = (v / c).max(1).sqrt.reciprocal;
        var out = 16.collect { |i|
            var x = sig[i] * (i < v);
            var p = pos[i];
            if(c == 1) { [x] } {
                if(c == 2) { Pan2.ar(x, (p.clip(0, 1) * 2) - 1) } {
                    PanAz.ar(c, x, Select.kr(line, [p.wrap(0, 1) * 2, p.clip(0, 1) * 2 * (c - 1) / c]), 1, 2, 0)
                }
            }
        }.sum;
        Out.kr(\vis.kr(0), pos);
        Out.ar(\out.kr(0), out * comp);
    }).writeDefFile(dir);
};
"PolyComb parts written to ".post; dir.postln;
)
