// PhysicalBox: a physical-modelling voice (PhysicalVoice, ugens/OceanodePhysical)
// per voice, around it the shared modular parts:
//
//   ModGate<N> -> ModLfo / ModEnv / ModRoute (scModulation) -> PhysicalBoxVoice
//   (one per voice, two pickups) -> PhysicalBoxSpread<C> (speakers)
//   -> [mixerProSat / EQ / Echo / Rvb <C>] -> [PhysicalBoxSpace<C>]
//
//   PhysicalBoxVoice   \gatebus: the voice's gate (ModGate: gate x level; the
//                      level is the velocity). \pitch (MIDI) or \pitch_ar with
//                      \pitch_sel 1. Every PhysicalVoice input by name (\material,
//                      \bow, ...), and the modulation (per-voice control buses,
//                      the node sets bus + voice): \mpitch (st) \mmat \mexc
//                      (excite and listen) \mpos \mhard \mpress \mspeed \mbreath
//                      \mscrape \mbright (A and B) \mdecay (octaves of decay time)
//                      \mshape \mcouple \mcascade \mamp (\vca 1: an envelope is the
//                      VCA). \in: an audio bus into the Ext exciter. -> \out
//                      (audio, 2: the pickups); \vis + voice * 4: level, gate, note.
//   PhysicalBoxSpread<C>  16 voices x 2 pickups on \in (32) onto C speakers: voice
//                      i at \pan[i] + \mpan, its pickups \width apart (0..1);
//                      the PolyCombSpread placement and level compensation.
//   PhysicalBoxSpace<C>   MiVerb (Mutable Instruments' Clouds / Rings reverb) on
//                      the C channels (pairs for C > 2): \mix \time \damp \hp
//                      \diff \freeze, \on crossfades it out (the node pauses it).
//   PhysicalBoxPulse   the automatic gates (Gate Mode Pulse): 16 gate levels on
//                      \out (audio, 16), read by ModGate as its \gate_ar. Per
//                      voice two event streams: a copy of the shared one (the
//                      same seed: every voice the same times, lengths and
//                      velocities) and its own (its own seed). \link 0..1: the
//                      chance that a voice plays a shared event, else (1 - link)
//                      its own: 0 independent, 1 all together. Intervals: mean
//                      1 / \rate always; \chaos 0 periodic .. 1 exponential (a
//                      Poisson process: a Geiger counter); \cluster splits them
//                      into bursts of short gaps and long silences (same mean).
//                      \chance thins the events, \phasing sets the voices' own
//                      rates apart (+-1 octave at 1), \strum (s per voice, +-0.08, < 0
//                      downwards) rolls the shared events. Length: \length x
//                      the period, or \lengthsec with \fixed 1; \jitter
//                      (+-2 octaves at 1). \velocity x (1 - \veljitter x rnd).
//                      \free 1 runs alone, else \enable (Oceanode values, 16)
//                      or \enable_ar (\enable_sel 1) holds each voice's pulses
//                      and scales them (the node's Gate). Random draws are a
//                      hash of (\seed, stream, event number): the same seed
//                      plays the same pattern from the synth's start. \vis
//                      (control, 48): per voice its event count, last length
//                      (s), last velocity.
(
var root, dir, names, defs, d;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "PhysicalBox";
File.mkdir(root); File.mkdir(dir);
names = PhysicalVoice.names;
defs = PhysicalVoice.defaults;
d = { |name| defs[names.indexOf(name)] };

SynthDef(\PhysicalBoxVoice, {
    var m = { |name| In.kr(NamedControl.kr(name, 0), 1) };
    var p = { |name| NamedControl.kr(name, d.(name)) };
    var modded = { |name, mod, lo, hi| (p.(name) + m.(mod)).clip(lo, hi) };
    var note = Select.kr(\pitch_sel.kr(0), [\pitch.kr(60), A2K.kr(\pitch_ar.ar(60))]) + m.(\mpitch);
    var gate = In.ar(\gatebus.kr(0), 1);
    var decay = 2 ** m.(\mdecay), bright = m.(\mbright);
    var given = (
        freq: note.clip(0, 140).midicps,
        gate: gate,
        strk: \strk.tr(0),
        material: modded.(\material, \mmat, 0, 1),
        excite: modded.(\excite, \mexc, 0, 1),
        listen: modded.(\listen, \mexc, 0, 1),
        pos: modded.(\pos, \mpos, 0, 1),
        hardness: modded.(\hardness, \mhard, 0, 1),
        bowpress: modded.(\bowpress, \mpress, 0, 1.2),
        bowspeed: modded.(\bowspeed, \mspeed, 0, 1),
        breath: modded.(\breath, \mbreath, 0, 1.2),
        scrapespeed: modded.(\scrapespeed, \mscrape, 0, 1),
        abright: (p.(\abright) + bright).clip(0, 1),
        bbright: (p.(\bbright) + bright).clip(0, 1),
        adecay: (p.(\adecay) * decay).clip(0.01, 60),
        bdecay: (p.(\bdecay) * decay).clip(0.01, 60),
        bshape: modded.(\bshape, \mshape, 0, 5),
        couple: modded.(\couple, \mcouple, 0, 1),
        cascade: modded.(\cascade, \mcascade, 0, 1)
    );
    var pairs = names.collect({ |name| [name, given[name] ?? { p.(name) }] }).flatten;
    var ext = In.ar(\in.kr(0), 1);
    var sig = PhysicalVoice.ar(ext, pairs);
    var amp = Select.kr(\vca.kr(0) > 0.5, [(1 + m.(\mamp)).clip(0, 2), m.(\mamp).clip(0, 1)]);
    sig = LeakDC.ar(Sanitize.ar(sig)) * amp * \level.kr(0).dbamp * \master.kr(0.6, 0.02) * \normalization.kr(0.25);
    Out.ar(\out.kr(0), sig);
    Out.kr(\vis.kr(0) + (\voice.kr(0) * 4), [Amplitude.kr(sig.sum * 0.5, 0.01, 0.2), A2K.kr(gate), note, 0]);
}).writeDefFile(dir);

(1..16).do { |c|
    SynthDef(("PhysicalBoxSpread" ++ c).asSymbol, {
        var v = \voices.kr(1).clip(1, 16);
        var sig = In.ar(\in.kr(0), 32).asArray;
        var pos = (NamedControl.kr(\pan, 0.5 ! 16) + Sanitize.kr(In.kr(\mpan.kr(0), 16))).asArray.collect { |x| x.lag(0.02) };
        var width = \width.kr(0.5).clip(0, 1);
        var comp = (v / c).max(1).sqrt.reciprocal;
        var place = { |x, at|
            if(c == 1) { [x] } {
                if(c == 2) { Pan2.ar(x, (at.clip(0, 1) * 2) - 1) } {
                    PanAz.ar(c, x, at.wrap(0, 1) * 2, 1, 2, 0)
                }
            }
        };
        var out = 16.collect { |i|
            var on = i < v;
            var l = sig[i * 2] * on, r = sig[i * 2 + 1] * on;
            if(c == 1) { [(l + r) * 0.7071] } {
                place.(l, pos[i] - (width * 0.25)) + place.(r, pos[i] + (width * 0.25))
            }
        }.sum;
        Out.kr(\vis.kr(0), pos);
        Out.ar(\out.kr(0), out * comp);
    }).writeDefFile(dir);

    SynthDef(("PhysicalBoxSpace" ++ c).asSymbol, {
        var dry = In.ar(\in.kr(0), c).asArray;
        var on = \on.kr(1).lag(0.05);
        var mix = \mix.kr(0.25).clip(0, 1) * on;
        var stereo = case
            { c == 1 } { [dry[0], dry[0]] }
            { c == 2 } { dry }
            { [dry.select({ |x, i| i.even }).sum, dry.select({ |x, i| i.odd }).sum] * (2 / c).sqrt };
        var wet = MiVerb.ar(stereo, \time.kr(0.8).clip(0, 1.2), 1, \damp.kr(0.5).clip(0, 1),
            \hp.kr(0.05).clip(0, 1), \freeze.kr(0), \diff.kr(0.7).clip(0, 1));
        var back = c.collect { |i| if(c == 1) { wet.sum * 0.5 } { wet[i % 2] } };
        Out.ar(\out.kr(0), dry * (1 - mix) + (back * mix));
    }).writeDefFile(dir);
};
SynthDef(\PhysicalBoxPulse, {
    var n = 16;
    var seed = \seed.ir(1);
    var voices = \voices.kr(4).clip(1, n);
    var rate = \rate.kr(4).clip(0.01, 2000);
    var chaos = \chaos.kr(0).clip(0, 1), cluster = \cluster.kr(0).clip(0, 1);
    var chance = \chance.kr(1).clip(0, 1), link = \link.kr(1).clip(0, 1);
    var phasing = \phasing.kr(0).clip(0, 1), strum = \strum.kr(0).clip(-0.08, 0.08);
    var length = Select.kr(\fixed.kr(0) > 0.5, [\length.kr(0.5).max(0) / rate, \lengthsec.kr(0.1).max(0)]);
    var jitter = \jitter.kr(0).clip(0, 1);
    var velocity = \velocity.kr(1).clip(0, 1), veljitter = \veljitter.kr(0).clip(0, 1);
    var enable = Select.ar(\enable_sel.kr(0) > 0.5, [K2A.ar(NamedControl.kr(\enable, 0 ! n)), \enable_ar.ar(0 ! n)]);
    var free = \free.kr(1) > 0.5;
    // a uniform 0..1 per event of a stream: a sin hash of its event number
    // (counted here, so the same seed replays the same draws)
    var hash = { |x, salt| (((x % 65536) * 12.9898 + salt).sin * 437.5853).frac };
    var draws = { |salt| hash.(Dseries(0, 1, inf), salt) };
    var salt = { |stream, which| (seed * 0.7071) + (stream * 3.1731) + (which * 17.2313) };
    // mean 1 / (rate x mult) whatever chaos and cluster are
    var intervals = { |stream, mult|
        var e = draws.(salt.(stream, 0)).max(1e-5).log.neg;
        var burst = cluster * 0.85, long = (1 - (burst * 0.06)) / (1 - burst);
        var factor = long + ((0.06 - long) * (draws.(salt.(stream, 1)) < burst));
        var first = hash.(0, salt.(stream, 2)).max(0.001);    // the phase: a fraction of a period
        Dseq([first / (rate * mult), ((1 + (chaos * (e - 1))) * factor / (rate * mult)).max(0.0005)])
    };
    var outs = n.collect { |i|
        var on = i < voices;
        var at = Select.kr(voices > 1, [0, (i / (voices - 1).max(1)) - 0.5]);
        var mult = 2 ** (at * phasing * 2);
        var roll = Select.kr(strum >= 0, [(voices - 1 - i).max(0), i]) * strum.abs;
        // the shared events (stream 0: the same for every voice), each taken
        // with a coin of its own; ticks are 1, a played event 2
        // (rolled by the strum: one short delay line of ticks per voice)
        var shared = DelayN.ar(TDuty.ar(intervals.(0, 1), 0,
            1 + ((draws.(salt.(0, 3)) < chance) * (draws.(salt.(i + 1, 4)) < link)), 0, 1), 1.25, roll);
        var sharedLen = Demand.ar(shared, 0, draws.(salt.(0, 5)));
        var sharedVel = Demand.ar(shared, 0, draws.(salt.(0, 6)));
        var own = TDuty.ar(intervals.(i + 1, mult), 0,
            1 + ((draws.(salt.(i + 1, 3)) < chance) * (draws.(salt.(i + 1, 7)) >= link)), 0, 1);
        var ownLen = Demand.ar(own, 0, draws.(salt.(i + 1, 5)));
        var ownVel = Demand.ar(own, 0, draws.(salt.(i + 1, 6)));
        var fromShared = shared > 1.5;
        var fromOwn = own > 1.5;
        var trig = (fromShared + fromOwn) > 0;
        var len = Latch.ar(Select.ar(fromShared, [ownLen, sharedLen]), trig);
        var vel = Latch.ar(Select.ar(fromShared, [ownVel, sharedVel]), trig);
        var count = PulseCount.ar(trig);
        var dur = Latch.ar(K2A.ar(length), trig) * (2 ** (((len * 2) - 1) * jitter * 2));
        var level = Latch.ar(K2A.ar(velocity), trig) * (1 - (veljitter * vel));
        var since = Sweep.ar(trig, 1);
        // two samples closed before each event: a fresh edge even legato
        var open = (since > (1.5 * SampleDur.ir)) * (since < dur) * (count > 0);
        var hold = Select.ar(free, [enable[i], DC.ar(1)]);
        var gate = open * level * Select.ar(free, [Latch.ar(hold, trig), DC.ar(1)]) * (hold > 0) * on;
        [gate, A2K.kr(count), A2K.kr(dur), A2K.kr(level)]
    };
    Out.ar(\out.kr(0), outs.collect(_[0]));
    Out.kr(\vis.kr(0), outs.collect(_[1]) ++ outs.collect(_[2]) ++ outs.collect(_[3]));
}).writeDefFile(dir);

"PhysicalBox SynthDefs written to ".post; dir.postln;
)
