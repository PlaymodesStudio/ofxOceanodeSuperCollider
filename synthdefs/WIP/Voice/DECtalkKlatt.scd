/*
DECtalk-inspired text-to-speech instrument for SuperCollider 3.14.

This is an original speech synthesizer, not DECtalk firmware or an exact
recreation of any DECtalk voice. It uses a five-formant voiced oscillator and
filter bank, parallel consonant noise, nasal resonance, stop closures,
coarticulation, pitch contours, a dictionary and spelling rules.

The bundled cmudict.dict supplies English word pronunciations in ARPABET.
Other languages need a dictionary parser/phoneme mapping and any missing
vowel or consonant targets; the synthesis engine is not English-specific.
For clarity, written words ending in JH use a switchable T+S approximation.

In SC IDE, evaluate this first parenthesized block. Wait for "DECtalk model
loaded", then evaluate one example at a time at the bottom of the file.
*/

(
~decSourcePath = thisProcess.nowExecutingPath;
if(~decRoutine.notNil) { ~decRoutine.stop; ~decRoutine = nil };
if(~decVoice.notNil) { ~decVoice.free; ~decVoice = nil };
s.waitForBoot {
    SynthDef(\decKlattVoice, {
        |out = 0, amp = 0, pan = 0, gate = 1, tract = 1,
        f0Start = 115, f0End = 110, phoneDur = 0.15,
        transition = 0.025, voice = 1, aspiration = 0,
        frication = 0, fricFreq = 5000, fricBW = 2500,
        nasal = 0, stop = 0, stopDelay = 0.025,
        burst = 0, burstFreq = 3500,
        morph = 0, morphTime = 0.10, morphStart = 0.15,
        color = 0.55, t_trig = 0|

        var formants, formantsEnd, bandwidths, phoneTime, phase;
        var f, bw, pitch, glottal, breath, voiced, resonant, pulses;
        var air;
        var fric, nasalSig, weight, pulseWeight;
        var closure, release, releaseTrig, stopAir, impact;
        var burstSig, env, signal;

        formants = NamedControl.kr(\formants, #[500, 1500, 2500, 3500, 4500]);
        formantsEnd = NamedControl.kr(\formantsEnd, #[500, 1500, 2500, 3500, 4500]);
        bandwidths = NamedControl.kr(\bandwidths, #[80, 110, 160, 230, 320]);

        phoneTime = Sweep.kr(t_trig);
        phase = (phoneTime / phoneDur.clip(0.02, 5)).clip(0, 1);
        pitch = Lag.kr(f0Start + ((f0End - f0Start) * phase), 0.012)
            .clip(50, 500);

        // A tilted, harmonically rich glottal source. The pulse-resonator
        // layer fills gaps between harmonics when F0 misses a formant peak.
        glottal = (LFSaw.ar(pitch) * 0.68)
            + (Pulse.ar(pitch, 0.27) * 0.32);
        glottal = LeakDC.ar(OnePole.ar(LPF.ar(glottal, 6500), 0.52));
        breath = WhiteNoise.ar;

        closure = stop * (phoneTime < stopDelay);
        release = LagUD.kr(phoneTime >= stopDelay, 0.003, 0.003);
        phase = ((phoneTime - (phoneDur * morphStart.clip(0, 0.8)))
            / morphTime.clip(0.015, 5)).clip(0, 1);
        f = Lag.kr(formants + ((formantsEnd - formants) * phase * morph),
            transition).clip(100, 8000) * tract.clip(0.7, 1.5);
        bw = Lag.kr(bandwidths, transition).clip(30, 1200);

        // Parallel voiced resonators provide a much stronger vowel envelope
        // than a chain of broad peaking EQs. The second, formant-oscillator
        // layer adds density and a deliberately synthetic character.
        resonant = 0;
        pulses = 0;
        5.do { |i|
            weight = [1.0, 1.1, 0.85, 0.42, 0.24][i];
            pulseWeight = [0.60, 0.85, 0.55, 0.20, 0.10][i];
            resonant = resonant + (BPF.ar(glottal, f[i],
                (bw[i] / f[i]).clip(0.035, 0.8)) * weight);
            pulses = pulses + (Formant.ar(pitch, f[i],
                bw[i].max(pitch * 1.35)) * pulseWeight);
        };
        voiced = (resonant * 1.8)
            + (pulses * (0.18 + (color.clip(0, 1) * 0.25)))
            + (LPF.ar(glottal, 650) * 0.10);
        voiced = voiced * Lag.kr(voice, transition)
            * (1 - (closure * 0.97));
        // H uses vocal-tract-shaped breath; released stops need a broader,
        // higher-frequency puff that lasts beyond the initial burst.
        air = (BPF.ar(breath, f[0], 0.65) * (1 - stop))
            + ((
                (BPF.ar(breath, (burstFreq * 0.75).clip(1800, 5000), 0.8) * 0.85)
                + (HPF.ar(breath, 2600) * 0.15)
            ) * stop);
        releaseTrig = (t_trig * (1 - stop))
            + (HPZ1.kr(phoneTime >= stopDelay).max(0) * stop * 2);
        stopAir = Decay.kr(releaseTrig, 0.070);
        voiced = voiced + (air * Lag.kr(aspiration, transition)
            * ((1 - stop) * 0.32 + (stop * 1.25))
            * ((1 - stop) + (stop * release * stopAir)));

        fric = BPF.ar(breath,
            Lag.kr(fricFreq, transition).clip(500, 10000),
            (Lag.kr(fricBW, transition) / fricFreq).clip(0.03, 1)
        ) * Lag.kr(frication, transition) * 0.30;
        fric = fric + (HPF.ar(breath, 2600)
            * Lag.kr(frication, transition) * 0.025);
        // An affricate must close before its hiss begins: without this,
        // JH sounds like a soft ZH and the "dge" in "edge" disappears.
        fric = fric * ((1 - stop) + (stop * release));

        nasalSig = BPF.ar(glottal, 280, 0.5)
            * Lag.kr(nasal, transition) * 0.65;

        // The release is a short crack plus a louder, audio-rate noise burst.
        // The longer shaped aspiration above then connects it to the vowel.
        impact = T2A.ar(releaseTrig);
        burstSig = (
            (Ringz.ar(impact, burstFreq * 0.60, 0.008) * 0.8)
            + (Ringz.ar(impact, burstFreq * 1.10, 0.004) * 0.45)
        ) * burst * 0.55;
        burstSig = burstSig + (BPF.ar(WhiteNoise.ar, burstFreq, 0.9)
            * Decay2.ar(impact, 0.0015, 0.034) * burst * 0.58);

        env = EnvGen.kr(Env.asr(0.01, 1, 0.08), gate, doneAction: 2);
        signal = HPF.ar(LPF.ar(voiced + fric + nasalSig + burstSig, 8500), 65);
        signal = (signal * 1.6).tanh * Lag.kr(amp, 0.006) * env * 1.5;
        signal = Limiter.ar(signal, 0.8, 0.005);
        Out.ar(out, Pan2.ar(signal, pan));
    }).add;

    ~decVowel = { |f, dur = 0.15, fEnd = nil, bw = #[80, 110, 160, 230, 320]|
        (
            f: f, fEnd: fEnd ? f, bw: bw, dur: dur, vowel: true,
            morph: if(fEnd.isNil) { 0 } { 1 }, morphTime: dur * 0.7,
            morphStart: 0.08,
            voice: 1, aspiration: 0.012, frication: 0, nasal: 0,
            stop: 0, stopDelay: 0.025, burst: 0,
            fricFreq: 5000, fricBW: 2500, burstFreq: 3500
        )
    };

    ~decConsonant = {
        |f = #[400, 1500, 2500, 3500, 4500], dur = 0.08,
        voice = 0, aspiration = 0, frication = 0, nasal = 0,
        stop = 0, stopDelay = 0.025, burst = 0,
        fricFreq = 5000, fricBW = 2500, burstFreq = 3500|
        (
            f: f, fEnd: f, bw: #[120, 160, 220, 300, 380],
            dur: dur, vowel: false, morph: 0, morphTime: 0.1,
            morphStart: 0.08,
            voice: voice, aspiration: aspiration,
            frication: frication, nasal: nasal,
            stop: stop, stopDelay: stopDelay, burst: burst,
            fricFreq: fricFreq, fricBW: fricBW,
            burstFreq: burstFreq
        )
    };

    ~decPhones = IdentityDictionary.new;

    // Vowel targets are loosely based on the values in Klatt's 1980 paper.
    // The symbols use ARPABET spelling, which differs from the SC-01A codes.
    ~decPhones[\IY] = ~decVowel.([310, 2020, 2960, 3700, 4500], 0.16,
        [290, 2070, 2960, 3700, 4500]);
    ~decPhones[\IH] = ~decVowel.([400, 1800, 2570, 3500, 4500], 0.12);
    ~decPhones[\EY] = ~decVowel.([480, 1720, 2520, 3500, 4500], 0.17,
        [330, 2020, 2600, 3600, 4500]);
    ~decPhones[\EH] = ~decVowel.([530, 1680, 2500, 3500, 4500], 0.14);
    ~decPhones[\AE] = ~decVowel.([650, 1660, 2430, 3400, 4500], 0.16);
    ~decPhones[\AA] = ~decVowel.([700, 1220, 2600, 3500, 4500], 0.16);
    ~decPhones[\AO] = ~decVowel.([620, 990, 2570, 3400, 4500], 0.17);
    ~decPhones[\OW] = ~decVowel.([540, 1100, 2300, 3400, 4500], 0.18,
        [350, 900, 2200, 3300, 4500]);
    ~decPhones[\UH] = ~decVowel.([450, 1100, 2350, 3400, 4500], 0.13);
    ~decPhones[\UW] = ~decVowel.([350, 1250, 2200, 3300, 4500], 0.17,
        [320, 900, 2200, 3300, 4500]);
    ~decPhones[\AH] = ~decVowel.([620, 1300, 2500, 3500, 4500], 0.14);
    ~decPhones[\AX] = ~decVowel.([500, 1500, 2500, 3500, 4500], 0.09);
    ~decPhones[\ER] = ~decVowel.([470, 1270, 1540, 3400, 4500], 0.15);
    ~decPhones[\AY] = ~decVowel.([660, 1200, 2550, 3500, 4500], 0.19,
        [400, 1880, 2500, 3600, 4500]);
    ~decPhones[\AW] = ~decVowel.([640, 1230, 2550, 3500, 4500], 0.19,
        [420, 940, 2350, 3400, 4500]);
    ~decPhones[\OY] = ~decVowel.([550, 960, 2400, 3500, 4500], 0.19,
        [360, 1820, 2450, 3600, 4500]);

    // Sonorants, nasals, fricatives, stops and affricates.
    ~decPhones[\W]  = ~decConsonant.([290, 610, 2150, 3300, 4500], 0.08, voice: 0.75);
    ~decPhones[\Y]  = ~decConsonant.([260, 2070, 3020, 3800, 4500], 0.08, voice: 0.75);
    ~decPhones[\R]  = ~decConsonant.([310, 1060, 1380, 3300, 4500], 0.09, voice: 0.8);
    ~decPhones[\L]  = ~decConsonant.([310, 1050, 2880, 3500, 4500], 0.09, voice: 0.8);
    ~decPhones[\M]  = ~decConsonant.([270, 1100, 2200, 3400, 4500], 0.09, voice: 0.45, nasal: 0.65);
    ~decPhones[\N]  = ~decConsonant.([270, 1450, 2470, 3500, 4500], 0.09, voice: 0.45, nasal: 0.65);
    ~decPhones[\NG] = ~decConsonant.([270, 1900, 2800, 3600, 4500], 0.10, voice: 0.45, nasal: 0.65);
    ~decPhones[\HH] = ~decConsonant.(dur: 0.075, aspiration: 0.26);
    ~decPhones[\F]  = ~decConsonant.(dur: 0.10, frication: 0.42, fricFreq: 4200, fricBW: 3400);
    ~decPhones[\V]  = ~decConsonant.(dur: 0.09, voice: 0.4, frication: 0.30, fricFreq: 4100, fricBW: 3300);
    ~decPhones[\TH] = ~decConsonant.(dur: 0.10, frication: 0.34, fricFreq: 5200, fricBW: 3500);
    ~decPhones[\DH] = ~decConsonant.(dur: 0.09, voice: 0.4, frication: 0.22, fricFreq: 5000, fricBW: 3500);
    ~decPhones[\S]  = ~decConsonant.(dur: 0.11, frication: 0.85, fricFreq: 7000, fricBW: 2400);
    ~decPhones[\Z]  = ~decConsonant.(dur: 0.10, voice: 0.4, frication: 0.62, fricFreq: 6800, fricBW: 2500);
    ~decPhones[\SH] = ~decConsonant.(dur: 0.12, frication: 0.75, fricFreq: 3700, fricBW: 2200);
    ~decPhones[\ZH] = ~decConsonant.(dur: 0.10, voice: 0.4, frication: 0.52, fricFreq: 3600, fricBW: 2200);
    ~decPhones[\P]  = ~decConsonant.([400, 1100, 2150, 3500, 4500], 0.10,
        stop: 1, stopDelay: 0.032, burst: 0.62, burstFreq: 2500, aspiration: 0.30);
    ~decPhones[\B]  = ~decConsonant.([200, 1100, 2150, 3500, 4500], 0.075,
        voice: 0.35, stop: 1, stopDelay: 0.026, burst: 0.25, burstFreq: 2500);
    ~decPhones[\T]  = ~decConsonant.([400, 1600, 2600, 3700, 4500], 0.10,
        stop: 1, stopDelay: 0.029, burst: 0.90, burstFreq: 5200, aspiration: 0.75);
    ~decPhones[\D]  = ~decConsonant.([200, 1600, 2600, 3700, 4500], 0.065,
        voice: 0.35, stop: 1, stopDelay: 0.023, burst: 0.30, burstFreq: 4700);
    ~decPhones[\K]  = ~decConsonant.([300, 1990, 2850, 3800, 4500], 0.10,
        stop: 1, stopDelay: 0.030, burst: 0.72, burstFreq: 3500, aspiration: 0.34);
    ~decPhones[\G]  = ~decConsonant.([200, 1990, 2850, 3800, 4500], 0.075,
        voice: 0.35, stop: 1, stopDelay: 0.024, burst: 0.32, burstFreq: 3500);
    ~decPhones[\CH] = ~decConsonant.([350, 1800, 2820, 3800, 4500], 0.16,
        stop: 1, stopDelay: 0.044, frication: 0.74,
        fricFreq: 3900, fricBW: 2200, burst: 0.58, burstFreq: 3500);
    // A compact D-plus-S-like release avoids the low, diffuse SH noise.
    ~decPhones[\JH] = ~decConsonant.([260, 1800, 2820, 3800, 4500], 0.16,
        voice: 0.28, stop: 1, stopDelay: 0.040, frication: 0.60,
        fricFreq: 7000, fricBW: 1800, burst: 0.70, burstFreq: 5100);
    ~decPhones[\PAUSE] = ~decConsonant.(dur: 0.18);

    // Common exceptions and function words. Pronunciations use ARPABET with
    // stress digits on vowels: 0 unstressed, 1 primary, 2 secondary.
    ~decWords = Dictionary.new;
    [
        ["one", "W AH1 N"], ["two", "T UW1"], ["three", "TH R IY1"],
        ["four", "F AO1 R"], ["five", "F AY1 V"], ["six", "S IH1 K S"],
        ["seven", "S EH1 V AH0 N"], ["eight", "EY1 T"],
        ["nine", "N AY1 N"], ["ten", "T EH1 N"],
        ["zero", "Z IH1 R OW0"], ["hundred", "HH AH1 N D R AH0 D"],
        ["thousand", "TH AW1 Z AH0 N D"],
        ["0", "Z IH1 R OW0"], ["1", "W AH1 N"], ["2", "T UW1"],
        ["3", "TH R IY1"], ["4", "F AO1 R"], ["5", "F AY1 V"],
        ["6", "S IH1 K S"], ["7", "S EH1 V AH0 N"],
        ["8", "EY1 T"], ["9", "N AY1 N"],
        ["hello", "HH AH0 L OW1"], ["hi", "HH AY1"],
        ["world", "W ER1 L D"], ["dectalk", "D EH1 K T AO2 K"],
        ["supercollider", "S UW2 P ER0 K AH0 L AY1 D ER0"],
        ["super", "S UW1 P ER0"], ["collider", "K AH0 L AY1 D ER0"],
        ["computer", "K AH0 M P Y UW1 T ER0"],
        ["voice", "V OY1 S"], ["voices", "V OY1 S AH0 Z"],
        ["text", "T EH1 K S T"], ["sound", "S AW1 N D"],
        ["speech", "S P IY1 CH"], ["synth", "S IH1 N TH"],
        ["synthesizer", "S IH1 N TH AH0 S AY2 Z ER0"],
        ["robot", "R OW1 B AA2 T"], ["robots", "R OW1 B AA2 T S"],
        ["say", "S EY1"], ["says", "S EH1 Z"], ["said", "S EH1 D"],
        ["speak", "S P IY1 K"], ["speaks", "S P IY1 K S"],
        ["talk", "T AO1 K"], ["talking", "T AO1 K IH0 NG"],
        ["test", "T EH1 S T"], ["testing", "T EH1 S T IH0 NG"],
        ["this", "DH IH1 S"], ["that", "DH AE1 T"],
        ["these", "DH IY1 Z"], ["those", "DH OW1 Z"],
        ["is", "IH1 Z"], ["are", "AA1 R"], ["was", "W AH1 Z"],
        ["were", "W ER1"], ["a", "AH0"], ["an", "AE1 N"],
        ["the", "DH AH0"], ["and", "AE1 N D"],
        ["to", "T UW1"], ["too", "T UW1"], ["of", "AH1 V"],
        ["for", "F AO1 R"], ["from", "F R AH1 M"],
        ["with", "W IH1 DH"], ["in", "IH1 N"],
        ["on", "AA1 N"], ["off", "AO1 F"],
        ["out", "AW1 T"], ["all", "AO1 L"],
        ["about", "AH0 B AW1 T"], ["there", "DH EH1 R"],
        ["here", "HH IY1 R"], ["now", "N AW1"],
        ["then", "DH EH1 N"], ["you", "Y UW1"],
        ["your", "Y AO1 R"], ["me", "M IY1"],
        ["my", "M AY1"], ["we", "W IY1"],
        ["our", "AW1 ER0"], ["they", "DH EY1"],
        ["their", "DH EH1 R"], ["i", "AY1"],
        ["it", "IH1 T"], ["its", "IH1 T S"],
        ["yes", "Y EH1 S"], ["no", "N OW1"],
        ["good", "G UH1 D"], ["bad", "B AE1 D"],
        ["day", "D EY1"], ["night", "N AY1 T"],
        ["time", "T AY1 M"], ["music", "M Y UW1 Z IH0 K"],
        ["sing", "S IH1 NG"], ["play", "P L EY1"],
        ["stop", "S T AA1 P"], ["go", "G OW1"],
        ["run", "R AH1 N"], ["love", "L AH1 V"],
        ["open", "OW1 P AH0 N"], ["close", "K L OW1 Z"],
        ["slow", "S L OW1"], ["fast", "F AE1 S T"],
        ["new", "N UW1"], ["old", "OW1 L D"],
        ["can", "K AE1 N"], ["make", "M EY1 K"],
        ["making", "M EY1 K IH0 NG"], ["hear", "HH IY1 R"],
        ["heard", "HH ER1 D"], ["why", "W AY1"],
        ["what", "W AH1 T"], ["where", "W EH1 R"],
        ["when", "W EH1 N"], ["how", "HH AW1"],
        ["who", "HH UW1"], ["do", "D UW1"],
        ["does", "D AH1 Z"], ["did", "D IH1 D"],
        ["have", "HH AE1 V"], ["has", "HH AE1 Z"],
        ["had", "HH AE1 D"], ["be", "B IY1"],
        ["been", "B IH1 N"], ["word", "W ER1 D"],
        ["words", "W ER1 D Z"], ["name", "N EY1 M"],
        ["number", "N AH1 M B ER0"],
        ["please", "P L IY1 Z"], ["thanks", "TH AE1 NG K S"],
        ["thank", "TH AE1 NG K"], ["very", "V EH1 R IY0"],
        ["more", "M AO1 R"], ["less", "L EH1 S"],
        ["again", "AH0 G EH1 N"], ["right", "R AY1 T"],
        ["wrong", "R AO1 NG"], ["maybe", "M EY1 B IY0"],
        ["something", "S AH1 M TH IH0 NG"],
        ["can't", "K AE1 N T"], ["don't", "D OW1 N T"],
        ["it's", "IH1 T S"], ["i'm", "AY1 M"],
        ["let's", "L EH1 T S"]
    ].do { |entry| ~decWords[entry[0]] = entry[1] };

    // Load the bundled CMU pronunciation dictionary. The hand-written
    // entries above take precedence where phrasing needs a specific reading.
    ~decLoadCMU = { |path|
        var file, line, fields, spelling, count;
        count = 0;
        if(File.exists(path)) {
            file = File(path, "r");
            line = file.getLine;
            while { line.notNil } {
                if(line.size > 0 and: { line[0] != $# }) {
                    fields = line.split($ );
                    spelling = fields[0];
                    if((fields.size > 1) and: { spelling.find("(").isNil }
                        and: { ~decWords[spelling].isNil }) {
                        ~decWords[spelling] = fields.copyRange(1,
                            fields.size - 1).join(" ");
                        count = count + 1;
                    };
                };
                line = file.getLine;
            };
            file.close;
            ("Loaded " ++ count ++ " CMUdict pronunciations.").postln;
        } {
            ("CMUdict not found: " ++ path).warn;
        };
        count;
    };

    // Parse an ARPABET string such as "W AH1 N" into timed phone events.
    ~decParsePhones = { |arpa|
        var result;
        result = List.new;
        arpa.toUpper.split($ ).do { |raw|
            var last, name, stress, symbol;
            if(raw.size > 0) {
                last = raw.last;
                stress = if(last == $1) { 1 } { if(last == $2) { 2 } { 0 } };
                name = if((last == $0) or: { (last == $1) or: { last == $2 } }) {
                    raw.copyRange(0, raw.size - 2)
                } { raw };
                symbol = name.asSymbol;
                if(symbol == \AH and: { last == $0 }) { symbol = \AX };
                if(~decPhones[symbol].notNil) {
                    result.add((phone: symbol, stress: stress));
                } {
                    ("Unknown ARPABET phoneme: " ++ name).warn;
                };
            };
        };
        result.asArray;
    };

    // Longest grapheme patterns first. The small dictionary above handles
    // common irregular spellings; these rules cover unfamiliar simple words.
    ~decRules = [
        ["tion", "SH AH0 N"], ["sion", "ZH AH0 N"],
        ["tch", "CH"], ["dge", "JH"], ["igh", "AY"],
        ["ing", "IH NG"], ["qu", "K W"],
        ["sh", "SH"], ["ch", "CH"], ["th", "TH"],
        ["ph", "F"], ["wh", "W"], ["ng", "NG"],
        ["ck", "K"], ["ee", "IY"], ["oo", "UW"],
        ["ea", "IY"], ["ai", "EY"], ["ay", "EY"],
        ["oa", "OW"], ["oi", "OY"], ["oy", "OY"],
        ["ou", "AW"], ["ow", "AW"],
        ["au", "AO"], ["aw", "AO"],
        ["er", "ER"], ["ir", "ER"], ["ur", "ER"],
        ["ar", "AA R"], ["or", "AO R"]
    ];

    ~decShortVowels = Dictionary.new;
    ~decLongVowels = Dictionary.new;
    [[ $a, "AE", "EY" ], [ $e, "EH", "IY" ],
     [ $i, "IH", "AY" ], [ $o, "AA", "OW" ],
     [ $u, "AH", "UW" ]].do { |row|
        ~decShortVowels[row[0]] = row[1];
        ~decLongVowels[row[0]] = row[2];
    };
    ~decLetters = Dictionary.new;
    [[ $b, "B" ], [ $d, "D" ], [ $f, "F" ], [ $h, "HH" ],
     [ $j, "JH" ], [ $k, "K" ], [ $l, "L" ], [ $m, "M" ],
     [ $n, "N" ], [ $p, "P" ], [ $r, "R" ], [ $s, "S" ],
     [ $t, "T" ], [ $v, "V" ], [ $w, "W" ], [ $z, "Z" ]
    ].do { |row| ~decLetters[row[0]] = row[1] };

    ~decGuessWord = { |word|
        var w, result, i, limit, silentE, matched, fragment, c, next;
        var sound, accented;
        w = word.toLower;
        result = List.new;
        silentE = (w.size > 3) and: { w.last == $e };
        limit = if(silentE) { w.size - 1 } { w.size };
        i = 0;

        while { i < limit } {
            matched = false;
            ~decRules.do { |rule|
                if(matched.not and: { i + rule[0].size <= limit }) {
                    fragment = w.copyRange(i, i + rule[0].size - 1);
                    if(fragment == rule[0]) {
                        ~decParsePhones.(rule[1]).do { |event| result.add(event) };
                        i = i + rule[0].size;
                        matched = true;
                    };
                };
            };

            if(matched.not) {
                c = w[i];
                next = if(i + 1 < limit) { w[i + 1] } { $  };
                sound = if(~decShortVowels[c].notNil) {
                    if(silentE and: { i == (limit - 2) }) {
                        ~decLongVowels[c]
                    } {
                        ~decShortVowels[c]
                    }
                } {
                    if(c == $c) {
                        if(($e == next) or: { ($i == next) or: { $y == next } }) {
                            "S"
                        } { "K" }
                    } {
                        if(c == $g) {
                            if(($e == next) or: { ($i == next) or: { $y == next } }) {
                                "JH"
                            } { "G" }
                        } {
                            if(c == $x) { "K S" } {
                                if(c == $q) { "K" } {
                                    if(c == $y) {
                                        if(i == 0) { "Y" } {
                                            if(i == (limit - 1)) { "IY" } { "IH" }
                                        }
                                    } { ~decLetters[c] ? "" }
                                }
                            }
                        }
                    }
                };
                if((i == 0) or: { c != w[i - 1] } or: {
                    ~decShortVowels[c].notNil
                }) {
                    if(sound.size > 0) {
                        ~decParsePhones.(sound).do { |event| result.add(event) };
                    };
                };
                i = i + 1;
            };
        };

        accented = false;
        result.do { |event|
            if(accented.not and: { ~decPhones[event[\phone]][\vowel] }) {
                event[\stress] = 1;
                accented = true;
            };
        };
        result.asArray;
    };

    ~decAddWord = { |spelling, arpa|
        ~decWords[spelling.toLower] = arpa;
    };

    // The raw JH model is available in ~decSayPhones. For written words,
    // use the crisper T+S gesture at word ends (edge, badge, judge). This
    // deliberately favors intelligibility over a strict phonetic mapping.
    ~decCrispFinalJH = true;

    ~decWordPhones = { |word|
        var spelling, entry, result;
        spelling = word.toLower;
        entry = ~decWords[spelling];
        result = if(entry.notNil) { ~decParsePhones.(entry) } {
            ~decGuessWord.(spelling)
        };
        if(~decCrispFinalJH and: { result.size > 0 and: {
            result.last[\phone] == \JH
        } }) {
            result = if(result.size > 1) {
                result.copyRange(0, result.size - 2)
            } { [] };
            result = result ++ [
                (phone: \T, stress: 0, dur: 0.08),
                (phone: \S, stress: 0, dur: 0.11)
            ];
        };
        result;
    };

    ~decTokenize = { |text|
        var tokens, current;
        tokens = List.new;
        current = "";
        text.toLower.do { |char|
            var ascii, letterOrNumber;
            ascii = char.ascii;
            letterOrNumber = ((ascii >= 97) and: { ascii <= 122 })
                or: { (ascii >= 48) and: { ascii <= 57 } };
            if(letterOrNumber or: { char == $' }) {
                current = current ++ char.asString;
            } {
                if(current.size > 0) { tokens.add(current); current = "" };
                if(".,!?;:".includes(char)) { tokens.add(char.asString) };
            };
        };
        if(current.size > 0) { tokens.add(current) };
        tokens.asArray;
    };

    ~decTextPhones = { |text|
        var result;
        result = List.new;
        ~decTokenize.(text).do { |token|
            if(".,!?;:".includes(token[0])) {
                result.add((phone: \PAUSE,
                    dur: if(token == ",") { 0.12 } {
                        if(token == ".") { 0.24 } { 0.18 }
                    }));
            } {
                ~decWordPhones.(token).do { |event| result.add(event) };
            };
        };
        result.asArray;
    };

    ~decLexiconPath = if(~decSourcePath.notNil) {
        PathName(~decSourcePath).pathOnly +/+ "cmudict.dict"
    } {
        // Selection evaluation in SC IDE has no executing path. Keep this
        // editable fallback next to the distributed patch and dictionary.
        "/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/synthdefs/WIP/Voice/cmudict.dict"
    };
    ~decLoadCMU.(~decLexiconPath);

    s.sync;
    ~decVoice = Synth(\decKlattVoice, [\amp, 0]);
    ~decRate = 0.85;  // 1 = standard timing, 0.5 = twice as slow
    ~decPitch = 115;  // Hz
    ~decTract = 1.0;  // < 1 sounds larger; > 1 sounds smaller
    ~decColor = 0.55; // 0 = warmer/cleaner, 1 = brighter/more synthetic

    ~decStop = {
        if(~decRoutine.notNil) { ~decRoutine.stop; ~decRoutine = nil };
        if(~decVoice.notNil) { ~decVoice.set(\amp, 0) };
    };

    // Feed ARPABET text ("W AH1 N") or an array returned by ~decTextPhones.
    ~decSayPhones = { |phones, pitch = nil, speed = nil, tract = nil,
        level = 0.35, question = false, color = nil|
        var sequence;
        sequence = if(phones.isString) { ~decParsePhones.(phones) } { phones };
        ~decStop.();

        ~decRoutine = Routine({
            var basePitch, rate, tractScale, count;
            basePitch = pitch ? ~decPitch;
            rate = (speed ? ~decRate).max(0.1);
            tractScale = tract ? ~decTract;
            count = sequence.size.max(1);

            sequence.do { |event, index|
                var p, stress, duration, progress, startPitch, endPitch;
                var next, target, morph, morphStart, morphTime, blend, gain;
                p = ~decPhones[event[\phone]];
                if(p.notNil) {
                    stress = event[\stress] ? 0;
                    duration = event[\dur] ? p[\dur];
                    if(p[\vowel]) {
                        duration = duration * if(stress == 1) { 1.18 } {
                            if(stress == 2) { 1.08 } { 0.86 }
                        };
                    };

                    progress = index / count;
                    startPitch = basePitch * (1 - (progress * 0.13));
                    if(stress == 1) { startPitch = startPitch * 1.08 };
                    if(stress == 2) { startPitch = startPitch * 1.04 };
                    if(question and: { progress > 0.65 }) {
                        startPitch = startPitch * (1 + ((progress - 0.65) * 0.65));
                    };
                    endPitch = startPitch * 0.98;

                    // Predict part of the following mouth shape. Keep the
                    // internal glide of diphthongs, but smooth other phones
                    // toward their neighbor rather than jumping at a boundary.
                    next = if((index + 1) < sequence.size) {
                        ~decPhones[sequence[index + 1][\phone]]
                    } { nil };
                    target = p[\fEnd];
                    morph = p[\morph];
                    morphStart = p[\morphStart];
                    morphTime = p[\morphTime] / rate;
                    if((morph == 0) and: { next.notNil }
                        and: { event[\phone] != \PAUSE }
                        and: { sequence[index + 1][\phone] != \PAUSE }) {
                        blend = if(p[\vowel]) { 0.22 } { 0.55 };
                        target = p[\f].collect { |freq, j|
                            (freq * (1 - blend)) + (next[\f][j] * blend)
                        };
                        morph = 1;
                        morphStart = 0.52;
                        morphTime = (duration / rate) * 0.48;
                    };
                    gain = if(p[\vowel]) {
                        if(stress == 1) { 1.10 } {
                            if(stress == 2) { 1.0 } { 0.86 }
                        }
                    } { 0.92 };

                    if(~decVoice.isNil) {
                        ~decVoice = Synth(\decKlattVoice, [\amp, 0]);
                    };

                    s.makeBundle(nil, {
                        ~decVoice.set(
                            \amp, if(event[\phone] == \PAUSE) { 0 } {
                                level * gain
                            },
                            \tract, tractScale,
                            \color, color ? ~decColor,
                            \f0Start, startPitch, \f0End, endPitch,
                            \phoneDur, duration / rate,
                            \transition, (0.025 / rate).clip(0.004, 0.5),
                            \voice, p[\voice],
                            \aspiration, p[\aspiration],
                            \frication, p[\frication],
                            \fricFreq, p[\fricFreq],
                            \fricBW, p[\fricBW],
                            \nasal, p[\nasal],
                            \stop, p[\stop],
                            \stopDelay, p[\stopDelay] / rate,
                            \burst, p[\burst],
                            \burstFreq, p[\burstFreq],
                            \morph, morph,
                            \morphStart, morphStart,
                            \morphTime, morphTime,
                            \t_trig, 1
                        );
                        ~decVoice.setn(\formants, p[\f]);
                        ~decVoice.setn(\formantsEnd, target);
                        ~decVoice.setn(\bandwidths, p[\bw]);
                    });
                    (duration / rate).wait;
                };
            };

            if(~decVoice.notNil) { ~decVoice.set(\amp, 0) };
        }).play(SystemClock);
        ~decRoutine;
    };

    // Everyday API: written English enters the dictionary/rule front end.
    ~decSpeak = { |text, pitch = nil, speed = nil, tract = nil,
        level = 0.35, color = nil|
        ~decSayPhones.(~decTextPhones.(text), pitch, speed, tract, level,
            text.find("?").notNil, color);
    };

    ~decFree = {
        ~decStop.();
        if(~decVoice.notNil) { ~decVoice.free; ~decVoice = nil };
    };

    "DECtalk model loaded. Evaluate an example below.".postln;
};
)


// ---------------------------------------------------------------------------
// EXAMPLES — evaluate one line or parenthesized block at a time in SC IDE
// ---------------------------------------------------------------------------

~decSpeak.("one");
~decSpeak.("Hello, Gerard.");
~decSpeak.("SuperCollider can speak words.");
~decSpeak.("On the confines of the universe, a black hole stands as a giant of the cosmos");
~decSpeak.("This voice has a brighter edge.", color: 0.9);
~decSpeak.("Edge. Badge. Judge."); // crisper final-JH default
~decSpeak.("Edge. Ets. Badge. Bats. Judge. Juts."); // compare endings
~decCrispFinalJH = false; // hear the original word-final JH
~decSpeak.("Edge. Badge. Judge.");
~decCrispFinalJH = true;  // restore the crisper ending
~decSpeak.("Two. Tea. Take."); // test the T closure and release

// Speed per phrase: 1.0 = normal, 0.5 = twice as slow, 1.5 = faster.
~decSpeak.("Judge the speed.", speed: 0.5);
~decSpeak.("Judge the speed.", speed: 3.5);

// A slower, lower voice for one phrase:
~decSpeak.("One, two, three, four, five, six.", pitch: 95, speed: 0.55);

// Change defaults for all subsequent phrases:
~decRate = 0.7;
~decPitch = 115;
~decTract = 1.0;
~decColor = 0.55;

// Bypass spelling and control phonemes directly (ARPABET, not Votrax codes):
~decSayPhones.("W AH1 N");
~decSayPhones.("HH AH0 L OW1");

// Override an irregular or invented word's pronunciation:
~decAddWord.("oceanode", "OW1 SH AH0 N OW0 D");
~decSpeak.("Oceanode speaks.");

// Inspect what the text front end will send to the synth:
~decTextPhones.("one").postln;

// Stop immediately or free the persistent voice:
~decStop.();
~decFree.();
