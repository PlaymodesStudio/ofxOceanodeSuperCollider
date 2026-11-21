// Bake: SynthDefs of the SC Bake node (src/nodes/recording/scBake.*).
//
// BakeLoop<n>  plays an n-channel baked texture, looping it forever. The
//              crossfade that hides the loop point is already inside the file
//              (written by the node after the offline render), so this is a
//              plain looping read: the cheapest thing that can replace a chain.
//              A short fade-in on creation keeps a graph rebuild from clicking.
// BakeTap<n>   only used inside offline render scores: copies n channels of
//              the baked chain's bus to the file's outputs, raw. The normal
//              output synth would add the master volume, tanh and LeakDC,
//              which the baked file must not contain (the player's output goes
//              through them again, live). It replaces, never adds to, what is
//              on those channels.
//
// BakeSync<n>  plays a Synced Loop bake, locked to the transport: the intro
//              (the first cycle of the render, the sequence starting from
//              silence) from the transport's start, then the loop (the
//              second cycle, which carries the first one's tails) over and
//              over. C++ sends scTransportSync anchors (as SampleBox's Sync):
//                \anchorId     counter, a new anchor on every change
//                \anchorPos    the anchor's beat, modulo \loopbeats
//                \anchorIntro  1 the anchor starts the intro, -1 it ends it,
//                              0 a drift correction (no change)
//                \run, \tbpm  moving or not, tempo
//                \bakedbpm    the tempo it was rendered at
//              The clocks count frames, one per sample at the baked tempo
//              (exact reads); jumps land at once on a whole frame, drift
//              corrections only past 256 frames, glided. Play starts at full
//              level; stopped: silent within 20 ms.
//
// Defaults scripts are loaded by syncCompile.scd, whose argv[0] is the
// compilation mode, not an output path: write beside this source.
var compiledDir, destination;
compiledDir = if(thisProcess.nowExecutingPath.notNil) {
    thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(compiledDir);
destination = compiledDir +/+ "Bake";
File.mkdir(destination);

(1..64).do({ |n|
    SynthDef(("BakeLoop" ++ n).asSymbol, {
        var buf = \buf.kr(0);
        var sig = PlayBuf.ar(n, buf, BufRateScale.kr(buf), loop: 1);
        var fadeIn = EnvGen.kr(Env([0, 1], [0.02]));
        Out.ar(\out.kr(0), sig * \levels.kr(1, 0.05) * fadeIn);
    }).writeDefFile(destination);

    SynthDef(("BakeSync" ++ n).asSymbol, {
        var beats = \loopbeats.kr(16).max(0.001);
        var bpm = \tbpm.kr(120);
        var run = \run.ar(0);
        var frames = \frames.kr(1).max(1);
        var half = frames * 0.5;
        // The clocks count frames: at the baked tempo exactly one per sample,
        // so the read position is a whole number (exact in float32, however
        // long the loop) and every sample is read as it was rendered. Another
        // tempo resamples.
        var rate = bpm / \bakedbpm.kr(120).max(1) * BufRateScale.kr(\loopbuf.kr(0)) * run;
        var anchorTrig = T2A.ar(Changed.kr(\anchorId.kr(0)));
        var anchorFrames = (\anchorPos.ar(0) / beats).frac * frames;
        var target = (anchorFrames + Phasor.ar(anchorTrig, rate, 0, frames, 0)) % frames;
        var free = Phasor.ar(0, rate, 0, frames, 0);
        var diff = ((target - free + half) % frames) - half;
        // more than 5 ms: a jump, to a whole frame
        var jump = (diff - Delay1.ar(diff)).abs > (0.005 * SampleRate.ir);
        var hard = Latch.ar(diff, jump + Impulse.ar(0)).round(1);
        // A drift correction lands on its block's start, so taken as it comes
        // each one moves the read by up to a block (64 samples) -- the loop
        // would wander. Real drift (audio clock vs system clock) is a few ppm:
        // corrections are rounded to 256 frames and glided, so the read stays
        // on the sample until the clocks are really that far apart.
        var soft = ((diff - hard + half) % frames) - half;
        var pos = (free + hard + Lag.ar((soft / 256).round * 256, 2)) % frames;
        // the loop's own wrap ends the intro; a jump is not a wrap
        var wrap = ((Delay1.ar(pos) - pos) > half) * (1 - jump);
        var intro = \anchorIntro.kr(0);
        // combined at control rate, then converted: an audio-rate trigger
        // times a control-rate value reads the previous block's value at the
        // block's first sample. Impulse: an anchor sent with the synth itself.
        var anchorK = Changed.kr(\anchorId.kr(0)) + Impulse.kr(0);
        var introOn = SetResetFF.ar(T2A.ar(anchorK * (intro > 0.5)), wrap + T2A.ar(anchorK * (intro < -0.5)));
        var loopSig = BufRd.ar(n, \loopbuf.kr(0), pos, 1, 4).asArray;
        var introSig = BufRd.ar(n, \introbuf.kr(0), pos, 1, 4).asArray;
        // instant on (a downbeat keeps its attack), 20 ms off; the synth's
        // own creation fades in (a graph rebuild mid-loop), Play does not
        var gain = LagUD.ar(run, 0, 0.02) * \levels.kr(1, 0.05) * EnvGen.kr(Env([0, 1], [0.02]));
        Out.ar(\out.kr(0), n.collect { |c| Select.ar(introOn, [loopSig[c], introSig[c]]) } * gain);
    }).writeDefFile(destination);

    SynthDef(("BakeTap" ++ n).asSymbol, {
        // ReplaceOut: from the tail of the tree it overwrites whatever else
        // reached the file's channels (a second Output, a synth's default out=0)
        ReplaceOut.ar(\out.kr(0), In.ar(\in.kr(0), n));
    }).writeDefFile(destination);
});
