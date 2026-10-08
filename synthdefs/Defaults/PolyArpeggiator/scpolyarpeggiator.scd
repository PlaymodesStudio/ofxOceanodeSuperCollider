// ScPolyArpeggiator — Polyphonic arpeggiator with euclidean patterns, audio-rate trigger and gate
// Fixed at 16 output channels. Active voices controlled at runtime via \polyphony KR parameter.
// Unused voices (i >= polyphony) are multiplied by 0 — no synth recreation on polyphony change.
// All musical logic (scale, pattern, deviations) is pre-computed in C++ and uploaded as arrays.
// All timing-critical logic (step counting, gate envelopes, strum) runs in this SynthDef.
// Clock: \sync = 0 (default) counts pulses of the Trig input bus (free clock);
// \sync = 1 follows ofxOceanode's transport through anchors (scTransportSync.h):
// steps = (beat + offset) * \div, pattern indices and random decisions are a
// function of the absolute step, so seek/loop replays are identical.
// Controls added: \rid (per-synth RandID), \sync \anchorId \anchorHard \anchorPos
// \anchorFire \run \bpm \div \seqbase \hashbase. \reset is now a counter.

(
var d, defDir;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
defDir = d +/+ "/ScPolyArpeggiator";
File.mkdir(d);
File.mkdir(defDir);

SynthDef(\ScPolyArpeggiator16, {

    // ── ALL VAR DECLARATIONS FIRST (SC requirement) ──
    var maxSteps, maxVoices, trigIn, reset, reseed;
    var pitchValues, eucGate, eucAccent, eucDur;
    var seqSize, eucLen, eucAccLen, eucDurLen, polyInterval, polyphony, skipSteps;
    var velBase, velRndm, accStr;
    var durBase, durRndm, durEucStr;
    var stepChance, noteChance, strumTime, strumRndm, strumDir, seed;
    var rid, combinedReset, freeTrig, trig;
    var rawCount, freeStepIndex, syncStepIndex, stepIndex;
    var syncOn, anchorId, anchorHard, anchorPos, anchorFire, run, bpm, div;
    var seqBase, hashBase, stepsPerSec, anchorTrig, hardTrig, pos, syncIdx, top, syncTrig, elapsed;
    var hStep, hashRand, stepRand;
    var eucGateIdx, isEucActive, stepChanceOK, gateTrig;
    var accentIdx, isAccented, velRand, stepVel;
    var durAccIdx, isDurAccented, durRand, stepDur;
    var gateArray, pitchArray, velArray, durArray;
    var maxStrumSeconds;
    var randomDirFlip, isRandom, isDesc, effectiveDir;

    // ── CONSTANTS ──
    maxSteps  = 64;
    maxVoices = 16; // fixed SynthDef width

    // ── AUDIO-RATE TRIGGER INPUT ──
    trigIn = In.ar(OceanodeInput.kr(\trig), 1);

    // ── RESET / RESEED ──
    // \reset is a COUNTER: C++ increments it on every Reset press and any
    // change is one reset (never 1-then-0 pulses, they can cancel in a block).
    reset  = OceanodeParameter.kr(\reset,  0, 1, 0, 16777216, "i");
    reseed = OceanodeParameter.kr(\reseed, 0, 1, 0, 1, "vi");

    // ── SEQUENCE DATA (64-wide arrays, set by C++ on parameter change) ──
    pitchValues = OceanodeParameter.ar(\pitchvalues, 60, maxSteps, 0,   127, "vf");
    eucGate     = OceanodeParameter.ar(\eucgate,      1, maxSteps, 0,   1,   "vf");
    eucAccent   = OceanodeParameter.ar(\eucaccent,    0, maxSteps, 0,   1,   "vf");
    eucDur      = OceanodeParameter.ar(\eucdur,       0, maxSteps, 0,   1,   "vf");

    // ── SCALAR PARAMETERS ──
    seqSize      = OceanodeParameter.kr(\seqsize,      16, 1, 1, maxSteps, "i");
    eucLen       = OceanodeParameter.kr(\euclen,        8, 1, 1, maxSteps, "i");
    eucAccLen    = OceanodeParameter.kr(\eucacclen,     4, 1, 1, maxSteps, "i");
    eucDurLen    = OceanodeParameter.kr(\eucdurlen,     4, 1, 1, maxSteps, "i");
    polyInterval = OceanodeParameter.kr(\polyinterval,  2, 1, 1, 12,       "i");
    polyphony    = OceanodeParameter.kr(\polyphony,     1, 1, 1, maxVoices, "i"); // runtime, no synth recreation
    skipSteps    = OceanodeParameter.kr(\skipsteps,     0, 1, 0, 32,       "i");

    // ── VELOCITY PARAMETERS ──
    velBase = OceanodeParameter.ar(\velbase,  0.8, 1, 0, 1,    "vf");
    velRndm = OceanodeParameter.ar(\velrndm,  0.1, 1, 0, 1,    "vf");
    accStr  = OceanodeParameter.ar(\accstr,   0.2, 1, 0, 1,    "vf");

    // ── DURATION PARAMETERS ──
    durBase   = OceanodeParameter.ar(\durbase,   100, 1, 1,     5000, "vf");
    durRndm   = OceanodeParameter.ar(\durrndm,    20, 1, 0,     1000, "vf");
    durEucStr = OceanodeParameter.ar(\dureucstr,  50, 1, -5000, 5000, "vf");

    // ── PROBABILITY ──
    stepChance = OceanodeParameter.ar(\stepchance, 1.0, 1, 0, 1, "vf");
    noteChance = OceanodeParameter.ar(\notechance, 1.0, 1, 0, 1, "vf");

    // ── STRUM ──
    strumTime = OceanodeParameter.ar(\strum,     0.0, 1,   0, 500, "vf");
    strumRndm = OceanodeParameter.ar(\strumrndm, 0.0, 1,   0, 200, "vf"); // per-voice random jitter (ms)
    strumDir  = OceanodeParameter.kr(\strumdir,    0, 1,   0, 2,   "i"); // 0=asc, 1=desc, 2=random per step

    // ── SEED ──
    seed = OceanodeParameter.kr(\seed, 0, 1, 0, 65536, "i");

    // ── PER-SYNTH RANDOM GENERATOR (free clock) ──
    // RandID selects ONE of the server's random generators (0..numRGens-1, 64 by
    // default) for the whole synth, so a single RandID per synth is all that
    // matters. C++ sets \rid per node (1..63), so arpeggiators neither share nor
    // reseed each other's stream (nor generator 0, used by every other synth).
    rid = \rid.ir(0);
    RandID.ir(rid);
    // Seeded mode (seed > 0): restart the stream at start, on Seed change, on
    // Reseed and on Reset, so the random decisions reproduce.
    RandSeed.kr(
        (seed > 0) * (Changed.kr(seed) + (reseed > 0.5) + Changed.kr(reset) + Impulse.kr(0)),
        seed.max(1)
    );

    // ── COMBINED RESET (free clock) — single-sample pulse per reset ──
    combinedReset = (T2A.ar(Changed.kr(reset)) + T2A.ar(reseed > 0.5)).min(1);

    // ── FREE CLOCK: NORMALISE INCOMING TRIGGER ──
    freeTrig = Trig1.ar(trigIn > 0.1, SampleDur.ir);

    // ── FREE STEP COUNTER ──
    // PulseCount outputs 1 on the first trigger after a reset: subtract 1 so the
    // first step played (at start and after every reset) is step 0.
    rawCount      = PulseCount.ar(freeTrig, combinedReset) - 1;
    freeStepIndex = (rawCount * (skipSteps + 1)) % seqSize;

    // ── TRANSPORT CLOCK (\sync = 1; see scTransportSync.h, positions in steps) ──
    // C++ anchors the position (steps = (beat + offset) * div) and sends, per
    // anchor, the sequence index (\seqbase) and a hash key (\hashbase) of the
    // ABSOLUTE step at floor(anchorPos), computed in 64-bit/double. Here we only
    // add the (small) number of steps elapsed since the anchor, so float32 stays
    // exact and seek/loop land on the right step.
    syncOn     = \sync.kr(0);
    anchorId   = \anchorId.kr(0);
    anchorHard = \anchorHard.kr(0);
    // Values read on the anchor's first sample are AUDIO-RATE controls: a .kr
    // control used in .ar math is ramped linearly across the block, so on the
    // anchor sample it would still hold the previous value (e.g. anchorFire
    // 0 -> the step under the playhead at Play was skipped, one step late).
    // .ar controls change in a single step at the block boundary.
    anchorPos  = \anchorPos.ar(0);
    anchorFire = \anchorFire.ar(0);
    run        = \run.ar(0);
    bpm        = \bpm.kr(120);
    div        = \div.kr(4);     // steps per beat
    seqBase    = \seqbase.ar(0);
    hashBase   = \hashbase.ar(0);

    stepsPerSec = bpm / 60 * div;
    anchorTrig  = T2A.ar(Changed.kr(anchorId));
    hardTrig    = T2A.ar(Changed.kr(anchorHard));
    pos         = anchorPos + Sweep.ar(anchorTrig, stepsPerSec * run);
    syncIdx     = pos.floor;
    top         = RunningMax.ar(syncIdx, hardTrig); // reset on jumps
    syncTrig    = ((syncIdx > Delay1.ar(top)) * (1 - hardTrig)) + (hardTrig * anchorFire);
    elapsed     = syncIdx - anchorPos.floor;        // steps since the anchor (>= 0)
    syncStepIndex = (seqBase + (elapsed * (skipSteps + 1))) % seqSize;

    // ── ACTIVE CLOCK ──
    trig      = Select.ar(syncOn, [freeTrig, syncTrig]);
    stepIndex = Select.ar(syncOn, [freeStepIndex, syncStepIndex]);

    // Sync-mode randomness: a pure function of (seed, absolute step, decision d),
    // so a replay after seek/loop is identical. Values in [0, 1).
    // (hashBase < 2^22 and seed * 131 + k < 2^24: exact integers in float32)
    hStep    = Hasher.ar(hashBase + elapsed);
    hashRand = { |k| (Hasher.ar(hStep + Hasher.kr((seed * 131) + k)) * 0.5) + 0.5 };

    // ── EUCLIDEAN GATE FILTER ──
    eucGateIdx  = stepIndex % eucLen;
    isEucActive = Select.ar(eucGateIdx, eucGate) > 0.5;

    // Step chance (free: seeded TRand stream; sync: hash, decision 0)
    stepRand     = Select.ar(syncOn, [TRand.ar(0, 1, trig), Latch.ar(hashRand.(0), trig)]);
    stepChanceOK = stepRand < stepChance.asArray[0];

    // Final gate trigger for this step
    gateTrig = trig * isEucActive * stepChanceOK;

    // ── PER-STEP VELOCITY (shared across all voices) ──
    accentIdx  = stepIndex % eucAccLen;
    isAccented = Select.ar(accentIdx, eucAccent) > 0.5;
    velRand = Select.ar(syncOn, [TRand.ar(0, 1, gateTrig), Latch.ar(hashRand.(1), gateTrig)]);
    stepVel = (velBase.asArray[0]
        + (velRand * velRndm.asArray[0])
        + (isAccented * accStr.asArray[0])).clip(0, 1);

    // ── PER-STEP DURATION (shared across all voices) ──
    durAccIdx     = stepIndex % eucDurLen;
    isDurAccented = Select.ar(durAccIdx, eucDur) > 0.5;
    durRand = Select.ar(syncOn, [TRand.ar(0, 1, gateTrig), Latch.ar(hashRand.(2), gateTrig)]);
    stepDur = (durBase.asArray[0]
        + (isDurAccented * durEucStr.asArray[0])
        + (durRand * durRndm.asArray[0])).clip(1, 60000);

    // ── STRUM DIRECTION (per step) ──
    // effectiveDir: 0 = ascending (voice 0 fires first), 1 = descending (last voice first)
    // strumDir == 0: always ascending
    // strumDir == 1: always descending
    // strumDir == 2: random flip per gateTrig
    randomDirFlip = Select.ar(syncOn, [TRand.ar(0, 1, gateTrig), Latch.ar(hashRand.(3), gateTrig)]) > 0.5; // 0 or 1, re-drawn each step
    isRandom = K2A.ar(strumDir >= 2); // 1 when strumDir == 2
    isDesc   = K2A.ar(strumDir >= 1); // 1 when strumDir == 1 or 2
    // When strumDir==0: isRandom=0, isDesc=0  → 0*flip + 1*0 = 0  (ascending)
    // When strumDir==1: isRandom=0, isDesc=1  → 0*flip + 1*1 = 1  (descending)
    // When strumDir==2: isRandom=1, isDesc=1  → 1*flip + 0*1 = flip (random per step)
    effectiveDir = isRandom * randomDirFlip + (1 - isRandom) * isDesc;

    // ── PER-VOICE OUTPUTS (always 16 channels) ──
    gateArray  = Array.newClear(maxVoices);
    pitchArray = Array.newClear(maxVoices);
    velArray   = Array.newClear(maxVoices);
    durArray   = Array.newClear(maxVoices);

    maxStrumSeconds = 0.6; // 600ms maximum strum delay (covers 500ms strum + 100ms jitter)

    maxVoices.do {|i|
        var voiceStep, pitch, strumDelay, voiceTrig, polyMask;
        var noteOK, pitchLatched, velLatched, durLatched, gate;
        var ascDelay, descDelay, jitter, noteRand;

        // Voice active mask: 1.0 if i < polyphony, 0.0 otherwise
        polyMask = K2A.ar(i < polyphony);

        voiceStep = (stepIndex + (i * polyInterval)) % seqSize;
        pitch     = Select.ar(voiceStep, pitchValues);

        // Strum base delays for each direction
        ascDelay  = i * strumTime.asArray[0] * 0.001;                  // ms → s, ascending
        // Descending: the highest ACTIVE voice (polyphony - 1) fires first
        descDelay = (polyphony - 1 - i).max(0) * strumTime.asArray[0] * 0.001; // ms → s, descending

        // Per-voice random strum jitter — drawn at gateTrig, independent per voice
        // (sync: hash decision 4 + i)
        jitter = Select.ar(syncOn, [TRand.ar(0, 1, gateTrig), Latch.ar(hashRand.(4 + i), gateTrig)])
                 * strumRndm.asArray[0] * 0.001; // ms → s

        // Weighted sum replaces Select.ar — avoids rate-mismatch when i=0 or i=maxVoices-1
        // makes ascDelay or descDelay a Float 0.0. Multiplying Float by AR UGen gives AR.
        // effectiveDir is 0 or 1, so this is a hard switch, not an interpolation.
        strumDelay = (ascDelay * (1 - effectiveDir) + descDelay * effectiveDir + jitter)
                         .clip(0, maxStrumSeconds);
        voiceTrig  = DelayN.ar(gateTrig, maxStrumSeconds, strumDelay);

        // Per-voice note chance (independent random draw per voice)
        // free: drawn when the voice fires; sync: hash decision 20 + i of the step
        noteRand = Select.ar(syncOn, [TRand.ar(0, 1, voiceTrig), Latch.ar(hashRand.(20 + i), gateTrig)]);
        noteOK   = noteRand < noteChance.asArray[0];

        // Apply note chance AND polyphony mask — unused voices output silence
        voiceTrig = voiceTrig * noteOK * polyMask;

        // Latch pitch, velocity, duration at the moment this voice fires
        pitchLatched = Latch.ar(pitch,   voiceTrig);
        velLatched   = Latch.ar(stepVel, voiceTrig);
        durLatched   = Latch.ar(stepDur, voiceTrig) * 0.001; // ms → s

        // Gate held for durLatched seconds
        gate = Trig.ar(voiceTrig, durLatched);

        gateArray[i]  = gate;
        pitchArray[i] = pitchLatched;
        velArray[i]   = velLatched;
        durArray[i]   = durLatched * 1000; // back to ms for output
    };

    Out.ar(OceanodeOutput.kr(\gateout),  gateArray);
    Out.ar(OceanodeOutput.kr(\pitchout), pitchArray);
    Out.ar(OceanodeOutput.kr(\velout),   velArray);
    Out.ar(OceanodeOutput.kr(\durout),   durArray);

}).writeDefFile(defDir);

"ScPolyArpeggiator16 compiled".postln;
)
