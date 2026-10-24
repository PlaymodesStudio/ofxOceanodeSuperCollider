// analogBox.scd — AnalogBox's voice as small synths (N voices, 1..16), and its
// own effects (C channels, 1..16). Needs the OceanodeAnalog UGens (MorphOsc,
// AnalogFilter: ugens/OceanodeAnalog). With the shared modulators
// (synthdefs/Defaults/Modulation), PolyComb's spreader (PolyCombSpread<C>) and
// MixerPro's inserts (EQ, saturation, echo, reverb):
//
//   ModGate -> ModLfo / ModEnv (sources) -> ModRoute (one per matrix cell)
//   ABPitch -> ABOsc (one per oscillator, in order: an oscillator's FM comes
//   from an earlier one) + ABNoise + ABRing -> ABFilter (1..4: in series,
//   each into the next, or in parallel, summed) -> ABAmp
//   -> PolyCombSpread<C> -> [ABFxShaper, mixerProSat, ABFxRing, ABFxShift, ABFxChorus,
//   mixerProEQ, mixerProEcho, boxMicroSpace <C>]
//
// Modulation arrives on control buses, one per target, each 16 channels (N
// used): \mpitch \mslave \mpw \mskew \mround \mfm \mnoise \mcut \mres \mdrive
// \mfb \mamp (\mpan: the spreader's).
(
var root, dir;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "AnalogBox";
File.mkdir(root); File.mkdir(dir);

(1..16).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };
    var modIn = { |name| In.kr(NamedControl.kr(name, 0), n) };
    var base = { In.kr(\note.kr(0) + n, n) };        // the note before vibrato and modulation (key tracking)

    // The note: glide, transpose, a random detune per voice, vibrato (a rate per voice), modulation
    // the pitch: Oceanode values, or an audio cable (\pitch_ar mapped, \pitch_sel 1)
    SynthDef(("ABPitch" ++ n).asSymbol, {
        var p = Select.kr(\pitch_sel.kr(0) > 0.5, [kv.(\pitch, 60), A2K.kr(\pitch_ar.ar(60 ! n))]);
        var b = Lag.kr(p, kv.(\glide, 0)) + kv.(\transpose, 0) + (({ Rand(-1, 1) } ! n) * kv.(\detune, 0));
        var rate = kv.(\vibrate, 5) * (1 + (({ Rand(-1, 1) } ! n) * kv.(\vibrand, 0).clip(0, 1)));
        var v = SinOsc.kr(rate, { Rand(0, 2pi) } ! n) * kv.(\vibamt, 0);
        Out.kr(\out.kr(0), (b + v + modIn.(\mpitch)) ++ b);
    }).writeDefFile(dir);

    // One oscillator: MorphOsc (the LFOs' shape, band-limited) at the note +
    // octave / semitones / cents x ratio (+ analog drift, + Slave Pitch when not
    // the first); phase modulation from an earlier oscillator's raw output;
    // hard sync to the first oscillator's frequency. Its raw output -> \raw (FM,
    // ring), its frequency -> \freqout (sync), x level -> \out (the mix).
    SynthDef(("ABOsc" ++ n).asSymbol, {
        var note = In.kr(\note.kr(0), n) + (\oct.kr(0) * 12) + kv.(\semi, 0) + (kv.(\fine, 0) / 100)
            + (modIn.(\mslave) * \slave.kr(0))
            + (LFNoise2.kr({ Rand(0.08, 0.25) } ! n) * kv.(\drift, 0) * 0.25);
        var freq = (note.clip(-36, 150).midicps * kv.(\ratio, 1)).clip(0.01, 22000);
        var weights = NamedControl.kr(\fmweights, [1, 0, 0, 0, 0, 0, 0]).lag(0.03);
        var sources = NamedControl.kr(\fmsources, 0 ! 6);
        var activation = weights.copyRange(1, 6).sum;
        var fm = (kv.(\fmamt, 0).lag(0.03) + (modIn.(\mfm) * activation)).max(0);
        var pm = Mix(Array.fill(6, { |i| InFeedback.ar(sources[i], n) * weights[i+1] }))
            * fm / activation.max(0.00001);
        var skew = (kv.(\skew, 0) + modIn.(\mskew)).clip(-1, 1);
        var round = (kv.(\round, 0.5) + modIn.(\mround)).clip(0, 1);
        var pw = (kv.(\pw, 0.5) + modIn.(\mpw)).clip(0.001, 1);
        var sync = In.kr(\syncfreq.kr(0), n);
        var free = MorphOsc.ar(freq, pm, skew, round, pw, \centered.kr(1), 0);
        var locked = MorphOsc.ar(freq, pm, skew, round, pw, \centered.kr(1), sync);
        var sig = LeakDC.ar(LinXFade2.ar(free, locked, \sync.kr(0, 0.03)*2-1))
            + (WhiteNoise.ar(1 ! n) * kv.(\noise, 0).clip(0, 1) * 1.5);   // Noise: added to this oscillator
        Out.kr(\freqout.kr(0), freq);
        Out.ar(\raw.kr(0), sig * \presence.kr(1,0.03));
        Out.ar(\out.kr(0), sig * kv.(\level, 0.5) * \presence.kr(1,0.03));
    }).writeDefFile(dir);

    // Noise: color -1 (dark) .. 0 (white) .. 1 (bright)
    SynthDef(("ABNoise" ++ n).asSymbol, {
        var w = WhiteNoise.ar(1 ! n);
        var c = kv.(\color, 0).clip(-1, 1);
        var dark = OnePole.ar(w, c.neg.max(0) * 0.995) * (1 + (c.neg.max(0) * 5));
        var bright = w - OnePole.ar(w, c.max(0) * 0.9);
        var sig = Select.ar(c < 0, [bright, dark]);
        // x1.5: white noise's energy is mostly high up, where the filter usually cuts
        Out.ar(\out.kr(0), sig * (kv.(\level, 0) + modIn.(\mnoise)).clip(0, 1) * 1.5);
    }).writeDefFile(dir);

    // Ring: oscillator 1 x oscillator 2
    SynthDef(("ABRing" ++ n).asSymbol, {
        var a = In.ar(\a.kr(0), n), b = In.ar(\b.kr(0), n);
        Out.ar(\out.kr(0), a * b * kv.(\level, 0).clip(0, 1));
    }).writeDefFile(dir);

    // A filter: a high-pass (Juno's), then AnalogFilter (SVF / ladder, 2 or 4
    // poles, or a comb) with drive, saturation and feedback; \on crossfades to
    // dry, \level after. Only the first writes \vis (\vison)
    SynthDef(("ABFilter" ++ n).asSymbol, {
        var sig = HPF.ar(In.ar(\in.kr(0), n), kv.(\hpf,10).clip(10, 4000));
        var cut = kv.(\cutoff, 100) + (kv.(\keytrack, 0) * (base.() - 60)) + modIn.(\mcut);
        var fc = cut.clip(0, 140).midicps.clip(10, 20000);
        var res = (kv.(\res, 0.2) + modIn.(\mres)).clip(0, 1);
        var driveDb = (kv.(\drive, 0) + modIn.(\mdrive)).clip(0, 48);
        var fb = (kv.(\fb, 0) + modIn.(\mfb)).clip(0, 1);
        var y = AnalogFilter.ar(sig, fc, res, \mode.kr(3), \type.kr(0), driveDb.dbamp, kv.(\sat, 0), fb);
        // the window: each voice's cutoff (note) and resonance now (\vis: control, 2 x 16)
        Out.kr(\vis.kr(0), (cut.clip(0, 140).asArray ++ (0 ! (16 - n)) ++ res.asArray) * \vison.kr(1));
        Out.ar(\out.kr(0), LinXFade2.ar(sig, y, \on.kr(1, 0.03) * 2 - 1) * Lag.kr(kv.(\level, 1), 0.05));
    }).writeDefFile(dir);

    // The VCA: \vca 1 (an envelope routed to Amp): the Amp modulation is the
    // level (0..1); else it drones, Amp modulation around 1. Velocity, level.
    SynthDef(("ABAmp" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0), n);
        var vel = In.kr(\vel.kr(0), n);
        var m = modIn.(\mamp);
        // the gate's level (held from its rising edge, 1 while droning); Vel Sens
        // blends in the velocity (\vel = level x Velocity)
        var lev = In.ar(\gate.kr(0), n);
        var vs = kv.(\velsens, 0.5).clip(0, 1);
        var amp = K2A.ar(Select.kr(\vca.kr(0) > 0.5, [(1 + m).clip(0, 2), m.clip(0, 1)])) *
            ((Gate.ar(lev, lev > 0) * (1 - vs)) + K2A.ar(vel * vs));
        Out.ar(\out.kr(0), Sanitize.ar(LeakDC.ar(sig * amp) * \level.kr(0.8, 0.05)));
    }).writeDefFile(dir);
};

// ABGateAlloc<N>: N gate / pitch / velocity inputs (Oceanode values or
// cables) onto a pool of K = min(16, 2N) voices, so a new note never restarts
// a voice that is still releasing: each new gate takes the next pool voice in
// turn (simultaneous ones the following), which follows its input's gate and
// pitch while it is that input's current voice, then releases with its own
// pitch. Writes as ModGate: \out each pool voice's gate x level (audio, K),
// \vel level x velocity, \rnd per note (control, K), plus \pitchout (audio, K).
(1..16).do { |n|
    var k = (n * 2).min(16);
    SynthDef(("ABGateAlloc" ++ n).asSymbol, {
        var gK = NamedControl.kr(\gate, 0 ! n), gA = (\gate_ar.ar(0 ! n) * \gate_sel.kr(0)).asArray;
        var inGate = ((K2A.ar(gK) + gA) > 0).asArray;
        var rise = inGate.collect { |g| HPZ1.ar(g) > 0 };
        // the level: a cable's exactly at its edge, Oceanode values held at control rate
        var levelIn = n.collect { |i| Latch.ar(gA[i], rise[i]).max(K2A.ar(Gate.kr(gK.asArray[i], gK.asArray[i] > 0))).max(0) };
        var pIn = Select.ar(\pitch_sel.kr(0) > 0.5, [K2A.ar(NamedControl.kr(\pitch, 60 ! n)), \pitch_ar.ar(60 ! n)]).asArray;
        var vIn = Select.ar(\vel_sel.kr(0) > 0.5, [K2A.ar(NamedControl.kr(\velocity, 1 ! n)), \vel_ar.ar(1 ! n)]).asArray;
        var count = rise.sum;
        var base = Integrator.ar(count, 1) - count;                 // notes before this sample
        var slot = n.collect { |i| (base + if(i == 0) { 0 } { rise[0 .. (i - 1)].sum }) % k };
        var cur = n.collect { |i| Latch.ar(slot[i], rise[i]) };      // each input's current pool voice
        var pool = k.collect { |j|
            var assign = n.collect { |i| rise[i] * ((slot[i] - j).abs < 0.5) };
            var any = assign.sum > 0;
            var owner = Latch.ar(n.collect { |i| assign[i] * i }.sum, any);
            var isCur = (Select.ar(owner, cur) - j).abs < 0.5;
            var gate = Select.ar(owner, inGate) * isCur;
            // the level: followed for a block and a half after the note (an
            // Oceanode value reaches audio rate as a ramp), then held
            var settle = Trig1.ar(any, ControlDur.ir * 1.5);
            var liveLevel = Select.ar(owner, levelIn);
            var level = Select.ar(settle, [Latch.ar(liveLevel, DelayN.ar(any, 0.1, ControlDur.ir * 1.5)), liveLevel]);
            var pitch = Gate.ar(Select.ar(owner, pIn), isCur);       // its own pitch through the release
            var vel = Latch.ar(Select.ar(owner, vIn), DelayN.ar(any, 0.1, ControlDur.ir * 1.5));
            [gate * level, pitch, vel * level, any]
        };
        Out.ar(\out.kr(0), pool.collect(_[0]));
        Out.ar(\pitchout.kr(0), pool.collect(_[1]));
        Out.kr(\vel.kr(0), A2K.kr(pool.collect(_[2])));
        Out.kr(\rnd.kr(0), A2K.kr(pool.collect { |v| TRand.ar(-1, 1, v[3] + Impulse.ar(0)) }));
    }).writeDefFile(dir);
};

// ── Effects at the speaker count (\on: crossfade to dry) ────────────────────
(1..16).do { |c|
    var insert = { |dry, wet| LinXFade2.ar(dry, wet, \on.kr(1, 0.03) * 2 - 1) };

    // Sine shaper (sineshaper.scd): sin(x * drive)
    SynthDef(("ABFxShaper" ++ c).asSymbol, {
        var sig = In.ar(\in.kr(0), c);
        var shaped = (sig * \drive.kr(2, 0.05).clip(0, 30)).sin;
        Out.ar(\out.kr(0), insert.(sig, XFade2.ar(sig, shaped, \mix.kr(1, 0.05).clip(0, 1) * 2 - 1)));
    }).writeDefFile(dir);

    // Ring modulator: a sine carrier, each channel a little apart (\spread, semitones)
    SynthDef(("ABFxRing" ++ c).asSymbol, {
        var sig = In.ar(\in.kr(0), c).asArray;
        var f = \freq.kr(440, 0.05).clip(0.1, 15000);
        var ring = sig.collect { |x, i|
            var off = if(c > 1) { (i / (c - 1) * 2 - 1) * \spread.kr(0) } { 0 };
            x * SinOsc.ar(f * (2 ** (off / 12)))
        };
        Out.ar(\out.kr(0), insert.(sig, XFade2.ar(sig, ring, \mix.kr(0.5, 0.05).clip(0, 1) * 2 - 1)));
    }).writeDefFile(dir);

    // Frequency shifter (FreqShifter.scd, extended): every partial moves by
    // \shift Hz (inharmonic: bells, barber-pole sweeps); \spread moves each
    // channel's shift apart (Hz across the speakers); \both adds the mirror
    // sideband (-shift: a ring modulator without its carrier); \fb feeds the
    // shifted sound back after \delay (s), \tone low-passing the loop: each
    // repeat shifts again (rising / falling spirals)
    SynthDef(("ABFxShift" ++ c).asSymbol, {
        var sig = In.ar(\in.kr(0), c).asArray;
        var shift = \shift.kr(50, 0.05).clip(-5000, 5000);
        var fb = \fb.kr(0, 0.05).clip(0, 0.95);
        var back = LocalIn.ar(c).asArray;
        var wet = sig.collect { |x, i|
            var off = if(c > 1) { (i / (c - 1) * 2 - 1) * \spread.kr(0, 0.05) } { 0 };
            var f = shift + off;
            var inp = x + (back[i] * fb);
            var up = FreqShift.ar(inp, f);
            var both = \both.kr(0).clip(0, 1);
            (up * (1 - (both * 0.5))) + (FreqShift.ar(inp, f.neg) * both * 0.5)
        };
        var loop = DelayC.ar(wet, 2, (\delay.kr(0.0, 0.05).clip(0, 2) - ControlDur.ir).max(0)).max(-4).min(4);
        LocalOut.ar(LeakDC.ar(LPF.ar(loop, \tone.kr(12000, 0.05).clip(200, 20000)).tanh));
        Out.ar(\out.kr(0), insert.(sig, XFade2.ar(sig, wet, \mix.kr(1, 0.05).clip(0, 1) * 2 - 1)));
    }).writeDefFile(dir);

    // Chorus: three modulated delays per channel (a BBD ensemble), their LFOs
    // a third of a cycle apart and turned per channel (\spread) for width
    SynthDef(("ABFxChorus" ++ c).asSymbol, {
        var sig = In.ar(\in.kr(0), c).asArray;
        var rate = \rate.kr(0.6, 0.05).clip(0.01, 10), depth = \depth.kr(3, 0.05).clip(0, 20) * 0.001;
        var delay = \delay.kr(8, 0.05).clip(1, 40) * 0.001;
        var wet = sig.collect { |x, i|
            var turn = if(c > 1) { i / c * \spread.kr(0.5) } { 0 };
            3.collect { |k|
                var ph = (k / 3 + turn) * 2pi;
                DelayC.ar(x, 0.08, delay + (SinOsc.kr(rate * (1 + (k * 0.11)), ph) * depth).max(0))
            }.sum * 0.45
        };
        Out.ar(\out.kr(0), insert.(sig, XFade2.ar(sig, wet, \mix.kr(0.5, 0.05).clip(0, 1) * 2 - 1)));
    }).writeDefFile(dir);
};
"AnalogBox SynthDefs written to ".post; dir.postln;
)
