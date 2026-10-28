// DrumKitBox: a kit of synthesized drum pieces (Kick, Snare, Hats...), each a
// stack of layers with its own voice, effects and mixer channel.
// Run this file in sclang; the node loads CompiledSynthdefs/DrumKitBox.
//
// Per piece, in its group, in this order (scDrumKitBox.cpp):
//   DKTrig            the piece's hits: reads its channel of the Gate
//                     input (audio cable \trigbus, Oceanode \gate level,
//                     \fire counter), its choke group, its pitch. Writes the
//                     hit bus (\hitbus, audio, 10 channels):
//                       0..3 hits         one-sample impulses; successive hits
//                                         take channels 0, 1, 2, 3 in turn (the
//                                         layers' voices, see below)
//                       4 choke           impulse: the choke group cut it
//                       5 level           the latest hit's level (velocity)
//                       6 pitch           semitones from 60, + Tune (live)
//                       7 gate            the gate (held), for Gated pieces
//                       8 duration        the latest hit's Duration (held from the hit): a multiplier
//                                         of the layers' hold and decay (\dur, per
//                                         hit: a closed hat stretched open)
//                       9 filter          the latest hit's Filter (\filt, per hit):
//                                         -1 low-pass closed .. 0 open .. +1
//                                         high-pass closed (DKVoice's step filter)
//                     Stutter: while a hit's gate is open, it retriggers at
//                     the frequency of \stut (a MIDI note per hit, 0 off), each
//                     retrigger Stutter Step semitones up, wrapping every
//                     Stutter Cycle (scRhythmBox's ARP in pitch mode).
//                     and the modulation's gate / velocity / random sources.
//   the piece's modulation   (synthdefs/Defaults/Modulation, 1 voice)
//   its layers, each created only while on, summed into the raw bus
//   (\out, audio, 2 channels):
//     DKBody          tonal body: phase-reset oscillator (sine / triangle /
//                     square), pitch sweep, bend, FM, second mode, drive
//     DKClick         transient: pulse, noise burst, blip, ping
//     DKNoise         noise: colour, clap bursts, filter + envelope, width
//     DKMetal         six-oscillator metal (808 ratios): square, ring, FM
//     DKSample1|2     sample: start, length, tune, reverse, envelope
//   Each layer comes in two builds (the pad's Poly):
//     mono  (DKBody...) a new hit takes over: Body and Sample run two voices
//           that take turns (the previous hit fades in 3 ms, no click), the
//           others one voice that the hit restarts
//     poly  (DKBodyP, DKClickP, DKNoiseP, DKMetalP, DKSample1P|2P) four voices
//           in turn: every hit rings on (until its choke group cuts it, or
//           a fourth hit later its voice is taken again)
//   DKVoice_N         the piece's voice: filter (+ envelope, key, velocity),
//                     character (drive types, crush), transient shaper,
//                     Gated VCA, level, pan / width into the piece's bus
//                     (N channels); its level for side-chain ducking
//   its effects, chained by the node: MixerPro's mixerProEQ, mixerProComp,
//   mixerProSat, boxMicroSpace, mixerProWidth, and two of this file:
//   boxEchoDrift_N (mixerProEcho plus Pitch per repeat and Drift) and
//   boxGrainDelay_N (a granular delay)
//   DKStrip_N         level, mute, ducking, into the node's Out and the
//                     piece's own output; its peak on \meterbus
//
// Every layer scales its times by \decayscale (the piece's Decay) and
// reads the modulation's targets (\modbus + k * 16, voice 0):
//   0 Pitch (st)  1 Decay (oct)  2 Cutoff (oct)  3 Resonance  4 Drive (dB)
//   5 Gain  6 Pan  7 Sweep (st)  8 Noise Cutoff (oct)  9 Metal Tone (oct)
//  10 Sample Start  11 Body Shape
(
var root, dir, hits, modOf, fadeOf, velGain, envAHD, voicesOf, layer;
var kChoke = 4, kLevel = 5, kPitch = 6, kGate = 7, kDur = 8, kFilt = 9;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "DrumKitBox";
File.mkdir(root); File.mkdir(dir);

hits = { In.ar(\hitbus.kr(0), 10) };
modOf = { |k| In.kr(\modbus.kr(0) + (k * 16), 1) };
// a voice cut (by the next hit in mono, by its choke group): 3 ms down,
// back to 1 at its own hit
fadeOf = { |hit, cut| LagUD.ar(1 - SetResetFF.ar(cut, hit), 0, 0.003) };
// the hit's level x Velocity amount (0: every hit at full level)
velGain = { |level| var a = \velamp.kr(1).clip(0, 1); (1 - a) + (a * level) };
// attack, hold, decay (curve); a new hit restarts it from where it is
envAHD = { |hit, atk, hold, dec, curve|
    EnvGen.ar(Env([0, 1, 1, 0], [atk.max(0.00005), hold.max(0), dec.max(0.001)], [0, 0, curve]), hit)
};
// A layer's voices, summed: fn.(hit, fade) makes one. poly: four voices, one
// per hit channel; mono and two: two voices taking turns, each faded by the
// other's hit; mono: one voice that every hit restarts
voicesOf = { |h, poly, two, fn|
    var all = h[0] + h[1] + h[2] + h[3];
    case
        { poly } { 4.collect { |v| fn.(h[v], fadeOf.(h[v], h[kChoke])) }.sum }
        { two } { 2.collect { |v| var hit = h[v] + h[v + 2];
            fn.(hit, fadeOf.(hit, h[1 - v] + h[3 - v] + h[kChoke])) }.sum }
        { fn.(all, fadeOf.(all, h[kChoke])) }
};
// both builds of a layer: name (mono) and name ++ "P" (poly)
layer = { |name, func| [false, true].do { |poly|
    SynthDef((name ++ if(poly, "P", "")).asSymbol, { func.(poly) }).writeDefFile(dir) } };

// ── DKTrig ──────────────────────────────────────────────────────────────────
SynthDef(\DKTrig, {
    var trigBus, audioGate, audioEdge, fire, hit, level, gate, count, turn, pitch;
    var chokeBlock, slot, cut, cutHit, kHit, dur, filt, stut, stutIdx, stutHit, allHit;
    var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);   // Global Pitch (scGlobalPitch.h)
    // Global Pitch's quantize / transpose / wrap at audio rate: a pitch sent
    // with its hit must be there at the hit's sample (K2A would ramp to it)
    var gq = { |p|
        var r = p.round, low = r < gg[17], on = ((low * gg[30]) + ((1 - low) * gg[1])) > 0.5;
        var off = Select.ar(r.mod(12) + (low * 12), (gg[2..13] ++ gg[18..29]).collect { |x| K2A.ar(x) });
        var q = p + (on * off), w = gg[16] + (((q - gg[16]) / 12).floor * 12);
        q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor)
    };

    trigBus = \trigbus.kr(-1);
    audioGate = In.ar(trigBus.max(0), 1) * (trigBus >= 0);
    audioEdge = HPZ1.ar(audioGate > 0) > 0;
    fire = T2A.ar(Changed.kr(\fire.kr(0)));
    hit = (audioEdge + fire) > 0;
    // the hit's level: the cable's at its edge, else the Oceanode gate's
    // (\gate is audio rate: set with the fire, it is there at the same
    // sample); none (a Play button): 1
    level = Latch.ar(Select.ar(audioEdge, [\gate.ar(0), audioGate]), hit);
    level = Select.ar(level > 0, [DC.ar(1), level]);
    gate = ((\gate.ar(0) > 0) + (audioGate > 0)) > 0;
    // Stutter (\stut is audio rate: sent with the hit, there at its sample)
    stut = Latch.ar(\stut.ar(0).clip(0, 127), hit);
    stutIdx = Sweep.ar(hit, stut.midicps * (stut > 0)).floor;
    stutHit = (HPZ1.ar(stutIdx) > 0) * (stut > 0) * gate;
    allHit = (hit + stutHit) > 0;
    count = PulseCount.ar(allHit);
    turn = (count - 1) % 4;
    pitch = gq.(\pitch.ar(60)) - 60 + \tune.kr(0) + (gg[14] * 0.01 * Rand(-1, 1))
        + ((stutIdx % \stutcycle.kr(4).round.clip(1, 64)) * \stutstep.kr(0) * (stut > 0));

    // Choke groups: every piece writes its hits to its slot of \chokeblock;
    // a hit of a piece in this one's group (\chokemask) cuts it
    kHit = A2K.kr(Trig1.ar(hit, ControlDur.ir * 2));
    chokeBlock = \chokeblock.kr(-1);
    slot = \chokeslot.kr(0).clip(0, 15);
    cut = (In.kr(chokeBlock.max(0), 16) * \chokemask.kr(0 ! 16)).sum * (chokeBlock >= 0);
    cutHit = T2A.ar((HPZ1.kr(cut > 0) > 0));
    ReplaceOut.kr(chokeBlock.max(0) + slot, Select.kr(chokeBlock >= 0,
        [In.kr(chokeBlock.max(0) + slot), kHit]));

    // the hit's Duration (\dur is audio rate: sent with the hit, there at its sample)
    dur = Latch.ar(\dur.ar(1).clip(0.05, 24), hit);
    dur = Select.ar(dur > 0, [DC.ar(1), dur]);
    filt = Latch.ar(\filt.ar(0).clip(-1, 1), hit);
    Out.ar(\hitbus.kr(0), 4.collect { |k| allHit * ((turn - k).abs < 0.5) } ++ [cutHit, level, pitch, gate, dur, filt]);
    // the modulation's sources: its envelopes follow the gate (a bare hit
    // opens it for 20 ms), Velocity, a Random per hit
    Out.ar(\gatebus.kr(0), Trig1.ar(hit, 0.02).max(gate) * level);
    Out.kr(\velbus.kr(0), A2K.kr(level));
    Out.kr(\rndbus.kr(0), TRand.kr(-1, 1, kHit));
    // the window: hits so far
    ReplaceOut.kr(\vis.kr(-1).max(0), Select.kr(\vis.kr(-1) >= 0, [In.kr(\vis.kr(-1).max(0)), A2K.kr(count)]));
}).writeDefFile(dir);

// ── DKBody: the tonal body ──────────────────────────────────────────────────
// Its phase restarts at every hit (Sweep integrates the frequency), so every
// kick starts the same. Frequency = Freq x (pitch + sweep + bend) semitones:
//   Sweep  starts Sweep semitones up and falls in Sweep Time (Curve)
//   Bend   moves Bend semitones over the whole hit (808 droop, rising zap)
// Shape 0 sine, 1 triangle, 2 square (morphs between). FM: a sine at Ratio,
// its index decaying in FM Decay. Mode 2: a second sine partial (membranes).
layer.(\DKBody, { |poly|
    var h = hits.(), decayK = \decayscale.kr(1) * (2 ** modOf.(1)), sig;
    sig = voicesOf.(h, poly, true, { |hit, fade|
        var p, level, amp, sw, bend, freq, ph, fmEnv, pm, x, shape, osc, mode2, g, dec, hold, dur;
        p = Latch.ar(h[kPitch], hit) + modOf.(0);
        level = Latch.ar(h[kLevel], hit);
        dur = A2K.kr(h[kDur]);   // control rate: EnvGen sticks on audio-rate times under fast retriggers
        hold = \bodyhold.kr(0) * dur;
        dec = \bodydecay.kr(0.45) * decayK * dur;
        amp = envAHD.(hit, \bodyatk.kr(0.0005), hold, dec, \bodycurve.kr(-5));
        sw = EnvGen.ar(Env([0, 1, 0], [0, (\sweeptime.kr(0.04) * decayK).max(0.0005)], [0, \sweepcurve.kr(-6)]), hit);
        bend = EnvGen.ar(Env([0, 0, 1], [0, hold + dec], [0, \bendcurve.kr(0)]), hit) * \bend.kr(0);
        freq = (\bodyfreq.kr(50) * (2 ** ((p + (sw * (\sweep.kr(36) + modOf.(7))) + bend) / 12))).clip(1, 20000);
        ph = Sweep.ar(hit, freq) + \bodyphase.kr(0);
        fmEnv = EnvGen.ar(Env([0, 1, 0], [0, (\fmdecay.kr(0.03) * decayK).max(0.0005)], [0, -4]), hit) * \fmamt.kr(0);
        pm = sin(2pi * Sweep.ar(hit, freq * \fmratio.kr(1.5))) * fmEnv * 2pi;
        x = (2pi * ph) + pm;
        shape = (\bodyshape.kr(0) + modOf.(11)).clip(0, 2);
        osc = SelectX.ar(shape, [sin(x), asin(sin(x)) * (2 / pi), (sin(x) * 6).tanh]);
        mode2 = sin(2pi * Sweep.ar(hit, freq * \m2ratio.kr(1.59))) * \m2level.kr(0) *
            EnvGen.ar(Env([0, 1, 0], [0, (dec * \m2decay.kr(0.5)).max(0.001)], [0, -4]), hit);
        osc = osc + mode2;
        g = 0.01 + (\bodydrive.kr(0).clip(0, 1) * 16);
        osc = (osc * g).tanh / g.tanh;
        osc * amp * fade * velGain.(level) * 0.7
    });
    Out.ar(\out.kr(0), (sig * (\bodylevel.kr(0).clip(-60, 24) > -59.9) * \bodylevel.kr(0).dbamp) ! 2);
});

// ── DKClick: the transient ──────────────────────────────────────────────────
// Type 0 Pulse (a decaying pulse through a resonant high-pass at Tone),
// 1 Noise (a noise burst band-passed at Tone), 2 Blip (a sine falling two
// octaves to Tone), 3 Ping (a resonator at Tone ringing for Decay)
layer.(\DKClick, { |poly|
    var h = hits.(), sig;
    sig = voicesOf.(h, poly, false, { |hit, fade|
        var level, tone, dec, rq, env, x;
        level = Latch.ar(h[kLevel], hit);
        tone = (\clicktone.kr(3000) * (2 ** (Latch.ar(h[kPitch], hit) * \clicktrack.kr(0) / 12))).clip(20, 20000);
        dec = (\clickdecay.kr(0.006) * (\decayscale.kr(1) * (2 ** modOf.(1)))).clip(0.0002, 0.5);
        rq = 1 - (\clickreso.kr(0.3).clip(0, 1) * 0.95);
        env = EnvGen.ar(Env([0, 1, 0], [0, dec], [0, -6]), hit);
        x = Select.ar(\clicktype.kr(0).round.clip(0, 3), [
            RHPF.ar(Decay2.ar(hit, 0.00003, dec * 0.5), tone, rq) * 2,
            BPF.ar(WhiteNoise.ar, tone, rq.max(0.1)) * env * 3,
            sin(2pi * Sweep.ar(hit, tone * (1 + (3 * EnvGen.ar(Env([0, 1, 0], [0, dec * 0.6], [0, -6]), hit))))) * env,
            Ringz.ar(hit, tone, dec * 4) * 0.5
        ]);
        x * fade * velGain.(level)
    });
    Out.ar(\out.kr(0), (sig * (\clicklevel.kr(0).clip(-60, 24) > -59.9) * \clicklevel.kr(0).dbamp) ! 2);
});

// ── DKNoise: noise, clap bursts ─────────────────────────────────────────────
// Colour 0 white, 1 pink, 2 brown. Bursts > 1: that many short bursts
// Spacing apart (a clap), the envelope (attack, hold, decay) from the last.
// Filter 0 low-pass, 1 band-pass, 2 high-pass, 3 notch at Cutoff, moved by
// its envelope (Env Amount octaves, Env Decay) and the pitch (Key Track).
// Width: the right channel's noise apart from the left's.
layer.(\DKNoise, { |poly|
    var h = hits.(), decayK = \decayscale.kr(1) * (2 ** modOf.(1)), sig;
    sig = voicesOf.(h, poly, false, { |hit, fade|
        var level, pitch, colour, noise, n, spacing, idx, step, burstHit, tailHit, burstEnv, tail, env;
        var fenv, cut, rq, filters, x, w, dur;
        level = Latch.ar(h[kLevel], hit);
        pitch = Latch.ar(h[kPitch], hit);
        dur = A2K.kr(h[kDur]);   // control rate: EnvGen sticks on audio-rate times under fast retriggers
        colour = \noisecolor.kr(0).clip(0, 2);
        noise = 2.collect { SelectX.ar(colour, [WhiteNoise.ar, PinkNoise.ar * 2, BrownNoise.ar * 2.5]) };
        w = \noisewidth.kr(0).clip(0, 1);
        noise = [noise[0], (noise[0] * (1 - w)) + (noise[1] * w)];
        n = \bursts.kr(1).round.clip(1, 8);
        spacing = \burstspace.kr(0.011).clip(0.002, 0.08);
        idx = Sweep.ar(hit, spacing.reciprocal).floor;
        step = (HPZ1.ar(idx) > 0) * (idx < n);
        burstHit = hit + step;
        tailHit = Select.ar(K2A.ar(n > 1), [hit, step * ((idx - (n - 1)).abs < 0.5)]);
        burstEnv = EnvGen.ar(Env([0, 1, 0], [0, spacing * 0.8], [0, -5]), burstHit) * (n > 1);
        tail = envAHD.(tailHit, \noiseatk.kr(0.0005), \noisehold.kr(0) * dur, \noisedecay.kr(0.2) * decayK * dur, \noisecurve.kr(-5));
        env = (burstEnv + tail).min(1);
        fenv = EnvGen.ar(Env([0, 1, 0], [0, (\noisefdecay.kr(0.08) * decayK).max(0.0005)], [0, -4]), hit);
        cut = (\noisecut.kr(6000) * (2 ** (modOf.(8) + (fenv * \noisefenv.kr(0)) + (pitch * \noisetrack.kr(0) / 12)))).clip(20, 20000);
        rq = 1 - (\noisereso.kr(0.2).clip(0, 1) * 0.97);
        filters = [RLPF.ar(noise, cut, rq), BPF.ar(noise, cut, rq) * (1 + (rq.reciprocal.sqrt)), RHPF.ar(noise, cut, rq), BRF.ar(noise, cut, rq)];
        x = Select.ar(\noisefilter.kr(1).round.clip(0, 3), filters);
        x = HPF.ar(x, \noisehp.kr(20).clip(20, 20000));
        x * env * fade * velGain.(level)
    });
    Out.ar(\out.kr(0), sig * (\noiselevel.kr(0).clip(-60, 24) > -59.9) * \noiselevel.kr(0).dbamp);
});

// ── DKMetal: six oscillators at the 808's ratios ────────────────────────────
// Inharmonic 1: the 808's ratios, 0: all at Freq, 2: twice as far apart.
// Mode 0 squares (808 hats / cymbal), 1 ring (pairs multiplied: bells,
// cowbells), 2 FM (three sine pairs, index FM). Tone: a band-pass; Colour
// blends in the unfiltered sound; everything above HP.
layer.(\DKMetal, { |poly|
    var h = hits.(), decayK = \decayscale.kr(1) * (2 ** modOf.(1)), sig;
    sig = voicesOf.(h, poly, false, { |hit, fade|
        var level, pitch, base, ratios, freqs, sq, src, tone, rq, bp, c, x, env, dur;
        level = Latch.ar(h[kLevel], hit);
        pitch = Latch.ar(h[kPitch], hit) + modOf.(0);
        dur = A2K.kr(h[kDur]);   // control rate: EnvGen sticks on audio-rate times under fast retriggers
        base = \metalfreq.kr(420) * (2 ** (pitch * \metaltrack.kr(1) / 12));
        ratios = [1, 1.4471, 1.617, 1.9265, 2.5028, 2.6637];
        freqs = (base * (1 + ((ratios - 1) * \inharm.kr(1).clip(0, 3)))).clip(1, 20000);
        sq = Pulse.ar(freqs, 0.5);
        src = Select.ar(\metalmode.kr(0).round.clip(0, 2), [
            sq.sum / 3,
            ((sq[0] * sq[1]) + (sq[2] * sq[3]) + (sq[4] * sq[5])) / 2,
            [0, 2, 4].collect { |k| SinOsc.ar(freqs[k], SinOsc.ar(freqs[k + 1]) * \metalfm.kr(2)) }.sum / 3
        ]);
        tone = (\metaltone.kr(8000) * (2 ** modOf.(9))).clip(20, 20000);
        rq = 1 - (\metalreso.kr(0.3).clip(0, 1) * 0.95);
        bp = BPF.ar(src, tone, rq) * (1 + rq.reciprocal.sqrt);
        c = \metalcolor.kr(0.3).clip(0, 1);
        x = HPF.ar((bp * (1 - c)) + (src * c), \metalhp.kr(3000).clip(20, 20000));
        env = envAHD.(hit, \metalatk.kr(0.0005), \metalhold.kr(0) * dur, \metaldecay.kr(0.08) * decayK * dur, \metalcurve.kr(-6));
        x * env * fade * velGain.(level)
    });
    Out.ar(\out.kr(0), (sig * (\metallevel.kr(0).clip(-60, 24) > -59.9) * \metallevel.kr(0).dbamp) ! 2);
});

// ── DKSample1 / DKSample2: a sample layer (mono / stereo file) ──────────────
// Start and Length (fractions of the file), Tune (st) plus the hit's pitch
// (Key Track), Reverse, the envelope (Decay 10 s: the whole region). Mono:
// two voices take turns like the body's. The region's end fades in 4 ms.
(1..2).do { |k|
    layer.("DKSample" ++ k, { |poly|
        var h = hits.(), decayK = \decayscale.kr(1) * (2 ** modOf.(1)), buf0 = \buf0.kr(0), buf1 = \buf1.kr(0), sig;
        sig = voicesOf.(h, poly, true, { |hit, fade|
            var level, pitch, frames, st, en, len, rate, pos, frame, edge, env, dec, l, r, dur;
            level = Latch.ar(h[kLevel], hit);
            pitch = Latch.ar(h[kPitch], hit) + modOf.(0);
            frames = BufFrames.kr(buf0).max(2);
            st = (\smpstart.kr(0) + modOf.(10)).clip(0, 0.999) * frames;
            len = (\smplen.kr(1).clip(0.001, 1) * frames).min(frames - st).max(2);
            en = st + len;
            rate = BufRateScale.kr(buf0) * (2 ** ((\smptune.kr(0) + (pitch * \smptrack.kr(1))) / 12));
            pos = Sweep.ar(hit, rate * SampleRate.ir);
            frame = Select.ar(K2A.ar(\smpreverse.kr(0) > 0.5), [st + pos, en - 1 - pos]).clip(0, frames - 1);
            edge = ((len - pos) / (rate * SampleRate.ir * 0.004).max(1)).clip(0, 1);
            dur = A2K.kr(h[kDur]);   // control rate: EnvGen sticks on audio-rate times under fast retriggers
            dec = \smpdecay.kr(10);
            env = Select.ar(K2A.ar(dec >= 9.99), [
                envAHD.(hit, \smpatk.kr(0.0002), \smphold.kr(0) * dur, dec * decayK * dur, \smpcurve.kr(-4)),
                EnvGen.ar(Env([0, 1, 1], [\smpatk.kr(0.0002).max(0.00005), 0]), hit)]);
            l = BufRd.ar(1, buf0, frame, 0, 4);
            r = if(k == 2, { BufRd.ar(1, buf1, frame, 0, 4) }, { l });
            [l, r] * env * edge * fade * velGain.(level)
        });
        Out.ar(\out.kr(0), sig * \loaded.kr(0) * (\smplevel.kr(0).clip(-60, 24) > -59.9) * \smplevel.kr(0).dbamp);
    });
};

// ── DKVoice_N: the piece's voice ────────────────────────────────────────────
// Filter 0 LP 12, 1 LP 24, 2 HP, 3 BP; its cutoff moved by its envelope
// (Env Amount octaves, Env Decay), the pitch (Key Track) and the hit's
// velocity (Vel Tone: soft hits darker). Character: Drive (dB) through
// 0 soft (tanh), 1 hard clip, 2 fold, 3 tube (asymmetric), then Crush (bits,
// sample rate). Transient: Punch boosts (or softens) the attack, Sustain the
// body after it. Gated: the gate closing fades it in Release. Pan and Width
// (the layers' stereo) into N channels.
(1..16).do { |n|
    SynthDef(("DKVoice_" ++ n).asSymbol, {
        var raw = In.ar(\raw.kr(0), 2), h = hits.(), hit = h[0] + h[1] + h[2] + h[3], decayK = \decayscale.kr(1) * (2 ** modOf.(1));
        var level, pitch, fenv, cut, rq, filters, weights, filtered, sig, drive, g, drv, bits, q, crushed, sr;
        var det, fast, slow, atkDiff, susLong, susShort, susDiff, tg, vca, gain, pan, width, mid, side, out, key;
        var sf, lpf, hpf, srq, stepped;
        level = Latch.ar(h[kLevel], hit);
        pitch = Latch.ar(h[kPitch], hit);
        sig = raw;

        // filter
        fenv = EnvGen.ar(Env([0, 1, 0], [0, (\fenvdecay.kr(0.15) * decayK).max(0.0005)], [0, -4]), hit);
        cut = (\cutoff.kr(18000) * (2 ** (modOf.(2) + (fenv * \fenvamt.kr(0)) + (pitch * \keytrack.kr(0) / 12) +
            (\veltone.kr(0).clip(0, 1) * (level - 1) * 3)))).clip(20, 20000);
        rq = 1 - ((\resonance.kr(0.1) + modOf.(3)).clip(0, 1) * 0.95);
        filters = [RLPF.ar(sig, cut, rq), RLPF.ar(RLPF.ar(sig, cut, rq.sqrt), cut, rq.sqrt), RHPF.ar(sig, cut, rq),
            BPF.ar(sig, cut, rq) * (1 + rq.reciprocal.sqrt)];
        weights = NamedControl.kr(\filterweights, 0 ! 4).lag(0.03);
        filtered = Select.ar(weights.sum > 0, [Select.ar(\filtertype.kr(0).round.clip(0, 3), filters),
            Mix(filters.collect { |f, i| f * weights[i] })]);
        sig = XFade2.ar(sig, filtered, \filteron.kr(0).lag(0.02) * 2 - 1);

        // the step filter: each hit's Filter (the Drum Grid's per-step LP / HP),
        // -1 low-pass down to 60 Hz .. 0 open (bypassed) .. +1 high-pass up to
        // 12 kHz, resonance Step Filter Reso
        sf = Lag.ar(h[kFilt], 0.002);
        lpf = 20000 * ((60 / 20000) ** sf.clip(-1, 0).neg);
        hpf = 20 * ((12000 / 20) ** sf.clip(0, 1));
        srq = 1 - (\sfreso.kr(0.2).clip(0, 1) * 0.9);
        stepped = RHPF.ar(RLPF.ar(sig, lpf, srq), hpf, srq);
        sig = XFade2.ar(sig, stepped, ((sf.abs * 40).clip(0, 1) * 2) - 1);

        // character
        drive = (\drive.kr(6) + modOf.(4)).clip(0, 48);
        g = drive.dbamp;
        drv = Select.ar(\drivetype.kr(0).round.clip(0, 3), [
            (sig * g).tanh,
            (sig * g).clip2(1),
            (sig * g).fold2(1),
            ((sig * g) + 0.3).tanh - 0.3.tanh
        ]) * (1 / (1 + (drive / 24)));
        drv = XFade2.ar(sig, LeakDC.ar(drv), \drivemix.kr(1).clip(0, 1) * 2 - 1);
        bits = \bits.kr(24).clip(1, 24);
        q = 2 ** (bits - 1);
        sr = \srate.kr(48000).clip(100, 48000).min(SampleRate.ir);
        crushed = Latch.ar(drv, Impulse.ar(sr));
        crushed = Select.ar(K2A.ar(sr >= (SampleRate.ir * 0.999)), [crushed, drv]);
        crushed = Select.ar(K2A.ar(bits < 23.99), [crushed, (crushed * q).round / q]);
        sig = XFade2.ar(sig, crushed, \charon.kr(0).lag(0.02) * 2 - 1);

        // transient shaper
        det = sig[0].abs.max(sig[1].abs);
        fast = Amplitude.ar(det, 0.0003, 0.04);
        slow = Amplitude.ar(det, 0.012, 0.04);
        atkDiff = ((fast - slow).max(0) / fast.max(0.00001)).clip(0, 1);
        susLong = Amplitude.ar(det, 0.0003, 0.35);
        susShort = Amplitude.ar(det, 0.0003, 0.04);
        susDiff = ((susLong - susShort).max(0) / susLong.max(0.00001)).clip(0, 1);
        tg = ((1 + (atkDiff * \punch.kr(0).clip(-1, 1) * 2)).max(0) * (1 + (susDiff * \sustain.kr(0).clip(-1, 1) * 3)).max(0)).clip(0, 4);
        sig = sig * Select.ar(K2A.ar(\transon.kr(0) > 0.5), [DC.ar(1), tg]);

        // gated, level, pan / width
        vca = Select.ar(K2A.ar(\gated.kr(0) > 0.5), [DC.ar(1), LagUD.ar(h[kGate], 0.0005, \release.kr(0.08).max(0.001))]);
        gain = (\gain.kr(1) + modOf.(5)).clip(0, 4);
        sig = sig * vca * gain;
        pan = (\pan.kr(0) + modOf.(6)).clip(-1, 1);
        width = \width.kr(1).clip(0, 2);
        mid = (sig[0] + sig[1]) * 0.5;
        side = (sig[0] - sig[1]) * 0.5 * width;
        out = case
            { n == 1 } { [mid] }
            { n == 2 } { Balance2.ar(mid + side, mid - side, pan) }
            { PanAz.ar(n, mid + side, pan - (width / n)) + PanAz.ar(n, mid - side, pan + (width / n)) * 0.7071 };
        Out.ar(\out.kr(0), out);
        // its level for the strips that duck from it (\keybus: the node's key block + this piece)
        key = Amplitude.kr(A2K.kr(mid.abs + side.abs), 0.001, 0.05);
        ReplaceOut.kr(\keybus.kr(-1).max(0), Select.kr(\keybus.kr(-1) >= 0, [In.kr(\keybus.kr(-1).max(0)), key]));
    }).writeDefFile(dir);

    // Echo with drift: mixerProEcho's loop (feedback through LP / HP / BP /
    // Peak, ping-pong, \on gates the input so the repeats ring out), plus
    //   \pitch      semitones each repeat is shifted by (a rising / falling
    //               spiral; 0: no shifter in the loop)
    //   \drift      the delay time wanders (tape wow), \driftrate Hz: the
    //               repeats' pitch drifts, differently on each channel
    SynthDef(("boxEchoDrift_" ++ n).asSymbol, {
        var maxDel = 4.5, dry, gate, del, fb, cut, rq, mix, pp, fbIn, wet, filters, weights, shift, wow;
        dry  = In.ar(\in.kr(0), n).asArray;
        gate = Lag.ar(K2A.ar(\on.kr(1)), 0.02);
        del  = \delay.kr(0.375, 0.1).clip(0.005, 4);
        fb   = \feed.kr(0.5, 0.05).clip(0, 0.98);
        cut  = \cutoff.kr(84, 0.05).clip(12, 130).midicps.min(SampleRate.ir * 0.45);
        rq   = 1 - \resonance.kr(0.2, 0.05).clip(0, 0.9);
        mix  = \mix.kr(0.35, 0.05).clip(0, 1);
        pp   = \pingpong.kr(0, 0.05).clip(0, 1);
        shift = \pitch.kr(0, 0.05).clip(-12, 12);
        fbIn = LocalIn.ar(n).asArray;
        if(n > 1) {
            fbIn = fbIn.collect { |x, i|
                var partner = if(i.even) { (i + 1).min(n - 1) } { i - 1 };
                (x * (1 - pp)) + (fbIn[partner] * pp)
            };
        };
        wet = LeakDC.ar((fbIn * fb) + (dry * gate), 0.9995);
        filters = [RLPF.ar(wet, cut, rq), RHPF.ar(wet, cut, rq), BPF.ar(wet, cut, rq), BPeakEQ.ar(wet, cut, 1, (1 - rq) * 12)];
        weights = NamedControl.kr(\filterweights, 0 ! 4);
        wet = Select.ar(weights.sum > 0, [Select.ar(\filtertype.kr(0).clip(0, 3), filters),
            Mix(filters.collect { |signal, i| signal * weights[i].lag(0.03) })]);
        wet = Select.ar(shift.abs > 0.01, [wet, PitchShift.ar(wet, 0.09, shift.midiratio, 0, 0.004)]);
        wet = (wet * 0.5).tanh * 2;
        // each channel's own slow wander: the delay time +- Drift x 4 %
        wow = n.collect { LFNoise2.kr(\driftrate.kr(0.4).clip(0.01, 20) * Rand(0.7, 1.3)) } * \drift.kr(0, 0.1).clip(0, 1) * 0.04 * del;
        LocalOut.ar(DelayC.ar(wet, maxDel, (del - ControlDur.ir + wow).clip(0, maxDel)));
        Out.ar(\out.kr(0), (dry * (1 - (mix * gate)).sqrt) + (fbIn * mix.sqrt));
    }).writeDefFile(dir);

    // Granular delay (from Source's GrainFeedDelay): grains read from the
    // last 4 s of each channel around Delay (+ random Jitter), Density per
    // second (Periodic: 0 random .. 1 regular), Size (+ random Size Jitter),
    // Pitch (+ random Pitch Jitter per grain), Reverse (chance per grain),
    // Spread (grains wander across the channels). Feedback through a
    // low-pass (Tone, MIDI). \on gates the input: the cloud rings out.
    SynthDef(("boxGrainDelay_" ++ n).asSymbol, {
        var dry, gate, del, jit, dens, per, size, sizeJit, pitch, pitchJit, rev, fb, tone, spread, mix, fbIn, frames, wet, outs;
        dry  = In.ar(\in.kr(0), n).asArray;
        gate = Lag.ar(K2A.ar(\on.kr(1)), 0.02);
        del = \delay.kr(0.25, 0.1).clip(0.01, 3.5);
        jit = \jitter.kr(0.05).clip(0, 0.4);
        dens = \density.kr(12).clip(0.5, 80);
        per = \periodic.kr(0).clip(0, 1);
        size = \size.kr(0.08).clip(0.005, 0.5);
        sizeJit = \sizejit.kr(0.3).clip(0, 1);
        pitch = \pitch.kr(0).clip(-24, 24);
        pitchJit = \pitchjit.kr(0.2).clip(0, 12);
        rev = \reverse.kr(0).clip(0, 1);
        fb = \feed.kr(0.3, 0.05).clip(0, 0.95);
        tone = \tone.kr(110, 0.05).clip(30, 130).midicps.min(SampleRate.ir * 0.45);
        spread = \spread.kr(0.5).clip(0, 1);
        mix = \mix.kr(0.35, 0.05).clip(0, 1);
        fbIn = LocalIn.ar(n).asArray;
        outs = n.collect { |i|
            var buf = LocalBuf(SampleRate.ir * 4, 1).clear, writePos, trig, d, g;
            frames = BufFrames.kr(buf);
            writePos = Phasor.ar(0, 1, 0, frames);
            BufWr.ar(LeakDC.ar((dry[i] * gate) + (fbIn[i] * fb)), buf, writePos);
            trig = SelectX.kr(per, [Dust.kr(dens), Impulse.kr(dens)]);
            d = (del + TRand.kr(0, jit, trig) + size).min(3.9);    // never past the write head
            g = GrainBuf.ar(1, trig, size * (1 + TRand.kr(sizeJit.neg, sizeJit, trig)).max(0.005), buf,
                (pitch + TRand.kr(pitchJit.neg, pitchJit, trig)).midiratio * Select.kr(TRand.kr(0, 1, trig) < rev, [1, -1]),
                Wrap.kr(A2K.kr(writePos) - (d * SampleRate.ir), 0, frames) / frames, 2, 0, -1, 64);
            [g, trig]
        };
        wet = outs.collect(_[0]);
        // Spread: each channel's grains leak into the others, more with Spread
        if(n > 1) {
            wet = wet.collect { |x, i| (x * (1 - (spread * 0.5))) + (wet.rotate(1)[i] * spread * 0.5) };
        };
        LocalOut.ar(LPF.ar(LeakDC.ar(wet), tone).collect { |x| (x * 0.8).tanh });
        Out.ar(\out.kr(0), (dry * (1 - (mix * gate)).sqrt) + (wet * mix.sqrt * 1.4));
    }).writeDefFile(dir);

    // The channel strip: Level x the node's Gain, muted (Mute, Solo), ducked by
    // another piece (\duckbus: its key; Duck amount, attack, release), into
    // the mix and the piece's own output; the peak for the meter
    SynthDef(("DKStrip_" ++ n).asSymbol, {
        var sig = In.ar(\bus.kr(0), n), g, mixBus = \mix.kr(-1), padBus = \padout.kr(-1), duck, denv;
        duck = \duckbus.kr(-1);
        denv = LagUD.kr((In.kr(duck.max(0)) * (duck >= 0) * 3).clip(0, 1), \duckatk.kr(0.003).max(0.0005), \duckrel.kr(0.18).max(0.005));
        g = (\level.kr(1).clip(0, 4) * \master.kr(0.7).clip(0, 4) * (1 - \mute.kr(0).clip(0, 1))).lag(0.02);
        g = g * (1 - (\duckamt.kr(0).clip(0, 1) * denv));
        sig = sig * g;
        Out.ar(mixBus.max(0), sig * (mixBus >= 0));
        Out.ar(padBus.max(0), sig * (padBus >= 0));
        Out.kr(\meterbus.kr(0), Amplitude.kr(sig.asArray.abs.reduce(\max), 0.002, 0.25));
    }).writeDefFile(dir);
};
"DrumKitBox SynthDefs written to ".post; dir.postln;
)
