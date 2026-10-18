// SampleBox: disk buffer instrument with an audio-rate trigger/gate input.
// Run this file in sclang; the node loads CompiledSynthdefs/SampleBox.
// K = source channels (1/2), N = output channels (1..16), R = resampling,
// T = Warp1 time stretch (pitch and timeline speed are independent).
(
var root, dir, targetRanges, spaceMaster;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "SampleBox";
File.mkdir(root); File.mkdir(dir);

// Modulation comes from the node's shared modulation (synthdefs/Defaults/
// Modulation): one control bus per target (\modbus + k * 16, 8 channels: one
// per voice), already scaled to the target's units by the routes. These are
// the synth's old target ids -> the node's target k (Pitch, Rate, In, Out,
// Gain, Pan, Stretch, Grain, Loop Crossfade, Cutoff, Resonance, Drive, Comp
// Threshold, Reverb Mix, Width). The voices write their gates (audio, 8), each
// note's velocity and a random -1..1 (control, 8) for the modulation.
targetRanges = Dictionary[1 -> 0, 2 -> 1, 3 -> 2, 4 -> 3, 5 -> 4, 6 -> 5, 28 -> 6, 32 -> 7, 33 -> 8,
    7 -> 9, 8 -> 10, 19 -> 11, 14 -> 12, 23 -> 13, 24 -> 14];

// The same R66/SpaceMaster tank used by GrainBox (spaceMaster.scd), embedded
// after the sampler's other effects so its controls can receive envelopes.
// One control value is shared by all channels, as in GrainBox's global FX.
spaceMaster = { |input, n, size, decay, predelay, lowpass, position, spread, mix, on|
    var feedback, results, pairMods, tick, p2d, allpass, onePoleLP, damp;
    var maxPredelay = 2.0, maxEarly = 0.2, maxLate = 0.25;
    var modfreq = 0.2, spin = 1, wander = 0.25;
    p2d = { |pitch| pitch.midicps.reciprocal };
    allpass = { |ugen, signal, seconds, coefficient, maxDelay|
        var delay = (seconds + SampleDur.ir).clip(SampleDur.ir, maxDelay);
        var coef = coefficient.clip(0.0001, 0.9999);
        ugen.ar(signal, maxDelay, delay, delay * (0.001.log / coef.log) * -1)
    };
    onePoleLP = { |signal, frequency|
        OnePole.ar(signal, (-2 * pi * frequency.clip(5, SampleRate.ir * 0.45) / SampleRate.ir).exp)
    };
    damp = { |signal| BHiShelf.ar(signal, 96.midicps, 1.0, -6) };
    tick = Impulse.ar(400);
    pairMods = Array.fill((n + 1).div(2), {
        Array.fill(4, { |index|
            var sine = Latch.ar(SinOsc.ar(modfreq, index * (-pi / 8)), tick) * spin;
            var random = OnePole.ar(TRand.ar(-1, 1, tick), (-2 * pi * modfreq / SampleRate.ir).exp);
            sine + (Latch.ar(random, tick) * wander)
        })
    });
    feedback = LocalIn.ar(n).asArray;
    results = Array.fill(n, { |ch|
        var left = ch.even, pair = ch.div(2);
        var partner = if(left) { (ch + 1).min(n - 1) } { ch - 1 };
        var side = if(partner == ch) { 0 } { if(left) { 1 } { -1 } };
        var earlyPitch = 65 - size, latePitch = 30 - size;
        var dffs = (0.5 + (size / 240)).clip(0.0001, 0.999);
        var early, tank, mods, undivided, physical, late, fbDelay, fbGain, wet;
        var wetMix = mix * on;
        early = DelayN.ar(input[ch] * on, maxPredelay,
            (predelay * 0.001 * (1 + (spread * side))).clip(0, maxPredelay));
        (if(left) { [0, -4, -8] } { [-2, -6, -10] }).do { |offset|
            early = allpass.(AllpassN, early, p2d.(earlyPitch + offset), dffs, maxEarly)
        };
        early = onePoleLP.(early, lowpass);
        tank = damp.(early + feedback[partner]);
        mods = if(left) { pairMods[pair] } { pairMods[pair].collect { |signal| signal.neg } };
        undivided = Array.fill(4, { |index|
            (p2d.(latePitch + (if(left) { [8, 4, 0, -7] } { [10, 6, 2, -5] })[index]) +
                (mods[index] * 0.001)).max(SampleDur.ir * 5)
        });
        physical = undivided / 5;
        late = tank;
        3.do { |index| late = allpass.(AllpassC, late, physical[index], dffs, maxLate) };
        fbDelay = (physical[3] - ControlDur.ir).clip(SampleDur.ir, maxLate);
        // Match SpaceMaster's feedback gain, including its measured RT60.
        fbGain = (-60 * undivided[3] / decay).clip(-60, 0).dbamp.clip(0, 0.9999);
        wet = (early * (1 - position).sqrt) + (late * position.sqrt);
        [(input[ch] * (1 - wetMix).sqrt) + (LeakDC.ar(wet, 0.9995) * wetMix.sqrt),
         DelayC.ar(late, maxLate, fbDelay) * fbGain * on]
    });
    LocalOut.ar(results.collect { |channel| channel[1] });
    results.collect { |channel| channel[0] }
};

(1..2).do { |k|
    (1..16).do { |n|
        [0, 1].do { |stretchVariant|
            var name = "SampleBox_" ++ k ++ "_" ++ n ++ if(stretchVariant == 0, { "_R" }, { "_T" });
            SynthDef(name.asSymbol, {
                var trigBus, audioGate, audioTrig, fireTrig, trig, gate, stopTrig, level;
                var buf, rateBase, inBase, outBase, polyMode, voiceLimit, voiceIndex;
                var source, voices, finished, sig, modArr, fxMod;
                var filter, eq, comp, sat, mid, side, dcor, bass;
                var f, q, key, threshold, ratio, drive;
                var gains, freqs, shapes, safeShelf;

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
                buf = \buf0.kr(0);
                rateBase = \rate.kr(1).clip(0.125, 8);
                inBase = \inpoint.kr(0).clip(0, 1);
                outBase = \outpoint.kr(1).clip(0, 1);
                polyMode = \poly.kr(1) > 0.5;
                voiceLimit = \voices.kr(8).clip(1, 8);
                // Polyphonic: round robin over Max Voices. Mono: two voices
                // take turns, so a retrigger fades the old note out (choke)
                // instead of jumping it back to the start (a click).
                voiceIndex = PulseCount.kr(trig) % Select.kr(polyMode, [2, voiceLimit]);

                // the modulation, per target (8 voices each)
                modArr = 15.collect { |k| In.kr(\modbus.kr(0) + (k * 16), 8) };

                // which one-shots reached their end (last block): their note ends
                finished = LocalIn.kr(8);
                voices = 8.collect { |v|
                    var voiceTrig, choke, held, vm, pitch, speed, step, start, finish;
                    var frames, startFrame, finishFrame, len, loopOn, fade, loopLen, pos, offset;
                    var fadeMix, xoff, rev, frame, xframe, edge, amp, voicePan, window;
                    var left, right, left2, right2, read, raw;
                    voiceTrig = trig * ((voiceIndex - v).abs < 0.5) * (Select.kr(polyMode, [2, voiceLimit]) > v);
                    // another note took over in mono mode: fade out fast
                    choke = SetResetFF.kr(trig * (1 - voiceTrig) * (1 - polyMode), voiceTrig);
                    held = SetResetFF.kr(voiceTrig, stopTrig + (choke > 0) + (finished[v] * (\loop.kr(0) < 0.5))) *
                        Select.kr(\gatemode.kr(0) > 0.5, [1, gate > 0]);
                    // this voice's modulation of a target (the old ids; none: the base)
                    vm = { |id, base| var k = targetRanges[id]; if(k.notNil) { base + modArr[k][v] } { base } };
                    // Latch the note and velocity per voice: a new note must
                    // not retune or re-level voices already sounding.
                    pitch = vm.(1, Latch.kr(\pitch.kr(60), voiceTrig)).clip(0, 127);
                    // Transpose is live (not latched): it moves notes already
                    // sounding. Stretch: only the grains' pitch, the
                    // timeline keeps its speed. Resample: speed and pitch.
                    pitch = pitch + \transpose.kr(0).clip(-48, 48).lag(0.03);
                    speed = vm.(2, rateBase).clip(0.01, 16);
                    start = vm.(3, inBase).clip(0, 0.9999);
                    finish = vm.(4, outBase).clip(start + 0.00001, 1);
                    frames = BufFrames.kr(buf).max(2);
                    startFrame = start * frames;
                    finishFrame = finish * frames;
                    len = (finishFrame - startFrame).max(2);
                    // file frames per output sample: Resample couples speed
                    // and pitch, Stretch moves the read pointer at
                    // Rate / Stretch and pitches the grains on their own
                    step = if(stretchVariant == 0,
                        { speed * (2 ** ((pitch - 60) / 12)) },
                        { speed / vm.(28, \stretch.kr(1)).clip(0.125, 8) }) * BufRateScale.kr(buf);
                    pos = Sweep.ar(T2A.ar(voiceTrig), step * SampleRate.ir);  // frames since the note started
                    loopOn = \loop.kr(0) > 0.5;
                    // Loop: the first pass plays the whole region, then it
                    // loops from start + fade, the tail fading into the head.
                    // One-shot: the position stops at the end (it never wraps
                    // back to the start while the release fades).
                    fade = len * (vm.(33, \loopcrossfade.kr(0)).clip(0, 50) * 0.01) * loopOn;
                    loopLen = (len - fade).max(1);
                    offset = Select.ar(loopOn, [
                        pos.min(len - 1),
                        Select.ar(pos < fade, [fade + (pos - fade).mod(loopLen), pos])]);
                    fadeMix = ((offset - loopLen) / fade.max(1)).clip(0, 1) * (fade > 0);
                    xoff = (offset - loopLen).max(0);
                    rev = \reverse.kr(0) > 0.5;
                    frame = Select.ar(rev, [startFrame + offset, finishFrame - 1 - offset]);
                    xframe = Select.ar(rev, [startFrame + xoff, finishFrame - 1 - xoff]);
                    read = { |b, at|
                        if(stretchVariant == 0, { BufRd.ar(1, b, at, 0, 4) }, {
                            Warp1.ar(1, b, at / frames, 2 ** ((pitch - 60) / 12),
                                vm.(32, \warpwindow.kr(0.15)).clip(0.01, 1), -1, 8, 0, 4)
                        })
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
                    edge = Select.ar(loopOn, [((len - pos) / (step * SampleRate.ir * 0.004).max(1)).clip(0, 1), DC.ar(1)]);
                    amp = LagUD.ar(K2A.ar(held), vm.(30, \attack.kr(0.005)).max(0.001),
                        Select.kr(choke > 0, [vm.(31, \release.kr(0.05)).max(0.001), 0.005])) * edge;
                    voicePan = vm.(6, \pan.kr(0)).clip(-1, 1);
                    if(k == 1) {
                        raw = if(n == 1, { [left] }, { PanAz.ar(n, left, voicePan) });
                    } {
                        raw = if(n == 1, { [(left + right) * 0.5] },
                            { PanAz.ar(n, left, voicePan - 0.5) + PanAz.ar(n, right, voicePan + 0.5) });
                    };
                    raw = raw * amp * vm.(5, \gain.kr(0.7)).max(0) * Latch.kr(level, voiceTrig) *
                        vm.(29, Latch.kr(\velocity.kr(1), voiceTrig)).clip(0, 1) * \loaded.kr(0);
                    // the window's playhead: position 0..1, -1 when silent
                    [raw, Select.kr(A2K.kr(amp) > 0.001, [-1, A2K.kr(frame / frames)]), held, A2K.kr(pos >= (len - 1)),
                     Latch.kr(\velocity.kr(1) * level, voiceTrig), TRand.kr(-1, 1, voiceTrig)]
                };
                source = voices.collect(_[0]).sum;
                Out.kr(\vis.kr(0), voices.collect(_[1]));
                LocalOut.kr(voices.collect(_[3]));
                // for the shared modulation: the gates, velocities, a random per note
                Out.ar(\gatebus.kr(0), K2A.ar(voices.collect(_[2])));
                Out.kr(\velbus.kr(0), voices.collect(_[4]));
                Out.kr(\rndbus.kr(0), voices.collect(_[5]));
                // the effects follow the latest note's modulation
                fxMod = { |id, base| var k = targetRanges[id]; if(k.notNil) { base + Select.kr(voiceIndex, modArr[k]) } { base } };

                sig = source.asArray;

                // Filter and five-band EQ use MixerPro's band layout.
                f = (\cutoff.kr(16000) * (2 ** fxMod.(7, 0))).clip(20, 20000);
                q = fxMod.(8, \resonance.kr(0.7)).clip(0.1, 2);
                filter = Select.ar(\filtertype.kr(0).clip(0, 2),
                    [RLPF.ar(sig, f, q.reciprocal.clip(0.05, 1)),
                     RHPF.ar(sig, f, q.reciprocal.clip(0.05, 1)),
                     BPF.ar(sig, f, q.reciprocal.clip(0.05, 1))]);
                sig = XFade2.ar(sig, filter, \filteron.kr(0).lag(0.03) * 2 - 1);

                // GrainBox's five-band GraphicEQ: two shelves and three
                // parametric peaks, with draggable frequency and shape.
                gains = Array.fill(5, { |b| fxMod.(9 + b,
                    NamedControl.kr(("eq" ++ (b + 1)).asSymbol, 0)).clip(-24, 24) });
                freqs = Array.fill(5, { |b| fxMod.(38 + b,
                    NamedControl.kr(("eq" ++ (b + 1) ++ "freq").asSymbol,
                        [80, 250, 1000, 4000, 12000][b])).clip(20, 20000) });
                shapes = Array.fill(5, { |b| fxMod.(43 + b,
                    NamedControl.kr(("eq" ++ (b + 1) ++ "shape").asSymbol, 1)).clip(0.1,
                        if([0, 4].includes(b), { 4 }, { 20 })) });
                safeShelf = { |slope, gain|
                    var a = 10 ** (gain / 40);
                    var k = 1 - (1.9 / (a + a.reciprocal));
                    slope.min(1 / k.max(0.0001))
                };
                eq = BLowShelf.ar(sig, freqs[0], safeShelf.(shapes[0], gains[0]).reciprocal, gains[0]);
                eq = BPeakEQ.ar(eq, freqs[1], shapes[1].reciprocal, gains[1]);
                eq = BPeakEQ.ar(eq, freqs[2], shapes[2].reciprocal, gains[2]);
                eq = BPeakEQ.ar(eq, freqs[3], shapes[3].reciprocal, gains[3]);
                eq = BHiShelf.ar(eq, freqs[4], safeShelf.(shapes[4], gains[4]).reciprocal, gains[4]);
                eq = XFade2.ar(sig, eq, fxMod.(48, \eqmix.kr(1)).clip(0, 1) * 2 - 1);
                sig = XFade2.ar(sig, eq, \eqon.kr(0).lag(0.03) * 2 - 1);

                threshold = fxMod.(14, \compthreshold.kr(-18)).clip(-60, 0);
                ratio = fxMod.(15, \compratio.kr(3)).clip(1, 20);
                key = sig.abs.reduce(\max);
                comp = Compander.ar(sig, key, threshold.dbamp, 1, ratio.reciprocal,
                    fxMod.(16, \compattack.kr(10)).clip(0.1, 200) * 0.001,
                    fxMod.(17, \comprelease.kr(150)).clip(1, 2000) * 0.001) *
                    fxMod.(18, \compmakeup.kr(0)).clip(-12, 24).dbamp;
                sig = XFade2.ar(sig, comp, \compon.kr(0).lag(0.03) * 2 - 1);

                drive = fxMod.(19, \satdrive.kr(6)).clip(0, 36).dbamp;
                sat = (sig * drive).tanh / drive.tanh.max(0.001);
                sat = XFade2.ar(sig, sat, fxMod.(20, \satmix.kr(1)).clip(0, 1) * 2 - 1);
                sig = XFade2.ar(sig, sat, \saton.kr(0).lag(0.03) * 2 - 1);

                sig = spaceMaster.(sig.asArray, n,
                    fxMod.(21, \revsize.kr(30)).clip(0, 60),
                    fxMod.(22, \revdecay.kr(4)).clip(0.05, 60),
                    fxMod.(34, \revpredelay.kr(20)).clip(0, 1000),
                    fxMod.(35, \revlowpass.kr(10000)).clip(20, 20000),
                    fxMod.(36, \revposition.kr(0.8)).clip(0, 1),
                    fxMod.(37, \revspread.kr(0.15)).clip(-1, 1),
                    fxMod.(23, \revmix.kr(0.33)).clip(0, 1),
                    \reverbon.kr(0).lag(0.03));

                // MixerPro's generalized mid/side: zero-sum difference and
                // zero-sum decorrelated mid preserve the multichannel mean.
                if(n > 1) {
                    mid = sig.sum / n;
                    bass = LPF.ar(LPF.ar(mid, fxMod.(27, \bassfreq.kr(150)).clip(20, 1000)),
                        fxMod.(27, \bassfreq.kr(150)).clip(20, 1000));
                    side = sig.collect { |channel| channel - mid };
                    side = side.collect { |channel|
                        channel - (LPF.ar(LPF.ar(channel, \bassfreq.kr(150)), \bassfreq.kr(150)) *
                            fxMod.(26, \bassmono.kr(0)).clip(0, 1))
                    };
                    dcor = Array.fill(n, { |i|
                        AllpassN.ar(mid - bass, 0.02, 0.002 + (i * 0.0011), 0.02)
                    });
                    dcor = dcor - (dcor.sum / n);
                    sig = Array.fill(n, { |i|
                        mid + (side[i] * fxMod.(24, \width.kr(1)).clip(0, 2)) +
                            (dcor[i] * fxMod.(25, \diffuse.kr(0)).clip(0, 1))
                    });
                };
                Out.ar(\out.kr(0), LeakDC.ar(sig));
            }).writeDefFile(dir);
        };
    };
};
"SampleBox SynthDefs written to ".post; dir.postln;
)
