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
"PhysicalBox SynthDefs written to ".post; dir.postln;
)
