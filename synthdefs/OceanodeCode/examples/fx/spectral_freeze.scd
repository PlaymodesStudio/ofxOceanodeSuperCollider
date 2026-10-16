// Spectral freeze (phase vocoder). Persistent effect: connect the In port.
// When freeze goes 0 -> 1 the spectrum of that instant is captured and held
// for as long as freeze stays on: the input no longer matters, it can change
// or stop. Each bin is judged at that moment: a steady one (a partial) keeps
// turning at the speed it had, so pitched sound is held exactly (same pitch,
// timbre and buzz); an unsteady one (noise, a transient) gets fresh random
// phases, so it becomes a smooth wash instead of the captured window looping
// like an echo. motion adds random phase to every bin: 0 = faithful,
// 1 = a soft, moving Paulstretch-like cloud.
// It swells in over attack and fades out over release; the capture is kept
// until the fade has finished.
// Mangle: freeze (gate it from a sequencer), motion, attack / release,
// blur (spectral smear), drift (slow pitch wander), soft (darkens the top), mix.
(
SynthDef(\spectralFreeze, { |in = 0, out = 0, freeze = 0, motion = 0, attack = 0.3, release = 2, blur = 0, drift = 0, soft = 0, mix = 1|
    var size = 16384;
    var dry = In.ar(in, 2);
    var on = freeze > 0.5;
    var env = EnvGen.kr(Env.asr(attack, 1, release, \sin), on);
    var hold = on + (env > 0.0005) > 0;
    // noise added to unit-length phases: its FFT bins are about 52x the
    // amplitude, so this goes from none to clearly random
    var noiseAmp = motion.squared * 2.5 / 52;
    var wet = dry.collect { |ch, i|
        var bLive = LocalBuf(size), bPrev = LocalBuf(size), bR = LocalBuf(size), bS = LocalBuf(size);
        var bHeld = LocalBuf(size), bW = LocalBuf(size);
        // a chain for `buf` that fires on the frames of `after`, and runs after it
        var on2 = { |buf, after| ((after >= 0) * (buf + 1)) - 1 };
        // two conjugations: no change, but the bins are now in complex form
        var complex = { |chain| PV_Conj(PV_Conj(chain)) };
        var live, rot, held, unsteady, unsteadyHeld, unit, state, phase, mags, sig, gain;
        live = FFT(bLive, ch, 0.25);
        // how far every bin turned since the previous frame: live * conj(prev), unit length
        rot = PV_Mul(PV_Copy(live, LocalBuf(size)), PV_Conj(PV_Copy(on2.(bPrev, live), LocalBuf(size))));
        rot = complex.(PV_MagDiv(rot, PV_Copy(rot, LocalBuf(size)), 1e-12));
        // PV_BinWipe copies bins but not the buffer's complex/polar flag, so it
        // always writes into a fresh PV_Copy (which carries the flag)
        held = PV_BinWipe(PV_Copy(rot, bHeld), on2.(bR, rot), hold);   // kept once frozen
        // How unsteady each bin is: |this rotation - the previous one| (bR still
        // holds the previous one here). ~0 for a partial, ~1.4 for noise.
        unsteady = PV_Add(PV_Copy(rot, LocalBuf(size)), PV_PhaseShift(PV_Copy(on2.(bR, held), LocalBuf(size)), pi));
        unsteadyHeld = PV_BinWipe(PV_Copy(unsteady, LocalBuf(size)), on2.(bW, unsteady), hold);
        PV_Copy(unsteadyHeld, bW);
        PV_Copy(on2.(bHeld, unsteadyHeld), bR);
        // phases: the live frame until it freezes, then the last ones turned by the held rotation
        unit = complex.(PV_MagDiv(PV_Copy(on2.(bLive, held), LocalBuf(size)), PV_Copy(on2.(bLive, held), LocalBuf(size)), 1e-12));
        state = PV_BinWipe(PV_Copy(unit, LocalBuf(size)), PV_Mul(on2.(bS, unit), held), hold);
        PV_Copy(state, bS);
        PV_Copy(on2.(bLive, state), bPrev);
        // Random phases: for unsteady bins always (noise weighted by their
        // unsteadiness, so partials are left alone), for all of them with
        // motion. Without the first, noise and transients came back every
        // window length (0.37 s), like an echo with feedback.
        phase = PV_Add(PV_Copy(state, LocalBuf(size)), FFT(LocalBuf(size), WhiteNoise.ar(noiseAmp), 0.25));
        phase = PV_Add(phase, PV_MagMul(FFT(LocalBuf(size), WhiteNoise.ar(0.06), 0.25),
            PV_Copy(unsteadyHeld, LocalBuf(size))));
        // magnitudes of the moment it froze
        mags = PV_MagFreeze(PV_Copy(on2.(bLive, phase), LocalBuf(size)), hold);
        mags = PV_MagSmear(mags, (blur * 24).round);
        sig = PV_CopyPhase(mags, phase);
        sig = PV_BinShift(sig, 1 + LFNoise2.kr(0.11 + (i * 0.04), drift * 0.006));
        sig = IFFT(sig);
        // 4x overlap doubles the level, and motion / blur change it again:
        // until it freezes, compare with the live level and keep that ratio
        gain = (Lag.ar(ch.squared, 1.5) / Lag.ar(sig.squared, 1.5).max(1e-10)).sqrt.clip(0.05, 8);
        sig * Gate.ar(gain, 1 - hold)
    };
    wet = LPF.ar(wet, soft.linexp(0, 1, 20000, 2500));
    Out.ar(out, XFade2.ar(dry, wet, (env * mix) * 2 - 1));
}, metadata: (specs: (freeze: [0, 1], motion: [0, 1], attack: [0.01, 10, \exp], release: [0.05, 20, \exp],
    blur: [0, 1], drift: [0, 1], soft: [0, 1], mix: [0, 1]))).add;
)
