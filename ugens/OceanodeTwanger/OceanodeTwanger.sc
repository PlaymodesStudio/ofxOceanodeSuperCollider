// One note of the Reaktor ensemble t'wanGer: see OceanodeTwanger.cpp.
// Values are the Reaktor panel's own; defaults: preset 'acousticGitarroid'.
TwangerVoice : UGen {
    *ar { arg gate = 0, pitch = 60, att = 11.34, rel = 40.47, cut = 103, res = 0.823, vel = 55.1, spread = 4.13,
            filter = 2, r1damp = 4.78, r1undamp = 10.77, r1cut = -13.2, r1res = 0.208, r1amt = 8600,
            r1dec = 25.1, r1rel = 45.0, r2hicut = 68.9, r2tune = -8.79, r2damp = -0.459, r2on = 0,
            timbre = 0.47, scale = 0.47, trans = 35.4, dton = 0, unison = 2;
        ^this.multiNew('audio', gate, pitch, att, rel, cut, res, vel, spread, filter,
            r1damp, r1undamp, r1cut, r1res, r1amt, r1dec, r1rel,
            r2hicut, r2tune, r2damp, r2on, timbre, scale, trans, dton, unison)
    }
}
