// echoBox.scd — SynthDefs for the EchoBox C++ node (src/nodes/scEchoBox.*)
//
//   EchoBoxTap<N>   one echo tap, N channels (1..16). The node runs one per
//                   tap; they all read the input and add their echoes into
//                   one wet bus.
//   EchoBoxMix<N>   input + echoes -> out: dry / wet, ducking, output gain.
// Between the two, while Reverb is on, MixerPro's SpaceMaster insert
// (mixerProRvb<N>) puts a reverb on the echoes.
//
// A tap, per channel (based on echo.scd and EchoBlur.scd):
//
//   input * send -> rotated by \rotate channels -> delay -> echo -> out
//                                                    ^        |
//   feedback <- freeze / feed <- shimmer <- saturation <- low cut <- filter
//                                           <- diffusion (4 allpasses) <-+
//
//   • Feedback returns to the next channel(s) with \pingpong (\bounce: the
//     step, +1 or -1): echoes travel around the channels.
//   • Diffusion is in the loop: the first echo is clear, every repeat runs
//     through the allpasses again and gets more diffuse (EchoBlur).
//   • Saturation: at 0 dB a softclip (linear below half scale: only a
//     safety), with Drive tanh(x * drive) / drive: unity for small signals,
//     so the feedback stays as set, and bounded, so Feedback can go past 1
//     (self-oscillating, without running away).
//   • Shimmer pitch-shifts the loop: each repeat climbs (or falls).
//   • Freeze mutes the input and holds the loop (feedback 1).
//   • Wow / flutter: a sine plus a little noise on the delay time.
//   • The loop's LocalIn/LocalOut block is compensated: Time is exact.
//   • \vis (control, 1 channel): the tap's output level, for the window.
//   • Modulation (the shared system, synthdefs/Defaults/Modulation): one LFO
//     oscillator per CHANNEL; target t on bus \modbus + t * 16, a channel
//     each (already scaled): 0 Time (octaves) 1 Feedback 2 Cutoff (st)
//     3 Level (x) 4 Ping-pong 5 Drive (dB) 6 Shimmer 7 Diffusion 8 Send (x);
//     the mix reads 9 Mix. Every tap gets the same per-channel modulation.
(
var root, dir;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "EchoBox";
File.mkdir(root); File.mkdir(dir);

(1..16).do { |n|
    var kv = { |name, def| NamedControl.kr(name,def!n).asArray };
    // a target's n channels (one per channel), 0 without modulation
    var mod = { |t| (In.kr(\modbus.kr(-1).max(0) + (t * 16), n) * (\modbus.kr(-1) >= 0)).asArray };
    SynthDef(("EchoBoxTap" ++ n).asSymbol, {
        var maxDel = 4, maxBlur = 0.5;
        var time = \time.kr(0.375, 0.08) * 2.pow(mod.(0));            // per channel (arrays of n)
        var freeze = \freeze.kr(0, 0.03).clip(0, 1);
        var feed = (\feed.kr(0.45, 0.05) + mod.(1)).clip(0, 1.2);
        var feedGain = feed + ((1 - feed) * freeze);
        var send = \send.kr(1, 0.05) * (1 + mod.(8)).max(0) * (1 - freeze);
        var rotate = \rotate.kr(0), bounce = \bounce.kr(1), pp = (\pingpong.kr(0, 0.05) + mod.(4)).clip(0, 1);
        var ftype = \ftype.kr(1).clip(0, 4);
        var cut = (\cutoff.kr(110, 0.05) + mod.(2)).clip(12, 135).midicps.min(SampleRate.ir * 0.45);
        var blur = (\blur.kr(0, 0.05) + mod.(7)).clip(0, 1);
        var driveDb = (\drive.kr(0, 0.05) + mod.(5)).clip(0, 36), drive = driveDb.dbamp, driveAmt = (driveDb / 6).clip(0, 1);
        var shiftMix = (\shiftmix.kr(0, 0.05) + mod.(6)).clip(0, 1);
        var ppA = pp.asArray, timeA = time.asArray, blurA = blur.asArray, cutA = cut.asArray;
        var driveA = drive.asArray, driveAmtA = driveAmt.asArray, shiftMixA = shiftMix.asArray, feedGainA = feedGain.asArray;
        var input = (In.ar(\in.kr(0), n) * send).asArray;
        var fbIn = LocalIn.ar(n).asArray;
        var rotated, bounced, results, wet;

        rotated = n.collect { |i| if(n == 1) { input[0] } { Select.ar((i - rotate).wrap(0, n), input) } };
        bounced = n.collect { |i|
            if(n == 1) { fbIn[0] } { (fbIn[i] * (1 - ppA[i])) + (Select.ar((i - bounce).wrap(0, n), fbIn) * ppA[i]) }
        };
        results = n.collect { |i|
            var spread = kv.(\spread,0.5)[i].lag(0.05), wowRate = kv.(\wowrate,0.6)[i].clip(0.01,20);
            var wowDepth = kv.(\wowdepth,0)[i].lag(0.05).clip(0,50)*0.001;
            var blurTime = kv.(\blurtime,8)[i].lag(0.05).clip(0.1,100)*0.001;
            var rq = (1-kv.(\res,0.1)[i].lag(0.05).clip(0,0.95)).max(0.05);
            var lowcut = kv.(\lowcut,40)[i].lag(0.05).clip(10,5000), shift = kv.(\shift,12)[i].lag(0.05).clip(-24,24);
            // a small fixed offset per channel for the diffuser and the wow,
            // so the channels do not blur identically (EchoBlur's Spread)
            var off = if(n == 1) { 0 } { ((((i * 7) % 17) - 8) * 0.08) * spread };
            var wow = (SinOsc.kr(wowRate, i * 0.7) + (LFNoise2.kr(wowRate * 3.1) * 0.3)) * wowDepth;
            var t = (timeA[i] + wow).clip(ControlDur.ir + SampleDur.ir, maxDel);
            var echo = DelayC.ar(rotated[i], maxDel, t) +
                DelayC.ar(bounced[i], maxDel, (t - ControlDur.ir).max(SampleDur.ir));
            var bt = (blurTime * 2.pow(off / 12)).clip(SampleDur.ir, maxBlur * 0.45);
            var coef = (0.45 + (blurA[i] * 0.4)).clip(0.0001, 0.9);
            var blurred = echo, loop;
            [1.0, 1.25992105, 1.58740105, 2.0].do { |r|
                var d = (bt * r + SampleDur.ir).clip(SampleDur.ir, maxBlur);
                blurred = AllpassN.ar(blurred, maxBlur, d, d * (0.001.log / coef.log) * -1);
            };
            loop = echo + ((blurred - echo) * blurA[i]);
            // 0 off, 1 low-pass, 2 high-pass, 3 band-pass, 4 peak (echo.scd's filters)
            loop = Select.ar(ftype, [loop, RLPF.ar(loop, cutA[i], rq), RHPF.ar(loop, cutA[i], rq), BPF.ar(loop, cutA[i], rq),
                BPeakEQ.ar(loop, cutA[i], 1, (1 - rq) * 12)]);
            loop = HPF.ar(loop, lowcut);
            // 0 dB: transparent below half scale (softclip), only a safety
            // for runaway feedback; more Drive blends into tanh saturation
            loop = (loop.softclip * (1 - driveAmtA[i])) + (((loop * driveA[i]).tanh / driveA[i]) * driveAmtA[i]);
            loop = (loop * (1 - shiftMixA[i])) + (PitchShift.ar(loop, 0.2, shift.midiratio, 0, 0.004) * shiftMixA[i]);
            [echo, LeakDC.ar(loop) * feedGainA[i]]
        };
        LocalOut.ar(results.collect(_[1]));
        wet = results.collect(_[0]) * \level.kr(1, 0.05) * (1 + mod.(3)).max(0);
        Out.kr(\vis.kr(0), Amplitude.kr(wet, 0.01, 0.2).asArray.reduce(\max));
        Out.ar(\out.kr(0), wet);
    }).writeDefFile(dir);

    SynthDef(("EchoBoxMix" ++ n).asSymbol, {
        var dry = In.ar(\in.kr(0), n).asArray, wet = In.ar(\wet.kr(0), n).asArray;
        var mix = (\mix.kr(0.35, 0.05) + mod.(9)).clip(0, 1);
        var env = Amplitude.kr(dry, 0.005, 0.1).asArray.reduce(\max);
        // ducking: the echoes step back while the input plays
        var duck = 1 - (\duck.kr(0, 0.05).clip(0, 1) * LagUD.kr((env * 4).clip(0, 1), 0.005, \duckrel.kr(0.3).max(0.01)));
        var sig;
        wet = wet * duck;
        sig = (dry * (1 - mix).sqrt) + (wet * mix.sqrt);
        Out.kr(\vis.kr(0), [env, Amplitude.kr(wet, 0.01, 0.2).asArray.reduce(\max), duck]);
        Out.ar(\out.kr(0), sig * \gain.kr(1, 0.05));
    }).writeDefFile(dir);
};
"EchoBox SynthDefs written to ".post; dir.postln;
)
