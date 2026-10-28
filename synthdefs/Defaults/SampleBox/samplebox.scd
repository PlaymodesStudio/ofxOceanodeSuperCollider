// SampleBox: a kit of sampler pads, each with its own mixer channel.
// Run this file in sclang; the node loads CompiledSynthdefs/SampleBox.
//
// Per pad, in its group, in this order (scSampleBox.cpp):
//   SampleBoxPad_K_N_<R|T|M> the sampler: voices, amp envelopes, filter.
//                           K = source channels (1/2), N = output channels
//                           (1..16), R = resampling, T = Warp1 time stretch,
//                           M = both readers blended by \stretchmix (only
//                           while a snapshot morph is between the modes: it
//                           costs both readers; C++ goes back to R/T after).
//                           Writes the pad's own bus (\out, N channels).
//   the pad's modulation    (synthdefs/Defaults/Modulation)
//   its effects, in order, each created only while switched on (so an
//   effect that is off costs nothing), each replacing the pad's bus:
//     SampleBoxEQ_N, SampleBoxComp_N, SampleBoxSat_N, SampleBoxEcho_N,
//     SampleBoxReverb_N, SampleBoxImage_N (N > 1)
//   SampleBoxStrip_N        level, mute: into the node's Out (\mix, summed
//                           with the other pads) and the pad's own output
//                           (\padout); its peak on \meterbus.
//
// Tempo (the pad's Fit and Sync):
//   \fitrate   speed factor that makes the region last Fit beats at the
//              current tempo (C++ keeps it up to date): Resample changes the
//              pitch with the tempo, Stretch keeps it
//   \sync      1: while the transport plays, the read position is the
//              transport's: (beat / \fitbeats x Rate) wrapped over the region.
//              A looping note plays it as is; a one-shot starts where the
//              transport is and runs at the fitted speed. The transport
//              clock follows scTransportSync's anchors (\anchorId, \anchorPos
//              in beats, kept small by C++, \run, \tbpm).
(
var root, dir, targetRanges, modOf;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "SampleBox";
File.mkdir(root); File.mkdir(dir);

// Modulation comes from the pad's own modulation (synthdefs/Defaults/
// Modulation): one control bus per target (\modbus + k * 16, 8 channels: one
// per voice), already scaled to the target's units by the routes. These are
// the synth's old target ids -> the node's target k (Pitch, Rate, In, Out,
// Gain, Pan, Stretch, Grain, Loop Crossfade, Cutoff, Resonance, Drive, Comp
// Threshold, Reverb Mix, Width). The voices write their gates (audio, 8), each
// note's velocity and a random -1..1 (control, 8) for the modulation.
targetRanges = Dictionary[1 -> 0, 2 -> 1, 3 -> 2, 4 -> 3, 5 -> 4, 6 -> 5, 28 -> 6, 32 -> 7, 33 -> 8,
    7 -> 9, 8 -> 10, 19 -> 11, 14 -> 12, 23 -> 13, 24 -> 14];

// An effect's modulation: the latest note's (its voice is on \latestbus)
modOf = { |k, base|
    base + Select.kr(In.kr(\latestbus.kr(0)).clip(0, 7), In.kr(\modbus.kr(0) + (k * 16), 8))
};

// ── The sampler of a pad ────────────────────────────────────────────────────
(1..2).do { |k|
    (1..16).do { |n|
        [0, 1, 2].do { |stretchVariant|
            var name = "SampleBoxPad_" ++ k ++ "_" ++ n ++ ["_R", "_T", "_M"][stretchVariant];
            SynthDef(name.asSymbol, {
                var trigBus, audioGate, audioTrig, fireTrig, trig, gate, stopTrig, level;
                var buf, rateBase, inBase, outBase, polyMode, voiceLimit, voiceIndex;
                var source, voices, finished, sig, modArr, fxMod, f, q, filter;
                var chokeBlock, chokeIn, cut;
                var tpos, tphase, syncOn, fitRate;
                // R: 0, T: 1 (constants: one reader only); M: the morph's weight
                var stretchMix = if(stretchVariant == 2, { \stretchmix.kr(0).clip(0, 1).lag(0.03) }, { stretchVariant });
                var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);   // Global Pitch: transpose, scale, detune (scGlobalPitch.h)
                var gq = { |p| var r = p.round, q = p + (((((r < gg[17]) * gg[30]) + ((r >= gg[17]) * gg[1])) > 0.5) * Select.kr(r.mod(12) + ((r < gg[17]) * 12), gg[2..13] ++ gg[18..29])), w = gg[16] + (((q - gg[16]) / 12).floor * 12); q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor) };

                trigBus = \trigbus.kr(-1);
                audioGate = In.ar(trigBus.max(0), 1) * (trigBus >= 0);
                // Stretch a one-sample edge to two control blocks so an audio
                // impulse cannot vanish between control-rate samples.
                audioTrig = A2K.kr(Trig1.ar(HPZ1.ar(audioGate > 0) > 0, 0.004));
                fireTrig = Changed.kr(\fire.kr(0));
                trig = (audioTrig + fireTrig) > 0;
                gate = (\gate.kr(0) > 0) + (A2K.kr(audioGate > 0) > 0);
                // the note's level: the gate's (audio: at its edge; Oceanode: its
                // value; a Trigger press without one: 1). A gate at 0.2 plays at 0.2
                level = Select.kr(audioTrig > 0, [\gate.kr(0), A2K.kr(Latch.ar(audioGate, HPZ1.ar(audioGate > 0) > 0))]);
                level = Select.kr(level > 0, [1, level]);
                stopTrig = Changed.kr(\stop.kr(0));
                // Choke groups: every pad writes its triggers to its slot of
                // \chokeblock; a trigger of a pad in this one's group (\chokemask)
                // cuts its notes (5 ms)
                chokeBlock = \chokeblock.kr(-1);
                chokeIn = (In.kr(chokeBlock.max(0), 16) * \chokemask.kr(0 ! 16)).sum * (chokeBlock >= 0);
                cut = chokeIn > 0;
                ReplaceOut.kr(chokeBlock.max(0) + \chokeslot.kr(0).clip(0, 15), Select.kr(chokeBlock >= 0,
                    [In.kr(chokeBlock.max(0) + \chokeslot.kr(0).clip(0, 15)), trig]));
                buf = \buf0.kr(0);
                rateBase = \rate.kr(1).clip(0.125, 8);
                fitRate = \fitrate.kr(1).clip(0.01, 100);
                // the transport's position (beats) and the region's phase there
                tpos = \anchorPos.ar(0) + Sweep.ar(T2A.ar(Changed.kr(\anchorId.kr(0))), \tbpm.kr(120) / 60 * \run.ar(0));
                tphase = (tpos / \fitbeats.kr(4).max(0.001)).frac;
                // stopped transport: the pad plays freely (auditioning still works)
                syncOn = (\sync.kr(0) > 0.5) * (A2K.kr(\run.ar(0)) > 0.5);
                inBase = \inpoint.kr(0).clip(0, 1);
                outBase = \outpoint.kr(1).clip(0, 1);
                polyMode = \poly.kr(1) > 0.5;
                voiceLimit = \voices.kr(8).clip(1, 8);
                // Polyphonic: round robin over Max Voices. Mono: two voices
                // take turns, so a retrigger fades the old note out (choke)
                // instead of jumping it back to the start (a click).
                voiceIndex = PulseCount.kr(trig) % Select.kr(polyMode, [2, voiceLimit]);
                // the effects follow the latest note's modulation (no bus yet:
                // control bus 0 rewritten with its own value)
                ReplaceOut.kr(\latestbus.kr(-1).max(0), Select.kr(\latestbus.kr(-1) >= 0,
                    [In.kr(\latestbus.kr(-1).max(0)), voiceIndex]));

                // the modulation, per target (8 voices each)
                modArr = 15.collect { |t| In.kr(\modbus.kr(0) + (t * 16), 8) };

                // which one-shots reached their end (last block): their note ends
                finished = LocalIn.kr(8);
                voices = 8.collect { |v|
                    var voiceTrig, choke, held, vm, pitch, speed, step, start, finish;
                    var frames, startFrame, finishFrame, len, loopOn, fade, loopLen, pos, offset;
                    var fadeMix, xoff, rev, frame, xframe, edge, amp, voicePan, read;
                    var syncPhase, syncLoop, syncShot, endPos;
                    var left, right, left2, right2, raw;
                    voiceTrig = trig * ((voiceIndex - v).abs < 0.5) * (Select.kr(polyMode, [2, voiceLimit]) > v);
                    // another note took over in mono mode, or the choke group cut it: fade out fast
                    choke = SetResetFF.kr((trig * (1 - voiceTrig) * (1 - polyMode)) + cut, voiceTrig);
                    held = SetResetFF.kr(voiceTrig, stopTrig + (choke > 0) + (finished[v] * (\loop.kr(0) < 0.5))) *
                        Select.kr(\gatemode.kr(0) > 0.5, [1, gate > 0]);
                    // this voice's modulation of a target (the old ids; none: the base)
                    vm = { |id, base| var t = targetRanges[id]; if(t.notNil) { base + modArr[t][v] } { base } };
                    // Latch the note and velocity per voice: a new note must
                    // not retune or re-level voices already sounding.
                    pitch = vm.(1, gq.(Latch.kr(\pitch.kr(60), voiceTrig))).clip(0, 127);
                    // Transpose is live (not latched): it moves notes already
                    // sounding. Stretch: only the grains' pitch, the
                    // timeline keeps its speed. Resample: speed and pitch.
                    pitch = pitch + \transpose.kr(0).clip(-48, 48).lag(0.03)
                        + (gg[14] * 0.01 * Rand(-1, 1));   // Global Pitch: this voice's detune (the transpose is in gq)
                    speed = vm.(2, rateBase).clip(0.01, 16);
                    start = vm.(3, inBase).clip(0, 0.9999);
                    finish = vm.(4, outBase).clip(start + 0.00001, 1);
                    frames = BufFrames.kr(buf).max(2);
                    startFrame = start * frames;
                    finishFrame = finish * frames;
                    len = (finishFrame - startFrame).max(2);
                    // One timeline survives changes of playback mode. The two
                    // readers share note/gate/envelope/loop state throughout;
                    // R and T are M's endpoints exactly (stretchmix 0 / 1).
                    step = switch(stretchVariant,
                        0, { speed * (2 ** ((pitch - 60) / 12)) },
                        1, { speed / vm.(28, \stretch.kr(1)).clip(0.125, 8) },
                        { (speed * (2 ** ((pitch - 60) / 12)) * (1 - stretchMix))
                            + (speed / vm.(28, \stretch.kr(1)).clip(0.125, 8) * stretchMix) }) * BufRateScale.kr(buf) * fitRate;
                    pos = Sweep.ar(T2A.ar(voiceTrig), step * SampleRate.ir);  // frames since the note started
                    loopOn = \loop.kr(0) > 0.5;
                    // Sync: the transport's phase over the region (x Rate); a
                    // one-shot starts there and runs on at the fitted speed
                    syncPhase = (tphase * speed).frac;
                    syncLoop = syncPhase * len;
                    syncShot = ((Latch.ar(syncPhase, T2A.ar(voiceTrig)) * len) + pos).min(len - 1);
                    // Loop: the first pass plays the whole region, then it
                    // loops from start + fade, the tail fading into the head.
                    // One-shot: the position stops at the end (it never wraps
                    // back to the start while the release fades).
                    fade = len * (vm.(33, \loopcrossfade.kr(0)).clip(0, 50) * 0.01) * loopOn * (1 - syncOn);
                    loopLen = (len - fade).max(1);
                    offset = Select.ar(loopOn, [
                        pos.min(len - 1),
                        Select.ar(pos < fade, [fade + (pos - fade).mod(loopLen), pos])]);
                    offset = Select.ar(syncOn, [offset, Select.ar(loopOn, [syncShot, syncLoop])]);
                    // how far a one-shot has gone (its end fade, its end)
                    endPos = Select.ar(syncOn, [pos, syncShot]);
                    fadeMix = ((offset - loopLen) / fade.max(1)).clip(0, 1) * (fade > 0);
                    xoff = (offset - loopLen).max(0);
                    rev = \reverse.kr(0) > 0.5;
                    frame = Select.ar(rev, [startFrame + offset, finishFrame - 1 - offset]);
                    xframe = Select.ar(rev, [startFrame + xoff, finishFrame - 1 - xoff]);
                    read = { |b, at|
                        var resampled = { BufRd.ar(1, b, at, 0, 4) };
                        var stretched = { Warp1.ar(1, b, at / frames, 2 ** ((pitch - 60) / 12),
                            vm.(32, \warpwindow.kr(0.15)).clip(0.01, 1), -1, 8, 0, 4) };
                        switch(stretchVariant, 0, resampled, 1, stretched,
                            { (resampled.value * (1 - stretchMix)) + (stretched.value * stretchMix) })
                    };
                    left = read.(buf, frame);
                    left2 = read.(buf, xframe);
                    left = (left * (1 - fadeMix)) + (left2 * fadeMix);
                    if(k == 2) {
                        right = read.(\buf1.kr(0), frame);
                        right2 = read.(\buf1.kr(0), xframe);
                        right = (right * (1 - fadeMix)) + (right2 * fadeMix);
                    };
                    // A one-shot fades out over its last 4 ms, reaching
                    // silence exactly at the region's end.
                    edge = Select.ar(loopOn, [((len - endPos) / (step * SampleRate.ir * 0.004).max(1)).clip(0, 1), DC.ar(1)]);
                    amp = LagUD.ar(K2A.ar(held), vm.(30, NamedControl.kr(\attack,0.005!8)[v]).max(0.001),
                        Select.kr(choke > 0, [vm.(31, NamedControl.kr(\release,0.05!8)[v]).max(0.001), 0.005])) * edge;
                    voicePan = vm.(6, \pan.kr(0)).clip(-1, 1);
                    if(k == 1) {
                        raw = if(n == 1, { [left] }, { PanAz.ar(n, left, voicePan) });
                    } {
                        raw = if(n == 1, { [(left + right) * 0.5] },
                            { PanAz.ar(n, left, voicePan - 0.5) + PanAz.ar(n, right, voicePan + 0.5) });
                    };
                    // Gain: 1, moved per voice by the modulation (the pad's
                    // Level is the mixer's)
                    raw = raw * amp * vm.(5, \gain.kr(1)).max(0) * Latch.kr(level, voiceTrig) *
                        vm.(29, Latch.kr(\velocity.kr(1), voiceTrig)).clip(0, 1) * \loaded.kr(0);
                    // the window's playhead: position 0..1, -1 when silent
                    [raw, Select.kr(A2K.kr(amp) > 0.001, [-1, A2K.kr(frame / frames)]), held, A2K.kr(endPos >= (len - 1)),
                     Latch.kr(\velocity.kr(1) * level, voiceTrig), TRand.kr(-1, 1, voiceTrig)]
                };
                source = voices.collect(_[0]).sum;
                Out.kr(\vis.kr(0), voices.collect(_[1]));
                LocalOut.kr(voices.collect(_[3]));
                // for the modulation: the gates, velocities, a random per note
                Out.ar(\gatebus.kr(0), K2A.ar(voices.collect(_[2])));
                Out.kr(\velbus.kr(0), voices.collect(_[4]));
                Out.kr(\rndbus.kr(0), voices.collect(_[5]));
                fxMod = { |id, base| var t = targetRanges[id]; if(t.notNil) { base + Select.kr(voiceIndex, modArr[t]) } { base } };

                sig = source.asArray;
                // The filter belongs to the sound (its envelopes move it per
                // note); the mixer's effects come after the modulation.
                f = (\cutoff.kr(16000) * (2 ** fxMod.(7, 0))).clip(20, 20000);
                q = fxMod.(8, \resonance.kr(0.7)).clip(0.1, 2);
                filter = { var alternatives = [RLPF.ar(sig, f, q.reciprocal.clip(0.05, 1)),
                     RHPF.ar(sig, f, q.reciprocal.clip(0.05, 1)),
                     BPF.ar(sig, f, q.reciprocal.clip(0.05, 1))];
                    var weights = NamedControl.kr(\filterweights, 0 ! 3).lag(0.03);
                    Select.ar(weights.sum > 0, [Select.ar(\filtertype.kr(0).clip(0, 2), alternatives),
                        Mix(alternatives.collect { |signal, i| signal * weights[i] })])
                }.value;
                sig = XFade2.ar(sig, filter, \filteron.kr(0).lag(0.03) * 2 - 1);
                Out.ar(\out.kr(0), LeakDC.ar(sig));
            }).writeDefFile(dir);
        };
    };
};

// ── The mixer's effects: one synth each, created only while on ─────────────
(1..16).do { |n|
    // Five-band EQ (MixerPro's layout): two shelves and three peaks
    SynthDef(("SampleBoxEQ_" ++ n).asSymbol, {
        var bus = \bus.kr(0), sig = In.ar(bus, n), eq, gains, freqs, shapes, safeShelf;
        gains = Array.fill(5, { |b| NamedControl.kr(("eq" ++ (b + 1)).asSymbol, 0).clip(-24, 24) });
        freqs = Array.fill(5, { |b| NamedControl.kr(("eq" ++ (b + 1) ++ "freq").asSymbol, [80, 250, 1000, 4000, 12000][b]).clip(20, 20000) });
        shapes = Array.fill(5, { |b| NamedControl.kr(("eq" ++ (b + 1) ++ "shape").asSymbol, 1).clip(0.1, if([0, 4].includes(b), { 4 }, { 20 })) });
        safeShelf = { |slope, gain|
            var a = 10 ** (gain / 40);
            var kk = 1 - (1.9 / (a + a.reciprocal));
            slope.min(1 / kk.max(0.0001))
        };
        eq = BLowShelf.ar(sig, freqs[0], safeShelf.(shapes[0], gains[0]).reciprocal, gains[0]);
        eq = BPeakEQ.ar(eq, freqs[1], shapes[1].reciprocal, gains[1]);
        eq = BPeakEQ.ar(eq, freqs[2], shapes[2].reciprocal, gains[2]);
        eq = BPeakEQ.ar(eq, freqs[3], shapes[3].reciprocal, gains[3]);
        eq = BHiShelf.ar(eq, freqs[4], safeShelf.(shapes[4], gains[4]).reciprocal, gains[4]);
        ReplaceOut.ar(bus, LinXFade2.ar(sig, XFade2.ar(sig, eq, \eqmix.kr(1).clip(0, 1) * 2 - 1), \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    SynthDef(("SampleBoxComp_" ++ n).asSymbol, {
        var bus = \bus.kr(0), sig = In.ar(bus, n), threshold, ratio, key, comp;
        threshold = modOf.(12, \compthreshold.kr(-18)).clip(-60, 0);
        ratio = \compratio.kr(3).clip(1, 20);
        key = sig.asArray.abs.reduce(\max);
        comp = Compander.ar(sig, key, threshold.dbamp, 1, ratio.reciprocal,
            \compattack.kr(10).clip(0.1, 200) * 0.001, \comprelease.kr(150).clip(1, 2000) * 0.001) *
            \compmakeup.kr(0).clip(-12, 24).dbamp;
        ReplaceOut.ar(bus, LinXFade2.ar(sig, comp, \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    SynthDef(("SampleBoxSat_" ++ n).asSymbol, {
        var bus = \bus.kr(0), sig = In.ar(bus, n), drive, sat;
        drive = modOf.(11, \satdrive.kr(6)).clip(0, 36).dbamp;
        sat = (sig * drive).tanh / drive.tanh.max(0.001);
        ReplaceOut.ar(bus, LinXFade2.ar(sig, XFade2.ar(sig, sat, \satmix.kr(1).clip(0, 1) * 2 - 1), \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    // Echo: Time in beats (\bpm), feedback through a lowpass (Tone); Spread
    // turns the repeats across the channels (ping-pong). Amount: the
    // repeats' level, added to the dry sound.
    SynthDef(("SampleBoxEcho_" ++ n).asSymbol, {
        var bus = \bus.kr(0), sig = In.ar(bus, n), time, fb, loop, wet, spread;
        time = (\echotime.kr(0.75) * 60 / \bpm.kr(120).max(1)).clip(0.001, 4).lag(0.1);
        spread = \echospread.kr(0).clip(0, 1);
        fb = LocalIn.ar(n).asArray;
        loop = sig.asArray + (fb * \echofeedback.kr(0.4).clip(0, 0.95));
        wet = DelayC.ar(loop, 4, time);
        wet = LPF.ar(wet, \echotone.kr(6000).clip(200, 20000));
        wet = LeakDC.ar(wet).softclip;
        LocalOut.ar(if(n > 1, { (wet * (1 - spread)) + (wet.rotate(1) * spread) }, { wet }));
        ReplaceOut.ar(bus, LinXFade2.ar(sig, sig + (wet * \echoamount.kr(0.35).clip(0, 1)), \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    SynthDef(("SampleBoxReverb_" ++ n).asSymbol, {
        var bus = \bus.kr(0), sig = In.ar(bus, n);
        ReplaceOut.ar(bus, LinXFade2.ar(sig, OceanodeMicroSpace.box(sig.asArray,
            \revsize.kr(30).clip(0, 60), \revdecay.kr(4).clip(0.05, 60), \revpredelay.kr(20).clip(0, 1000),
            \revlowpass.kr(10000).clip(20, 20000), \revposition.kr(0.8).clip(0, 1), \revspread.kr(0.15).clip(-1, 1),
            modOf.(13, \revmix.kr(0.33)).clip(0, 1)), \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    // MixerPro's generalized mid/side: zero-sum difference and zero-sum
    // decorrelated mid preserve the multichannel mean
    if(n > 1) {
        SynthDef(("SampleBoxImage_" ++ n).asSymbol, {
            var bus = \bus.kr(0), sig = In.ar(bus, n), mid, side, dcor, bass, bf;
            bf = \bassfreq.kr(150).clip(20, 1000);
            mid = sig.sum / n;
            bass = LPF.ar(LPF.ar(mid, bf), bf);
            side = sig.collect { |channel| channel - mid };
            side = side.collect { |channel| channel - (LPF.ar(LPF.ar(channel, bf), bf) * \bassmono.kr(0).clip(0, 1)) };
            dcor = Array.fill(n, { |i| AllpassN.ar(mid - bass, 0.02, 0.002 + (i * 0.0011), 0.02) });
            dcor = dcor - (dcor.sum / n);
            ReplaceOut.ar(bus, LinXFade2.ar(sig, Array.fill(n, { |i|
                mid + (side[i] * modOf.(14, \width.kr(1)).clip(0, 2)) + (dcor[i] * \diffuse.kr(0).clip(0, 1))
            }), \on.kr(1, 0.03) * 2 - 1));
        }).writeDefFile(dir);
    };

    // The channel strip: Level x the node's Gain, muted (Mute, Solo), into
    // the mix and the pad's own output; the peak for the meter
    SynthDef(("SampleBoxStrip_" ++ n).asSymbol, {
        var sig = In.ar(\bus.kr(0), n), g, mixBus = \mix.kr(-1), padBus = \padout.kr(-1);
        g = (\level.kr(1).clip(0, 4) * \master.kr(0.7).clip(0, 4) * (1 - \mute.kr(0).clip(0, 1))).lag(0.02);
        sig = sig * g;
        // an output the graph has not wired yet: nothing (bus 0 gets zeros added)
        Out.ar(mixBus.max(0), sig * (mixBus >= 0));
        Out.ar(padBus.max(0), sig * (padBus >= 0));
        Out.kr(\meterbus.kr(0), Amplitude.kr(sig.asArray.abs.reduce(\max), 0.002, 0.25));
    }).writeDefFile(dir);
};
"SampleBox SynthDefs written to ".post; dir.postln;
)
