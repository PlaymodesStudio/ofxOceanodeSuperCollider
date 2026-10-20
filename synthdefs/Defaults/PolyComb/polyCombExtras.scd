// PolyComb extras, beside the original parts (polyCombParts.scd stays as it was):
//
//   PCSpreadWide<C>  PolyCombSpread<C> for more than 16 voices: the voices
//                    (\voices of the 32 on \in) onto C speakers, the same
//                    placement and level compensation. Up to 16 voices the
//                    node keeps using PolyCombSpread<C>.
//   PCDelta<N>       the original PolyComb's DELTA source: how much a mix of
//                    two LFOs (\a, \b: control buses, N) moved over the last
//                    \window ms, |x - x(window ago)|, smoothed by a one-pole
//                    lowpass at 1000 / \window Hz, x \gain / 4 -> \out
//                    (control, N): a modulation source, 0 and up.
(
var root, dir;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "PolyComb";
File.mkdir(root); File.mkdir(dir);

(1..16).do { |c|
    SynthDef(("PCSpreadWide" ++ c).asSymbol, {
        var v = \voices.kr(1).clip(1, 32);
        var sig = In.ar(\in.kr(0), 32).asArray;
        var pos = (NamedControl.kr(\pan, 0.5 ! 32) + Sanitize.kr(In.kr(\mpan.kr(0), 32))).asArray.collect { |p| p.lag(0.02) };
        var line = \line.kr(0) > 0.5;
        var comp = (v / c).max(1).sqrt.reciprocal;
        var out = 32.collect { |i|
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

(1..32).do { |n|
    SynthDef(("PCDelta" ++ n).asSymbol, {
        var x = (In.kr(\a.kr(0), n) * \mixa.kr(0)) + (In.kr(\b.kr(0), n) * \mixb.kr(1));
        var win = \window.kr(1000).clip(1, 1000) / 1000;
        var moved = (x - DelayN.kr(x, 1, win)).abs;
        var smooth = OnePole.kr(moved, exp(-2pi * win.reciprocal / ControlRate.ir));
        Out.kr(\out.kr(0), Sanitize.kr(smooth * \gain.kr(3) * 0.25));
    }).writeDefFile(dir);
};
// PCEchoR<C>: the original PolyComb's delay (its Core cell): a line per
// speaker at \delay x (1 +- \spread) (alternating), each through a resonant
// peak-normalized 2-pole lowpass (\hi Hz, \res: -inf..0.98) and a one-pole
// highpass (\lo Hz); the lines' mean x \feed back into every line (the
// original: 0.5 x Feed x (L + R)); out: dry and the filtered lines crossfaded
// by \mix squared. \on: dry, the tail rings out.
(1..16).do { |c|
    SynthDef(("PCEchoR" ++ c).asSymbol, {
        var dry = In.ar(\in.kr(0), c).asArray;
        var on = \on.kr(1, 0.03);
        var t = \delay.kr(0.3, 0.05).clip(0.005, 4), lr = \spread.kr(0).clip(-0.33, 0.33);
        var times = if(c == 1, { [t] }, { c.collect { |i| t * (1 + (lr * (if(i.even, 1, -1)))) } });
        var fb = LocalIn.ar(1);
        var lines = c.collect { |i| DelayC.ar((dry[i] * on) + fb, 5.5, (times[i] - ControlDur.ir).max(0)) };
        var rq = (2 * (1 - \res.kr(0).clip(-4, 0.98).lag(0.03))).clip(0.04, 4);
        var hi = \hi.kr(8000).clip(20, SampleRate.ir * 0.45).lag(0.03);
        var lo = \lo.kr(40).clip(1, SampleRate.ir * 0.45).lag(0.03);
        // RLPF uses tan(pi * hi * rq / sr): rq up to 4 made that pole
        // mapping unstable within the Hi slider's range. RBJ's bilinear
        // lowpass is stable for every positive rq and cutoff below Nyquist.
        // Its resonant peak must also be <= 1 inside a feedback loop:
        // max |H| = 1/(rq * sqrt(1-rq^2/4)) when rq < sqrt(2), else 1.
        var peakGain = Select.kr(rq < 2.sqrt, [1, rq * (1 - (rq.squared * 0.25)).max(0).sqrt]);
        var wet = lines.collect { |x|
            var lp = BLowPass.ar(x, hi, rq) * peakGain;
            lp - OnePole.ar(lp, exp(-2pi * lo * SampleDur.ir))
        };
        var m = \mix.kr(0.3).clip(0, 1).squared * on;
        LocalOut.ar(Sanitize.ar(wet.sum / c * \feed.kr(0.5).clip(0, 0.98)));
        Out.ar(\out.kr(0), c.collect { |i| (dry[i] * (1 - m)) + (wet[i] * m) });
    }).writeDefFile(dir);
};
// PCCombR<N>: the original PolyComb's comb (ReaktorComb, OceanodeAnalog): the
// same note as PCComb (follows the note or fixed, + transpose, detune, \mcpitch),
// feedback up to 1.2 (the loop's saturator at \satlevel dB holds it), mix inside
// (dry / delayed, then the output saturator). \on: dry.
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
// PCFilterR<N>: the original PolyComb's filters (ReaktorMulti4, OceanodeAnalog: a
// 4-pole ladder, its outputs mixed from the stages). \type: the node's filter type
// (0 .. 11, as its menu); the controls as PCFilter's.
(1..32).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };
    var modIn = { |name| In.kr(NamedControl.kr(name, 0), n) };
    var base = { In.kr(\note.kr(0) + n, n) };
    SynthDef(("PCFilterR" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var cut = kv.(\cutoff, 100) + (\keytrack.kr(0) * (base.() - 60)) + modIn.(\mcut);
        var fc = cut.clip(0, 135).midicps.clip(20, 20000);
        var wet = Sanitize.ar(ReaktorMulti4.ar(sig, fc, (kv.(\res, 0.3) + modIn.(\mres)).clip(0, 1), \type.kr(0)));
        Out.ar(\out.kr(0), LinXFade2.ar(sig, wet, \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);
};
// PCRingR<N>: the original PolyComb's ring modulator (its RingMod macro): a sine at
// the voice's note (+ a random detune per voice, \ringdetune st) x \ringratio, made
// unipolar and raised to a power set by \ringwidth (-1: 0, flat; 0: 1, the sine;
// 1: 8, narrow peaks), bipolar again; the voice multiplied by 1 - a + a x that
// (a: \ringmix, the amount, + its modulation).
(1..32).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };
    var modIn = { |name| In.kr(NamedControl.kr(name, 0), n) };
    var note = { In.kr(\note.kr(0), n) };
    SynthDef(("PCRingR" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var pitch = Select.kr(\ringmode.kr(0), [note.(), kv.(\ringpitch, 72)])
            + kv.(\ringtranspose, 0) + (({ Rand(-1, 1) } ! n) * kv.(\ringdetune, 0));
        var freq = (pitch.clip(0, 135).midicps * kv.(\ringratio, 1)).clip(0, SampleRate.ir * 0.45);
        var width = kv.(\ringwidth, 0).clip(-1, 1);
        var k = Select.kr(width > 0, [width + 1, 1 + (7 * width)]);
        var m = (((SinOsc.ar(freq) + 1) * 0.5).max(1.0e-9) ** k * 2) - 1;
        var a = (kv.(\ringmix, 1) + modIn.(\mrmix)).clip(0, 1);
        Out.ar(\out.kr(0), LinXFade2.ar(sig, sig * ((1 - a) + (a * m)), \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);
};
"PolyComb extras written to ".post; dir.postln;
)
