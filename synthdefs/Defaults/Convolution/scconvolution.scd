// scconvolution.scd
//
// Two SynthDef families:
//
//   scconvIRGen  — writes a synthetic impulse response into a buffer.
//                  Uses gaussian-like noise with a time-varying one-pole
//                  LPF (high-frequency absorption increases over time) and
//                  an exponential decay envelope, matching the physics of a
//                  real reverberant space.  No audio output — RecordBuf only.
//
//   scconvolution1..16 — partitioned convolution (PartConv) with dry/wet
//                  blend and a 25 ms wet ramp (Lag.kr) that hides PartConv's
//                  startup transient when the synth is hot-swapped.
//
// fftsize = 2048  →  partition = 1024 samples ≈ 23 ms latency at 44.1 kHz.

var d, dir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

dir = d +/+ "/scconvolution";
File.mkdir(d);
File.mkdir(dir);

// ── IR generator ──────────────────────────────────────────────────────────────
//
//  irBuf       — target buffer (already allocated, numFrames = rt60 * sr)
//  rt60        — reverb time in seconds (duration of the IR)
//  brightness  — initial one-pole coefficient (0 = very bright, 1 = very dark)
//  absorpCurve — power applied to the growing coefficient (shape of darkening)
//
//  Algorithm matches the SC language example using:
//    • gaussian-like noise  (sum of 3 WhiteNoise UGens ≈ CLT)
//    • time-varying one-pole: coef = (brightness + t*(1-brightness))^absorpCurve
//      which starts bright and becomes fully absorbed by the end
//    • exponential decay envelope (-6 dB/oct)
//
SynthDef(\scconvIRGen, {
    |irBuf = 0, rt60 = 1.5, brightness = 0.15, absorpCurve = 0.7|

    var dur, t, coef, sig, env;

    dur = rt60.max(0.05);

    // Linear phase: 0 → 1 over the IR duration
    t = Phasor.ar(0, 1.0 / (dur * SampleRate.ir), 0, 1.0);

    // One-pole coefficient increases from brightness to 1.0 (full absorption).
    // Applying absorpCurve shapes the darkening trajectory.
    coef = (brightness + (t * (1.0 - brightness))).pow(absorpCurve);

    // Gaussian-like noise (Central Limit Theorem: sum of 3 uniforms)
    sig = (WhiteNoise.ar + WhiteNoise.ar + WhiteNoise.ar) * (1.0/3.0);

    // Time-varying one-pole LPF — darkens the reverb tail naturally
    // SC OnePole: out = in*(1-coef) + prev*coef  (LPF when coef > 0)
    sig = OnePole.ar(sig, coef);

    // Exponential decay: models energy decay in a room
    env = EnvGen.ar(Env.perc(0.001, dur, 1.0, -6), doneAction: Done.freeSelf);

    RecordBuf.ar(sig * env * 0.5, irBuf, loop: 0);

}).writeDefFile(dir);

"Created: scconvIRGen".postln;

// ── Convolution synths ────────────────────────────────────────────────────────
//
//  in      — input audio bus
//  out     — output audio bus
//  specbuf — PreparePartConv spectrum buffer index
//  wet     — 0 = dry, 1 = fully wet (smoothed over 25 ms to hide startup)
//  level   — output gain
//
(1..16).do({ |n|
    var name = ("scconvolution" ++ n).asSymbol;

    SynthDef(name, {
        |in = 0, out = 0, specbuf = 0, wet = 0.0, level = 1.0|

        var dry, conv, smoothWet, mix;

        dry = In.ar(in, n);

        // Each channel convolved with the same (mono) IR spectrum.
        // fftsize 2048 must match the value used in /b_gen PreparePartConv.
        conv = dry.asArray.collect { |ch|
            PartConv.ar(ch, 2048, specbuf)
        };

        // 25 ms wet ramp — when the synth is recreated via kAddAction_replace
        // the Lag's internal state starts at 0 and rises to the target wet value,
        // masking PartConv's ~23 ms startup transient.
        smoothWet = Lag.kr(wet, 0.025);

        mix = XFade2.ar(dry.asArray, conv, smoothWet * 2.0 - 1.0);

        Out.ar(out, mix * level);

    }).writeDefFile(dir);

    ("Created: " ++ name).postln;
});

"SC Convolution SynthDefs written.".postln;
