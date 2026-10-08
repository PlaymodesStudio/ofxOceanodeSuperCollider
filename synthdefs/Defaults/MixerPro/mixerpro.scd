// SC Mixer Pro SynthDefs
//
//   mixerProTrackN  -- one mixer channel. polyMixerTrackN (polymixer.scd)
//                      plus sidechain: same balance law, level / masterLevel
//                      per channel, mute, NaN protection and VU.
//
// Sidechain. Every track writes, after its numChannels VU channels on vuBus:
//   vuBus+N     key: the instant envelope of its signal (max of the channels,
//               after balance, before fader and mute -- a muted track still
//               keys, the "ghost kick" trick)
//   vuBus+N+1   the same with a 50 ms hold, read by the node at frame rate
// A ducked track reads its key either from scBus (source on the same server:
// one block of latency at most) or from scKey (source on another server: the
// node relays the held key every frame). The key is scaled so that it reaches
// 1 at scThresh (linear; a key at or above the threshold ducks fully), then
// LagUD (scAttack / scRelease, ms) and gain = 1 - scStrength * env. With
// scStrength 0 the gain is 1.
//   mixerProEQN     -- per-channel insert: 5-band EQ (graphiceqN's bands)
//   mixerProDCN     -- per-channel insert: DC correction (LeakDC)
//   mixerProEQDCN   -- both
//
// The inserts are separate synths so that a channel without EQ / DC costs
// nothing: the node creates an insert only when one is switched on, places it
// just before the channel synth, and pauses it (n_run 0) when both are off.
//
//   mixerProCompN   -- per-channel insert: compressor (CompressorPro core:
//                      log-domain soft knee, per-sample ballistics, linked
//                      detector with sidechain HPF, peak/RMS, auto makeup,
//                      parallel mix, no lookahead so tracks stay aligned).
//                      \on (lagged) crossfades to dry before the node pauses
//                      it. Gain reduction (dB) is written to grBus
//                      (= vuBus + N + 2).
//   mixerProRvbN    -- per-channel insert: SpaceMaster reverb (pairwise
//                      cross-feedback tank). \on gates the tank input only:
//                      switching it off lets the tail ring out, then the node
//                      pauses it.
//   mixerProHPN     -- per-channel insert: high-pass, 12 / 24 dB/oct
//   mixerProSatN    -- per-channel insert: soft saturation (anti-aliased tanh)
//   mixerProEchoN   -- per-channel insert: GrainBox's Echo, tempo-synced by the
//                      node (Delay in seconds)
// Chain: adapter -> HP -> EQ/DC -> Comp -> Sat -> Echo -> Rvb -> track. Every
// insert is created the first time it is switched on and paused while off.
//
// Aux sends: the track synth writes post- and pre-fader taps (2N) to \sendBus
// (-1: none, free); mixerProSendN, created for strips with a send, adds them
// into the return strips' buses. Returns are strips like the submasters.
//
// Master section (per server, only while one of its stages is on): the strips
// write into a master bus instead of the output, then
//   mixerProHPN -> mixerProEQN -> mixerProMaxN -> mixerProLimN -> mixerProMasterOutN
// (MasterOut copies to the output and meters it).
//
//   mixerProAdaptM_N -- channel adapter, M inputs to N outputs through a gain
//                      matrix (\matrix, M*N gains, row-major: input i to
//                      output j at i*N+j, lagged). The node computes the matrix
//                      for every mode (direct, wrap, blocks, stretch, rotate)
//                      and only creates the adapter for tracks that need it.
//
// Output: <this folder>/CompiledSynthdefs/mixerpro. Copy that folder anywhere
// under bin/data/Supercollider/Synthdefs (the node finds it by searching).
(
var d, dir, channelCounts;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
dir = d +/+ "/mixerpro";
File.mkdir(d);
File.mkdir(dir);

channelCounts = (1..16);

channelCounts.do({|numChannels|

    // ---- Channel (verbatim polyMixerTrack, renamed) ----
    SynthDef(("mixerProTrack" ++ numChannels).asSymbol, {
        arg in=0, out=0, mute=0, balance=0, vuBus=(-1);
        var sig, level, masterLevel, vuSig, processedSig, inputSig, cleaned;
        var vuAttackTime, vuReleaseTime;
        var normPositions, rightFavor, leftFavor, balanceGains;
        var balanceSq, panPos, leftGain, rightGain;
        var key, scBus, scEnv, duck, sendBus;

        // --- INPUT ---
        inputSig = In.ar(in, numChannels);
        sig = Gate.ar(inputSig, (in >= 0));

        // --- PARAMETERS ---
        level = NamedControl.kr(\level, Array.fill(numChannels, 0.75));
        masterLevel = NamedControl.kr(\masterLevel, Array.fill(numChannels, 1.0));
        vuAttackTime = NamedControl.kr(\vuAttackTime, 10.0) * 0.001;
        vuReleaseTime = NamedControl.kr(\vuReleaseTime, 300.0) * 0.001;

        // --- BALANCE LOGIC ---
        balanceGains = if(numChannels == 2, {
            panPos = balance * 0.5 + 0.5;
            leftGain = (1 - panPos).sqrt;
            rightGain = panPos.sqrt;
            [leftGain, rightGain];
        }, {
            normPositions = Array.series(numChannels, 0, 1/(numChannels-1).max(1));
            balanceSq = balance.sign * (balance.abs.squared);
            leftFavor = 1.0 - (normPositions * balanceSq.min(0).abs);
            rightFavor = 1.0 - ((1.0 - normPositions) * balanceSq.max(0));
            (leftFavor * rightFavor).squared;
        });

        sig = sig * balanceGains;

        // --- SIDECHAIN KEY (this track as a source) ---
        key = Amplitude.kr(sig).asArray.reduce(\max);
        key = Select.kr(CheckBadValues.kr(key, 0, 0) > 0, [key, 0]);
        Out.kr(Select.kr(vuBus >= 0, [-1, vuBus + numChannels]), [key, LagUD.kr(key, 0, 0.05)]);

        // --- SIDECHAIN DUCKING (this track as a target) ---
        scBus = \scBus.kr(-1);
        scEnv = Select.kr(scBus >= 0, [\scKey.kr(0), In.kr(scBus.max(0), 1)]);
        scEnv = Select.kr(CheckBadValues.kr(scEnv, 0, 0) > 0, [scEnv, 0]);
        scEnv = LagUD.kr((scEnv / \scThresh.kr(0.125).max(0.0001)).clip(0, 1), \scAttack.kr(10) * 0.001, \scRelease.kr(200) * 0.001);
        duck = 1 - (\scStrength.kr(0).clip(0, 1) * scEnv);

        // --- LEVELS & OUTPUT ---
        processedSig = sig * level.lag(0.05) * duck;
        processedSig = processedSig * (1 - mute);

        // --- NaN/inf PROTECTION --- (Select: NaN * 0 is still NaN)
        cleaned = numChannels.collect({ |i|
            var ch = processedSig.asArray[i];
            Select.ar(CheckBadValues.ar(ch, id: i, post: 0) > 0, [ch, DC.ar(0)]);
        });

        // --- AUX SEND TAPS --- post-fader (before the master level) and
        // pre-fader (after balance and mute), 2N channels on sendBus, read by
        // mixerProSendN. scsynth's Out does NOT ignore a negative bus (it only
        // checks the top: Out.ar(-1, 2N) writes from bus 0, the hardware
        // outputs), so with no sends (sendBus -1) it adds silence to bus 0.
        sendBus = \sendBus.kr(-1);
        Out.ar(sendBus.max(0), (cleaned ++ (sig * (1 - mute)).asArray) * (sendBus >= 0));

        // VU on the sanitized signal
        vuSig = LagUD.kr(Amplitude.kr(cleaned), vuAttackTime, vuReleaseTime);
        Out.kr(vuBus, vuSig.max(0).min(2) * (vuBus >= 0));

        processedSig = cleaned * masterLevel.lag(0.05);
        Out.ar(out, processedSig);
    }).writeDefFile(dir);

    // ---- Inserts ----
    [
        [\mixerProEQ, true, false],
        [\mixerProDC, false, true],
        [\mixerProEQDC, true, true]
    ].do({|variant|
        var withEQ = variant[1], withDC = variant[2];
        SynthDef((variant[0] ++ numChannels).asSymbol, {
            arg in=0, out=0;
            var sig = In.ar(in, numChannels);
            if(withEQ, {
                // graphiceqN's bands: gains in dB, frequencies in Hz,
                // shelves take rs (= 1/slope), peaks rq (= 1/Q)
                sig = BLowShelf.ar(sig, \b1freq.kr(80),    \b1slope.kr(1), \b1gain.kr(0));
                sig = BPeakEQ.ar(sig,   \b2freq.kr(250),   \b2rq.kr(1),    \b2gain.kr(0));
                sig = BPeakEQ.ar(sig,   \b3freq.kr(1000),  \b3rq.kr(1),    \b3gain.kr(0));
                sig = BPeakEQ.ar(sig,   \b4freq.kr(4000),  \b4rq.kr(1),    \b4gain.kr(0));
                sig = BHiShelf.ar(sig,  \b5freq.kr(12000), \b5slope.kr(1), \b5gain.kr(0));
            });
            if(withDC, {
                sig = LeakDC.ar(sig, 0.9995);   // ~4 Hz (0.995 is a 38 Hz high-pass at 48 kHz)
            });
            Out.ar(out, sig);
        }).writeDefFile(dir);
    });

    // ---- Compressor insert ----
    SynthDef(("mixerProComp" ++ numChannels).asSymbol, {
        arg in=0, out=0, grBus=(-1);
        var sig, key, pk, rms, lvl, over, slope, xk, gr, grs, gain;
        var thr, ratio, knee, att, rel, makeup, auto, rmsAmt, mix, on, range = 40;
        sig    = In.ar(in, numChannels).asArray;
        thr    = \threshold.kr(-18, 0.05);
        ratio  = \ratio.kr(3, 0.05).max(1);
        knee   = \knee.kr(6, 0.05).max(0.01);
        att    = \attack.kr(10).max(0.01) * 0.001;
        rel    = \release.kr(150).max(1) * 0.001;
        makeup = \makeup.kr(0, 0.05);
        auto   = \automakeup.kr(0);
        rmsAmt = \rms.kr(0, 0.05);
        mix    = \mix.kr(1, 0.05);
        on     = \on.kr(1, 0.03);

        // detector: HPF, peak/RMS blend (RMS crest-normalised), linked (loudest channel)
        key = HPF.ar(sig, \schpf.kr(20).clip(10, 2000));
        pk  = key.abs;
        rms = OnePole.ar(key.squared, exp(-1 / (0.01 * SampleRate.ir))).sqrt * 2.sqrt;
        lvl = (pk + ((rms - pk) * rmsAmt)).max(-120.dbamp).reduce(\max).ampdb;

        // soft-knee gain computer (dB) and smooth decoupled ballistics
        over  = lvl - thr;
        slope = 1 - ratio.reciprocal;
        xk    = (over + (knee * 0.5)).clip(0, knee);
        gr    = (slope * ((xk.squared / (2 * knee)) + (over - (knee * 0.5)).max(0))).min(range);
        grs   = Lag.ar(LagUD.ar(gr, 0, rel * 6.9078), att * 6.9078);

        xk   = (thr.neg + (knee * 0.5)).clip(0, knee);
        gain = makeup + (auto * 0.5 * (slope * ((xk.squared / (2 * knee)) + (thr.neg - (knee * 0.5)).max(0))).min(range));
        gain = (gain - grs).dbamp;

        Out.kr(Select.kr(grBus >= 0, [-1, grBus]), A2K.kr(grs));
        Out.ar(out, LinXFade2.ar(sig, sig * gain, (mix * on) * 2 - 1));
    }).writeDefFile(dir);

    // ---- Reverb insert (SpaceMaster) ----
    // See SYNTHDEFS/Effect/Time/Reverb/spaceMaster.scd for the reconstruction
    // notes. Strip version: one value per control (no per-channel vectors),
    // buffers sized to what the controls can reach, interpolated reads on the
    // modulated delays, DC removal on the wet only.
    SynthDef(("mixerProRvb" ++ numChannels).asSymbol, {
        arg in=0, out=0;
        var n = numChannels, input, gate, wetIn, localFeedback, results;
        var predelay, spread, size, decay, lowpass, highdamp, lowdamp, modfreq, spin, wander, position, mix;
        var pairCount, pairMods, tick, p2d, allpass, onePoleLP, damp;
        var maxPredelay = 1.0, maxEarly = 0.2, maxLate = 0.25;

        input    = In.ar(in, n).asArray;
        gate     = Lag.ar(K2A.ar(\on.kr(1)), 0.02);
        predelay = \predelay.kr(20, 0.05).clip(0, 500) * 0.001;
        spread   = \spread.kr(0.15, 0.05).clip(-1, 1);
        size     = \size.kr(30, 0.05).clip(0, 60);
        decay    = \decay.kr(2.5, 0.05).max(0.05);
        lowpass  = \lowpass.kr(10000, 0.05);
        highdamp = \highdamp.kr(6, 0.05).clip(0, 60);
        lowdamp  = \lowdamp.kr(0, 0.05).clip(0, 60);
        modfreq  = \modfreq.kr(0.2, 0.05).clip(0.001, 20);
        spin     = \spin.kr(1, 0.05).clip(0, 20);
        wander   = \wander.kr(0.25, 0.05).clip(0, 20);
        position = \position.kr(0.8, 0.05).clip(0, 1);
        mix      = \mix.kr(0.25, 0.05).clip(0, 1);

        pairCount = (n + 1).div(2);
        p2d = { |pitch| pitch.midicps.reciprocal };
        // Reaktor diffuser y = g x + x[n-D] - g y[n-D] (negative decay time flips SC's sign)
        allpass = { |ugen, signal, delaySeconds, coefficient, maxDelay|
            var delay = (delaySeconds + SampleDur.ir).clip(SampleDur.ir, maxDelay);
            var coef = coefficient.clip(0.0001, 0.9999);
            ugen.ar(signal, maxDelay, delay, delay * (0.001.log / coef.log) * -1)
        };
        onePoleLP = { |signal, frequency|
            OnePole.ar(signal, (-2 * pi * frequency.clip(5, SampleRate.ir * 0.45) / SampleRate.ir).exp)
        };
        damp = { |signal|
            BLowShelf.ar(BHiShelf.ar(signal, 96.midicps, 1.0, highdamp.neg), 60.midicps, 1.0, lowdamp.neg)
        };

        tick = Impulse.ar(400);
        pairMods = Array.fill(pairCount, {
            Array.fill(4, { |modIndex|
                var sine = Latch.ar(SinOsc.ar(modfreq, modIndex * (-pi / 8)), tick) * spin;
                var rnd = OnePole.ar(TRand.ar(-1.0, 1.0, tick), (-2 * pi * modfreq / SampleRate.ir).exp);
                sine + (Latch.ar(rnd, tick) * wander)
            })
        });

        localFeedback = LocalIn.ar(n).asArray;
        results = Array.fill(n, { |ch|
            var isLeft = ch.even, pair = ch.div(2);
            var partner = if(isLeft) { (ch + 1).min(n - 1) } { ch - 1 };
            var side = if(partner == ch) { 0 } { if(isLeft) { 1 } { -1 } };
            var earlyPitch = 65 - size, latePitch = 30 - size;
            var dffs = (0.5 + (size / 240)).clip(0.0001, 0.999);
            var early, tank, mods, undivided, physical, late, fbDelay, fbGain, wet;

            early = DelayN.ar(input[ch] * gate, maxPredelay, (predelay * (1 + (spread * side))).clip(0, maxPredelay));
            (if(isLeft) { [0, -4, -8] } { [-2, -6, -10] }).do { |o|
                early = allpass.(AllpassN, early, p2d.(earlyPitch + o), dffs, maxEarly)
            };
            early = onePoleLP.(early, lowpass);

            tank = damp.(early + localFeedback[partner]);
            mods = if(isLeft) { pairMods[pair] } { pairMods[pair].collect(_.neg) };
            undivided = Array.fill(4, { |k|
                (p2d.(latePitch + (if(isLeft) { [8, 4, 0, -7] } { [10, 6, 2, -5] })[k]) + (mods[k] * 0.001)).max(SampleDur.ir * 5)
            });
            physical = undivided / 5;
            late = tank;
            3.do { |k| late = allpass.(AllpassC, late, physical[k], dffs, maxLate) };

            // one-block LocalIn/LocalOut latency compensated
            fbDelay = (physical[3] - ControlDur.ir).clip(SampleDur.ir, maxLate);
            // Decay is RT60: the gain is applied once per pass through the four
            // stages (SpaceMaster uses 5x the last stage, which gives ~0.53 x Decay)
            fbGain  = (-60 * physical.sum / decay).clip(-60, 0).dbamp.clip(0, 0.9999);

            wet = (early * (1 - position).sqrt) + (late * position.sqrt);
            [(input[ch] * (1 - (mix * gate)).sqrt) + (LeakDC.ar(wet, 0.9995) * mix.sqrt),
             DelayC.ar(late, maxLate, fbDelay) * fbGain]
        });
        LocalOut.ar(results.collect(_[1]));
        Out.ar(out, results.collect(_[0]));
    }).writeDefFile(dir);

    // ---- High-pass insert: 12 or 24 dB/oct Butterworth ----
    SynthDef(("mixerProHP" ++ numChannels).asSymbol, {
        arg in=0, out=0;
        var sig = In.ar(in, numChannels);
        var f = \freq.kr(80, 0.05).clip(10, 20000);
        var y2 = BHiPass.ar(sig, f, 2.sqrt);
        var y4 = BHiPass.ar(BHiPass.ar(sig, f, 0.5412.reciprocal), f, 1.3066.reciprocal);
        Out.ar(out, LinXFade2.ar(sig, Select.ar(\slope.kr(0).clip(0, 1), [y2, y4]), \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    // ---- Soft saturation insert ----
    // tanh with first-order antiderivative anti-aliasing (ADAA: the average of
    // tanh between two samples, (F(u) - F(u1)) / (u - u1), F = log cosh), so
    // drive doesn't fold harmonics back down as much as a plain tanh. Bias adds
    // even harmonics (warmth); Auto Gain keeps small signals at unity.
    SynthDef(("mixerProSat" ++ numChannels).asSymbol, {
        arg in=0, out=0;
        var sig = In.ar(in, numChannels).asArray;
        var drive = \drive.kr(6, 0.05).clip(0, 36).dbamp;
        var bias  = \bias.kr(0, 0.05).clip(0, 1) * 0.5;
        var tb    = bias.tanh;
        var comp  = Select.kr(\autogain.kr(1) > 0.5, [1, (drive * (1 - tb.squared)).reciprocal]);
        var logcosh = { |v| v.abs + (1 + (-2 * v.abs).exp).log - 2.log };   // overflow-safe
        var wet = sig.collect { |x|
            var u  = ((x * drive) + bias).clip(-40, 40);
            var u1 = Delay1.ar(u);
            var du = u - u1;
            var y  = Select.ar(du.abs < 0.01, [(logcosh.(u) - logcosh.(u1)) / du, ((u + u1) * 0.5).tanh]);
            LeakDC.ar(y - tb, 0.9995)
        };
        wet = LPF.ar(wet, \tone.kr(20000, 0.05).clip(500, 20000).min(SampleRate.ir * 0.45)) * comp * \output.kr(0, 0.05).dbamp;
        wet = LinXFade2.ar(sig, wet, \mix.kr(1, 0.05) * 2 - 1);
        Out.ar(out, LinXFade2.ar(sig, wet, \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    // ---- Echo insert (GrainBox's Echo: feedback through LP/HP/BP/Peak filter) ----
    // Differences from echo.scd: the output is the echoes only (echo.scd's wet
    // also held an undelayed filtered copy of the input), the LocalIn/LocalOut
    // block is compensated so Delay is exact, resonance is capped (rq 0 blew up),
    // the loop is soft-limited, Ping-pong cross-feeds stereo pairs. \on gates
    // the input only: the repeats ring out.
    SynthDef(("mixerProEcho" ++ numChannels).asSymbol, {
        arg in=0, out=0;
        var n = numChannels, maxDel = 4, dry, gate, del, fb, cut, rq, mix, pp, fbIn, wet;
        dry  = In.ar(in, n).asArray;
        gate = Lag.ar(K2A.ar(\on.kr(1)), 0.02);
        del  = \delay.kr(0.375, 0.1).clip(0.005, maxDel);
        fb   = \feed.kr(0.5, 0.05).clip(0, 0.98);
        cut  = \cutoff.kr(84, 0.05).clip(12, 130).midicps.min(SampleRate.ir * 0.45);
        rq   = 1 - \resonance.kr(0.2, 0.05).clip(0, 0.9);
        mix  = \mix.kr(0.35, 0.05).clip(0, 1);
        pp   = \pingpong.kr(0, 0.05).clip(0, 1);
        fbIn = LocalIn.ar(n).asArray;
        if(n > 1) {
            fbIn = fbIn.collect { |x, i|
                var partner = if(i.even) { (i + 1).min(n - 1) } { i - 1 };
                (x * (1 - pp)) + (fbIn[partner] * pp)
            };
        };
        wet = LeakDC.ar((fbIn * fb) + (dry * gate), 0.9995);
        wet = Select.ar(\filtertype.kr(0).clip(0, 3), [
            RLPF.ar(wet, cut, rq), RHPF.ar(wet, cut, rq), BPF.ar(wet, cut, rq), BPeakEQ.ar(wet, cut, 1, (1 - rq) * 12)
        ]);
        wet = (wet * 0.5).tanh * 2;
        LocalOut.ar(DelayC.ar(wet, maxDel, (del - ControlDur.ir).max(0)));
        Out.ar(out, (dry * (1 - (mix * gate)).sqrt) + (fbIn * mix.sqrt));
    }).writeDefFile(dir);

    // ---- Aux sends: one per strip with sends, after its track synth ----
    // Reads the track's taps (post ++ pre, 2N) and adds them, times gainK, into
    // up to 4 return buses (retK, -1 unused); preK picks the pre-fader tap.
    SynthDef(("mixerProSend" ++ numChannels).asSymbol, {
        arg in=0;
        var n = numChannels, taps, post, pre;
        taps = In.ar(in, 2 * n).asArray.collect { |x| Select.ar(CheckBadValues.ar(x, 0, 0) > 0, [x, DC.ar(0)]) };
        post = taps.copyRange(0, n - 1);
        pre  = taps.copyRange(n, 2 * n - 1);
        4.do { |k|
            var bus = ("ret" ++ k).asSymbol.kr(-1);
            var g = ("gain" ++ k).asSymbol.kr(0, 0.05);
            var isPre = ("pre" ++ k).asSymbol.kr(0) > 0.5;
            // unused slot: silence into bus 0 (a negative bus is not ignored by Out)
            Out.ar(bus.max(0), Select.ar(isPre, [post, pre]) * (g * (bus >= 0)));
        };
    }).writeDefFile(dir);

    // ---- Master: maximizer (drive into a 5 ms lookahead limiter, as Maximizer.scd) ----
    SynthDef(("mixerProMax" ++ numChannels).asSymbol, {
        arg in=0, out=0;
        var la = 0.005, sig, x, taps, mid, det, over, xk, gr, knee, release, isp, latency;
        sig     = In.ar(in, numChannels).asArray;
        x       = sig * \drive.kr(0, 0.05).clip(0, 24).dbamp;
        release = (\character.kr(4, 0.05).clip(0, 10) * 0.15).linexp(0, 1.5, 0.05, 0.8);
        knee    = \character.kr(4, 0.05).clip(0, 10) * 0.3;
        isp     = \isp.kr(0) > 0.5;
        // detector on the undelayed signal; ISP: 4x oversampled true-peak estimate
        taps = [x];
        31.do { taps = taps.add(Delay1.ar(taps.last)) };
        mid = [0.25, 0.5, 0.75].collect { |fr|
            var h = (0..31).collect { |k| var t = (15 - k) + fr;
                if(t == 0, 1, { sin(pi * t) / (pi * t) }) * cos(pi * t / 33).squared };
            h = h / h.sum;
            (taps * h).sum.abs
        };
        det  = Select.ar(isp, [x.abs, max(taps[15].abs, mid.reduce(\max))]);
        det  = det.reduce(\max);   // linked
        over = det.max(-120.dbamp).ampdb;
        xk   = (over + (knee * 0.5)).clip(0, knee.max(0.01));
        gr   = (xk.squared / (2 * knee.max(0.01))) + (over - (knee * 0.5)).max(0);
        gr   = Lag.ar(LagUD.ar(gr, 0, release * 6.9078), la * 0.8);
        x    = Limiter.ar(DelayN.ar(x, la, la) * gr.neg.dbamp, 1, 0.001) * \ceiling.kr(-0.3, 0.05).clip(-12, 0).dbamp;
        latency = la + 0.002;
        Out.ar(out, LinXFade2.ar(DelayN.ar(sig, latency, latency), x, \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    // ---- Master: brickwall limiter (SC Limiter, 3 ms lookahead = 6 ms latency) ----
    SynthDef(("mixerProLim" ++ numChannels).asSymbol, {
        arg in=0, out=0;
        var dur = 0.003, sig = In.ar(in, numChannels);
        var lim = Limiter.ar(sig, \ceiling.kr(-0.3, 0.05).clip(-24, 0).dbamp, dur);
        Out.ar(out, LinXFade2.ar(DelayN.ar(sig, 2 * dur, 2 * dur), lim, \on.kr(1, 0.03) * 2 - 1));
    }).writeDefFile(dir);

    // ---- Master: output (after the master chain) + its VU ----
    SynthDef(("mixerProMasterOut" ++ numChannels).asSymbol, {
        arg in=0, out=0, vuBus=(-1);
        var sig = In.ar(in, numChannels);
        Out.kr(vuBus, LagUD.kr(Amplitude.kr(sig), \vuAttackTime.kr(10) * 0.001, \vuReleaseTime.kr(300) * 0.001).max(0).min(2) * (vuBus >= 0));
        Out.ar(out, sig);
    }).writeDefFile(dir);

    // ---- Channel adapter: every input count into this channel count ----
    channelCounts.do({|numInputs|
        SynthDef(("mixerProAdapt" ++ numInputs ++ "_" ++ numChannels).asSymbol, {
            arg in=0, out=0;
            var sig = In.ar(in, numInputs).asArray;
            var identity = Array.fill(numInputs * numChannels, {|k|
                if((k div: numChannels) == (k mod: numChannels), 1, 0)
            });
            var matrix = NamedControl.kr(\matrix, identity, 0.05);
            Out.ar(out, Array.fill(numChannels, {|j|
                Array.fill(numInputs, {|i| sig[i] * matrix[i * numChannels + j] }).sum
            }));
        }).writeDefFile(dir);
    });
});
("SC Mixer Pro SynthDefs written to " ++ dir).postln;
)
