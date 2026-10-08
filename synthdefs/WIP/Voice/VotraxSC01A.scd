/*
Votrax SC-01A inspired speech synthesizer for SuperCollider.

This is a behavioural / musical model, not a transistor-level emulation.
It uses a mixed pulse-and-noise excitation, four parallel formant filters,
frication, aspiration and plosive bursts.  A persistent Synth is updated for
each phoneme so the formants glide as they did in the original analogue chip.

In SC IDE, evaluate only the first parenthesized block to boot and load the
model. Then evaluate one example line or block at a time below it.
*/

(
s.waitForBoot {

    SynthDef(\votraxSC01A, {
        |out = 0, gate = 1, amp = 0, pan = 0, pitch = 105,
        transition = 0.035, voiced = 1, noise = 0.03,
        aspiration = 0, fricative = 0, fricFreq = 5000,
        fricBw = 1800, burst = 0, stop = 0, stopDelay = 0.025,
        diphthong = 0, diphTime = 0.08, t_trig = 0|

        var formants, formantsEnd, bandwidths, formantAmps;
        var glide, fundamental, jitter, glottal, breath, excitation;
        var phoneTime, closure, releaseTrig, diphPhase;
        var ff, bb, aa, body, fric, burstEnv, burstSound;
        var envelope, signal;

        formants = NamedControl.kr(\formants, #[500, 1500, 2500, 3500]);
        formantsEnd = NamedControl.kr(\formantsEnd, #[500, 1500, 2500, 3500]);
        bandwidths = NamedControl.kr(\bandwidths, #[80, 110, 160, 240]);
        formantAmps = NamedControl.kr(\formantAmps, #[1, 0.55, 0.28, 0.12]);

        glide = transition.clip(0.002, 2.0);
        fundamental = Lag.kr(pitch.clip(35, 500), glide);
        jitter = (LFNoise2.kr(7) * 0.004) + 1;

        // Slightly asymmetric pulse plus a weak subharmonic makes the
        // excitation less clean and more like early speech hardware.
        glottal = Pulse.ar(fundamental * jitter, 0.38, 0.72);
        glottal = glottal + (Pulse.ar(fundamental * 0.5, 0.46, 0.12));
        glottal = LPF.ar(glottal, 6500);

        breath = WhiteNoise.ar;
        phoneTime = Sweep.kr(t_trig);
        // Voiced stops retain a little low buzz while the mouth is closed.
        closure = stop * (phoneTime < stopDelay);
        excitation = (glottal * Lag.kr(voiced, glide) * (1 - (closure * 0.8)))
            + (breath * Lag.kr(noise, glide) * (1 - closure));
        excitation = excitation
            + (HPF.ar(breath, 900) * Lag.kr(aspiration, glide));

        // Diphthongs move between two formant targets within one event.
        // Sweep restarts at each phoneme trigger; ordinary phonemes have
        // diphthong = 0 and stay at their single formant target.
        diphPhase = ((phoneTime - 0.012)
            / diphTime.clip(0.015, 5.0)).clip(0, 1);
        ff = Lag.kr(
            formants + ((formantsEnd - formants) * diphPhase * diphthong),
            glide
        ).clip(90, 9000);
        bb = Lag.kr(bandwidths, glide).clip(20, 5000);
        aa = Lag.kr(formantAmps, glide);

        body = Mix(BPF.ar(excitation, ff, (bb / ff).clip(0.005, 1), aa));

        fric = BPF.ar(
            WhiteNoise.ar,
            Lag.kr(fricFreq, glide).clip(300, 10000),
            (Lag.kr(fricBw, glide) / Lag.kr(fricFreq, glide)).clip(0.02, 1)
        ) * Lag.kr(fricative, glide);

        // Delay a stop's burst until its closure releases. Other consonants
        // still burst immediately when triggered.
        releaseTrig = (t_trig * (1 - stop))
            + (HPZ1.kr(phoneTime >= stopDelay).max(0) * stop);
        burstEnv = Decay2.kr(releaseTrig, 0.001, 0.016);
        burstSound = BPF.ar(WhiteNoise.ar, 3200, 0.85)
            * burstEnv * burst * 3;

        envelope = EnvGen.kr(Env.asr(0.01, 1, 0.06), gate, doneAction: 2);
        signal = body + fric + burstSound;
        signal = HPF.ar(LPF.ar(LeakDC.ar(signal), 9000), 55);
        signal = (signal * 2.2).tanh * 0.48;

        Out.ar(out, Pan2.ar(signal * Lag.kr(amp, 0.008) * envelope, pan));
    }).add;

    // Preset constructors. Values are deliberately exposed below so they are
    // easy to retune by ear against recordings of a real SC-01A.
    ~votraxVowel = {
        |f = #[500, 1500, 2500, 3500],
        bw = #[80, 110, 160, 240],
        fa = #[1, 0.55, 0.28, 0.12], dur = 0.14, gain = 1|
        (
            f: f, fEnd: f, diphthong: 0, diphTime: 0.08,
            bw: bw, fa: fa, dur: dur, gain: gain,
            voiced: 1, noise: 0.018, aspiration: 0,
            stop: 0, stopDelay: 0.025,
            fricative: 0, fricFreq: 5000, fricBw: 1800, burst: 0
        )
    };

    ~votraxConsonant = {
        |f = #[450, 1450, 2500, 3500],
        bw = #[120, 180, 260, 380],
        fa = #[0.8, 0.4, 0.2, 0.08], dur = 0.085, gain = 1,
        voiced = 0, noise = 0.08, aspiration = 0,
        fricative = 0, fricFreq = 5000, fricBw = 1800, burst = 0,
        stop = 0, stopDelay = 0.025|
        (
            f: f, fEnd: f, diphthong: 0, diphTime: 0.08,
            bw: bw, fa: fa, dur: dur, gain: gain,
            voiced: voiced, noise: noise, aspiration: aspiration,
            fricative: fricative, fricFreq: fricFreq,
            fricBw: fricBw, burst: burst,
            stop: stop, stopDelay: stopDelay
        )
    };

    ~votraxPause = { |dur = 0.08|
        (
            f: #[500, 1500, 2500, 3500],
            fEnd: #[500, 1500, 2500, 3500],
            diphthong: 0, diphTime: 0.08,
            bw: #[100, 150, 220, 320],
            fa: #[0, 0, 0, 0], dur: dur, gain: 0,
            voiced: 0, noise: 0, aspiration: 0,
            fricative: 0, fricFreq: 5000, fricBw: 1800, burst: 0,
            stop: 0, stopDelay: 0.025
        )
    };

    ~votraxPhonemes = IdentityDictionary.new;

    // Vowels and vowel variants. Names follow the SC-01A's 6-bit code table.
    ~votraxPhonemes[\EH3] = ~votraxVowel.([460, 1900, 2550, 3500], dur: 0.11);
    ~votraxPhonemes[\EH2] = ~votraxVowel.([500, 1860, 2510, 3460], dur: 0.12);
    ~votraxPhonemes[\EH1] = ~votraxVowel.([540, 1820, 2480, 3420], dur: 0.13);
    ~votraxPhonemes[\EH]  = ~votraxVowel.([530, 1840, 2480, 3420], dur: 0.14);
    ~votraxPhonemes[\E1]  = ~votraxVowel.([300, 2240, 3000, 3650], dur: 0.13);
    ~votraxPhonemes[\E]   = ~votraxVowel.([270, 2290, 3010, 3700], dur: 0.15);

    ~votraxPhonemes[\I3]  = ~votraxVowel.([350, 2100, 2760, 3560], dur: 0.10);
    ~votraxPhonemes[\I2]  = ~votraxVowel.([380, 2040, 2660, 3510], dur: 0.11);
    ~votraxPhonemes[\I1]  = ~votraxVowel.([410, 1990, 2580, 3470], dur: 0.12);
    ~votraxPhonemes[\I]   = ~votraxVowel.([390, 1990, 2550, 3450], dur: 0.13);

    ~votraxPhonemes[\A2]  = ~votraxVowel.([680, 1230, 2470, 3350], dur: 0.12);
    ~votraxPhonemes[\A1]  = ~votraxVowel.([720, 1150, 2450, 3320], dur: 0.13);
    ~votraxPhonemes[\A]   = ~votraxVowel.([730, 1090, 2440, 3300], dur: 0.15);
    ~votraxPhonemes[\AE1] = ~votraxVowel.([620, 1780, 2450, 3370], dur: 0.13);
    ~votraxPhonemes[\AE]  = ~votraxVowel.([660, 1720, 2410, 3340], dur: 0.15);

    ~votraxPhonemes[\AH2] = ~votraxVowel.([560, 1370, 2460, 3380], dur: 0.11);
    ~votraxPhonemes[\AH1] = ~votraxVowel.([610, 1270, 2420, 3340], dur: 0.12);
    ~votraxPhonemes[\AH]  = ~votraxVowel.([640, 1190, 2390, 3310], dur: 0.14);
    ~votraxPhonemes[\UH3] = ~votraxVowel.([390, 1120, 2300, 3300], dur: 0.10);
    ~votraxPhonemes[\UH2] = ~votraxVowel.([420, 1060, 2270, 3270], dur: 0.11);
    ~votraxPhonemes[\UH1] = ~votraxVowel.([450, 1020, 2240, 3240], dur: 0.12);
    ~votraxPhonemes[\UH]  = ~votraxVowel.([440, 1020, 2240, 3240], dur: 0.13);

    ~votraxPhonemes[\OO1] = ~votraxVowel.([330, 920, 2290, 3230], dur: 0.13);
    ~votraxPhonemes[\OO]  = ~votraxVowel.([300, 870, 2240, 3200], dur: 0.15);
    ~votraxPhonemes[\O2]  = ~votraxVowel.([540, 900, 2410, 3300], dur: 0.12);
    ~votraxPhonemes[\O1]  = ~votraxVowel.([570, 850, 2410, 3300], dur: 0.13);
    // O is /oʊ/ as in "cold" or the end of "hello": close toward OO.
    ~votraxPhonemes[\O]   = ~votraxVowel.([530, 1040, 2390, 3310], dur: 0.185);
    ~votraxPhonemes[\O][\fEnd] = [310, 840, 2230, 3210];
    ~votraxPhonemes[\O][\diphthong] = 1;
    ~votraxPhonemes[\O][\diphTime] = 0.12;
    ~votraxPhonemes[\U1]  = ~votraxVowel.([310, 940, 2250, 3220], dur: 0.12);
    ~votraxPhonemes[\U]   = ~votraxVowel.([290, 900, 2200, 3180], dur: 0.14);
    ~votraxPhonemes[\IU]  = ~votraxVowel.([300, 1700, 2550, 3450], dur: 0.14);
    ~votraxPhonemes[\ER]  = ~votraxVowel.([490, 1350, 1690, 3300], dur: 0.16);

    // SC-01A AY is the vowel in "day" (/eɪ/). It moves internally from
    // an EH-like onset toward an E-like ending; it is not "eye" (/aɪ/).
    ~votraxPhonemes[\AY] = ~votraxVowel.([520, 1840, 2510, 3420], dur: 0.075);
    ~votraxPhonemes[\AY][\fEnd] = [300, 2240, 2900, 3580];
    ~votraxPhonemes[\AY][\diphthong] = 1;
    ~votraxPhonemes[\AY][\diphTime] = 0.045;

    // Convenience sound for the "eye" in "collider" (/aɪ/). AI is not
    // one of the SC-01A's 64 hardware phoneme codes.
    ~votraxPhonemes[\AI] = ~votraxVowel.([740, 1260, 2460, 3350], dur: 0.17);
    ~votraxPhonemes[\AI][\fEnd] = [320, 2220, 2890, 3550];
    ~votraxPhonemes[\AI][\diphthong] = 1;
    ~votraxPhonemes[\AI][\diphTime] = 0.11;

    ~votraxPhonemes[\AW1] = ~votraxVowel.([650, 1120, 2410, 3300], dur: 0.13);
    ~votraxPhonemes[\AW2] = ~votraxVowel.([520, 920, 2350, 3260], dur: 0.13);
    ~votraxPhonemes[\AW]  = ~votraxVowel.([610, 1020, 2390, 3280], dur: 0.15);

    // Nasals, liquids and approximants.
    ~votraxPhonemes[\M] = ~votraxConsonant.([250, 1050, 2150, 3150], [90, 170, 230, 320], [1, 0.22, 0.10, 0.04], 0.10, voiced: 0.85, noise: 0.015);
    ~votraxPhonemes[\N] = ~votraxConsonant.([300, 1450, 2350, 3350], [100, 180, 260, 350], [0.9, 0.28, 0.12, 0.05], 0.09, voiced: 0.82, noise: 0.025);
    ~votraxPhonemes[\NG] = ~votraxConsonant.([300, 1950, 2750, 3500], [110, 230, 300, 420], [0.8, 0.24, 0.12, 0.04], 0.11, voiced: 0.80, noise: 0.025);
    ~votraxPhonemes[\L] = ~votraxConsonant.([360, 1320, 2700, 3500], dur: 0.09, voiced: 0.90, noise: 0.012);
    ~votraxPhonemes[\R] = ~votraxConsonant.([420, 1280, 1660, 3300], dur: 0.10, voiced: 0.92, noise: 0.01);
    ~votraxPhonemes[\W] = ~votraxConsonant.([310, 900, 2200, 3200], dur: 0.09, voiced: 0.90, noise: 0.01);
    ~votraxPhonemes[\Y] = ~votraxConsonant.([280, 2180, 2950, 3600], dur: 0.09, voiced: 0.88, noise: 0.01);
    ~votraxPhonemes[\Y1] = ~votraxConsonant.([300, 2050, 2820, 3540], dur: 0.085, voiced: 0.86, noise: 0.012);

    // Fricatives: unvoiced and voiced pairs.
    ~votraxPhonemes[\F] = ~votraxConsonant.(dur: 0.095, noise: 0.05, fricative: 0.36, fricFreq: 4200, fricBw: 3200);
    ~votraxPhonemes[\V] = ~votraxConsonant.(dur: 0.09, voiced: 0.45, noise: 0.035, fricative: 0.24, fricFreq: 4000, fricBw: 3000);
    ~votraxPhonemes[\S] = ~votraxConsonant.(dur: 0.11, noise: 0.025, fricative: 0.72, fricFreq: 6900, fricBw: 2300);
    ~votraxPhonemes[\Z] = ~votraxConsonant.(dur: 0.10, voiced: 0.42, noise: 0.02, fricative: 0.48, fricFreq: 6500, fricBw: 2400);
    ~votraxPhonemes[\SH] = ~votraxConsonant.(dur: 0.12, noise: 0.04, fricative: 0.68, fricFreq: 3600, fricBw: 1900);
    ~votraxPhonemes[\ZH] = ~votraxConsonant.(dur: 0.105, voiced: 0.42, noise: 0.03, fricative: 0.46, fricFreq: 3400, fricBw: 1900);
    ~votraxPhonemes[\TH] = ~votraxConsonant.(dur: 0.09, noise: 0.055, fricative: 0.30, fricFreq: 5200, fricBw: 3800);
    ~votraxPhonemes[\THV] = ~votraxConsonant.(dur: 0.09, voiced: 0.46, noise: 0.04, fricative: 0.20, fricFreq: 4700, fricBw: 3600);
    ~votraxPhonemes[\H] = ~votraxConsonant.([510, 1820, 2500, 3420],
        dur: 0.07, gain: 0.6, noise: 0.008, aspiration: 0.055);

    // Stops and affricates.
    ~votraxPhonemes[\P] = ~votraxConsonant.(dur: 0.075, noise: 0.035, aspiration: 0.10, burst: 0.65);
    ~votraxPhonemes[\B] = ~votraxConsonant.(dur: 0.070, voiced: 0.48, noise: 0.015, burst: 0.42);
    ~votraxPhonemes[\T] = ~votraxConsonant.([420, 1800, 2900, 3900], dur: 0.065, noise: 0.035, aspiration: 0.08, burst: 0.75);
    ~votraxPhonemes[\D] = ~votraxConsonant.([350, 1650, 2750, 3750],
        dur: 0.065, voiced: 0.48, noise: 0.012, burst: 0.20,
        stop: 1, stopDelay: 0.024);
    ~votraxPhonemes[\DT] = ~votraxConsonant.([430, 1650, 2700, 3700], dur: 0.050, voiced: 0.38, noise: 0.02, burst: 0.30);
    ~votraxPhonemes[\K] = ~votraxConsonant.([350, 2050, 3000, 3900], dur: 0.080, noise: 0.04, aspiration: 0.10, burst: 0.82);
    ~votraxPhonemes[\G] = ~votraxConsonant.([350, 1950, 2900, 3800], dur: 0.075, voiced: 0.50, noise: 0.018, burst: 0.50);
    ~votraxPhonemes[\CH] = ~votraxConsonant.(dur: 0.115, noise: 0.04, fricative: 0.52, fricFreq: 3900, fricBw: 2100, burst: 0.72);
    ~votraxPhonemes[\J] = ~votraxConsonant.(dur: 0.105, voiced: 0.38, noise: 0.025, fricative: 0.36, fricFreq: 3600, fricBw: 2100, burst: 0.50);

    ~votraxPhonemes[\PA0] = ~votraxPause.(0.055);
    ~votraxPhonemes[\PA1] = ~votraxPause.(0.12);
    ~votraxPhonemes[\STOP] = ~votraxPause.(0.18);

    s.sync;

    ~votraxVoice = Synth(\votraxSC01A, [\amp, 0]);
    ~votraxSpeed = 0.65; // 1 = original timing; 0.5 = twice as slow

    // Send one preset to the persistent Synth. Inflection is 0..3.
    ~votraxSet = { |name, fundamental = 105, inflection = 1,
        level = 0.18, transition = 0.035, speed = 1|

        var p, pitchRatio;
        p = ~votraxPhonemes[name.asSymbol];

        if(p.isNil) {
            ("Unknown Votrax phoneme: " ++ name).warn;
        } {
            pitchRatio = [-2, 0, 2, 4].at(inflection.clip(0, 3).asInteger).midiratio;

            if(~votraxVoice.isNil) {
                ~votraxVoice = Synth(\votraxSC01A, [\amp, 0]);
            };

            ~votraxVoice.set(
                \pitch, fundamental * pitchRatio,
                \transition, transition,
                \amp, level * p[\gain],
                \voiced, p[\voiced],
                \noise, p[\noise],
                \aspiration, p[\aspiration],
                \fricative, p[\fricative],
                \fricFreq, p[\fricFreq],
                \fricBw, p[\fricBw],
                \stop, p[\stop],
                \stopDelay, p[\stopDelay] / speed,
                \diphthong, p[\diphthong],
                \diphTime, p[\diphTime] / speed,
                \burst, p[\burst],
                \t_trig, 1
            );
            ~votraxVoice.setn(\formants, p[\f]);
            ~votraxVoice.setn(\formantsEnd, p[\fEnd]);
            ~votraxVoice.setn(\bandwidths, p[\bw]);
            ~votraxVoice.setn(\formantAmps, p[\fa]);
        };
    };

    /*
    Sequence item formats:
        \EH1                 use preset duration and inflection 1
        [\EH1, 0.20]         explicit duration
        [\EH1, 0.20, 3]      explicit duration and inflection (0..3)

    By default, playback uses ~votraxSpeed. An explicit speed: value overrides
    it for one phrase. 1 = original timing, 0.5 = twice as slow, 2 = twice as
    fast. The returned Routine can be stopped with .stop.
    */
    ~votraxSay = { |sequence, fundamental = 105, speed = nil,
        level = 0.18, transition = 0.035|

        Routine({
            var name, p, duration, inflection, rate;

            sequence.do { |item|
                if(item.isArray) {
                    name = item[0].asSymbol;
                    p = ~votraxPhonemes[name];
                    duration = item[1] ? if(p.notNil) { p[\dur] } { 0.1 };
                    inflection = item[2] ? 1;
                } {
                    name = item.asSymbol;
                    p = ~votraxPhonemes[name];
                    duration = if(p.notNil) { p[\dur] } { 0.1 };
                    inflection = 1;
                };

                rate = ((speed ? ~votraxSpeed) ? 1).max(0.05);
                ~votraxSet.(name, fundamental, inflection, level,
                    transition / rate, rate);
                (duration / rate).wait;
            };

            ~votraxVoice.set(\amp, 0);
        }).play(SystemClock);
    };

    "Votrax SC-01A model loaded. Evaluate an example at the bottom of the file.".postln;
};
)


// ---------------------------------------------------------------------------
// EXAMPLES — evaluate one line/block at a time after the message above appears
// ---------------------------------------------------------------------------

// Playback speed for all phrases: 1 = normal, 0.5 = half speed.
// Change this number and evaluate the line again whenever you like.
~votraxSpeed = 0.15;

// Hold one phoneme, then try other symbols or inflection values 0..3:
~votraxSet.(\AE, 105, 1, 0.18, 0.04);
~votraxSet.(\ER, 90, 0, 0.18, 0.08);
~votraxVoice.set(\amp, 0);

// "HELLO" — H EH L O, with a rising last phoneme:
~votraxSay.([\H, [\EH, 0.16], \L, [\O, 0.24, 3]], level: 0.20);

~votraxSay.([\O, \N, \E], level: 0.20);

// Hear the O-to-OO movement by itself:
~votraxSay.([[\O, 0.24]], fundamental: 105, level: 0.18);

// Listen to AY alone, then in "DAY". AY should move toward an "ee" sound:
~votraxSay.([[\AY, 0.22]], fundamental: 105, level: 0.18);
~votraxSay.([\D, [\AY, 0.22]], fundamental: 105, level: 0.18);

// The "eye" vowel in "collider" uses AI, not the SC-01A's AY:
~votraxSay.([[\AI, 0.22]], fundamental: 105, level: 0.18);

// "SUPER COLLIDER" (deliberately robotic SC-01A-style spelling):
(
~votraxSay.([
    \S, \U, \P, \ER, \PA0,
    \K, \UH, \L, [\AI, 0.19, 2], \D, [\ER, 0.20, 2]
], fundamental: 102, level: 0.19, transition: 0.028);
)

// More musical: phonemes with explicit pitch changes.
(
~votraxMelody = Routine({
    [[\V, 48], [\O, 52], [\T, 55], [\R, 60], [\AE, 64], [\K, 67], [\S, 72]].do {
        |pair, i|
        ~votraxSet.(pair[0], pair[1].midicps, i % 4, 0.18, 0.055);
        0.18.wait;
    };
    ~votraxVoice.set(\amp, 0);
}).play;
)

// Free the persistent synth when finished:
~votraxVoice.free; ~votraxVoice = nil;
