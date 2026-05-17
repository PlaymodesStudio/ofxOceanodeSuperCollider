# PolyphonicArpeggiator — Audio-Rate SuperCollider Version
## Concept, Architecture & Development Plan

---

## 1. Context & Goal

The existing `polyphonicArpeggiator` C++ node (ofxOceanodeNodeModel) works well for MIDI-rate use, but its timing is ultimately tied to the CPU update loop. Gate-on and gate-off events are resolved at frame rate (~60fps, ≈16ms granularity), and the trigger input is a C++ bang — not an audio-rate signal. This is fine for MIDI but inadequate for tight rhythmic use inside a SuperCollider audio graph.

The goal is a new node — **`scPolyphonicArpeggiator`** — that:

- Accepts an **audio-rate trigger input** (impulse bus from any SC metro/clock node)
- Outputs **audio-rate gate signals** per polyphonic voice (N-channel audio bus)
- Outputs **pitch, velocity, and duration** as latched audio-rate values per voice (held between triggers, compatible with downstream SC synths)
- Keeps all musical logic (scale, patterns, deviations, euclidean rhythms, snapshots) in C++
- Moves all timing-critical logic (step counting, gate envelopes, strum) into a SuperCollider SynthDef

The existing C++ node remains unchanged and continues to work for MIDI use. This is a new, parallel SC node.

---

## 2. Output Vector Model: Two Separate Outputs for Pitch

A key design decision is how polyphony maps to output vectors. The node has two conceptually different pitch outputs:

### `pitchOut` — full sequence, size `seqSize` (static)
The complete pre-computed pitch sequence, size `seqSize` (up to 64). This is rebuilt only when scale/pattern/deviation parameters change — **never per trigger**. Useful for visualisation, full-sequence consumers, and the existing MIDI use case where downstream nodes need to see the whole sequence.

### `activePitchOut` — active voices, size `polyphony` (dynamic)
Updated on every trigger. Contains only the pitches currently being played:
- `activePitchOut[0]` = pitch at `currentPitches[baseIndex]` (voice 0)
- `activePitchOut[1]` = pitch at `currentPitches[(baseIndex + polyInterval) % seqSize]` (voice 1)
- etc.

This is the clean voice-mapped interface. The key insight: **`polyInterval` is a step interval in the pitch sequence index space**, not semitones. With `polyInterval=2` on a diatonic scale you get thirds; with `polyInterval=4` you get a seventh chord. Voice i always plays the pitch at `baseIndex + i * polyInterval` positions ahead in the sequence.

### `gateOut`, `velocityOut`, `durOut` — size `polyphony` (dynamic)
Fixed, dedicated slots per voice: voice 0 always on index 0, voice 1 on index 1, etc. No scattering across `seqSize`. This eliminates the slot-collision problem in the current implementation (where `outputIndex = (baseIndex + voice * polyInt) % sz` conflated pitch lookup with output routing).

### Summary of output sizes

| Output | Size | Updated |
|---|---|---|
| `pitchOut` | `seqSize` | On param change only |
| `activePitchOut` | `polyphony` | Every trigger |
| `gateOut` | `polyphony` | Every trigger + gate-off |
| `velocityOut` | `polyphony` | Every trigger |
| `durOut` | `polyphony` | Every trigger |
| `gateVelOut` | `polyphony` | Every trigger + gate-off |

---

## 3. What Stays in C++ vs. What Moves to SC

### Stays in C++ (identical to current node)

| Responsibility | How |
|---|---|
| Scale expansion (`rebuildExpandedScale`) | `expandedScale` vector |
| Pitch sequence computation (`rebuildPitchSequence`) | `currentPitches[]` size `seqSize`, including deviations |
| Euclidean gate pattern (`euclideanPattern`) | `vector<bool>`, sent as float array to SC |
| Euclidean accent pattern (`euclideanAccents`) | `vector<bool>`, sent as float array to SC |
| Euclidean duration pattern (`euclideanDurations`) | `vector<bool>`, sent as float array to SC |
| Pattern mode (ascending/descending/random/user) | Pre-baked into pitch array, invisible to SC |
| Pitch deviations (octave/index/chromatic) | Pre-baked into pitch array |
| Snapshot/morph system | Pure C++ UI |
| All parameter UI | `ofParameter` / ImGui |

### Moves to SC (new SynthDef)

| Responsibility | SC mechanism |
|---|---|
| Step counter advancement | `PulseCount.ar` on incoming trigger |
| Euclidean gate filtering | `Select.ar` into C++-supplied euclidean array |
| Pitch lookup per voice | `Select.ar((stepIndex + i*polyInterval) % seqSize, pitchArray)` |
| Velocity computation | `velBase + TRand.ar * velRndm + Select.ar(accentPattern) * accStr` |
| Duration computation | `durBase + Select.ar(durPattern) * durEucStr + TRand.ar * durRndm` |
| Step chance | `TRand.ar(0,1,trig) < stepChance` |
| Note chance per voice | `TRand.ar(0,1,voiceTrig) < noteChance` |
| Strum (time-offset per voice) | `DelayN.ar(trig, maxStrum, i * strumTime)` |
| Gate envelope per voice | `Trig.ar(voiceTrig, Latch.ar(dur, voiceTrig))` |
| Gate output | N-channel audio bus, voice i always on channel i |
| Pitch/velocity output | `Latch.ar(value, voiceTrig)` — N-channel, voice i always on channel i |

---

## 4. Data Sharing: `OceanodeParameter.ar` Arrays (No Buffers)

The key insight from examining the existing sequencer SynthDefs is that value arrays are shared with SC via **wide `OceanodeParameter.ar` declarations**, not SC Buffers. For example, from `channelsequencer.scd`:

```supercollider
// File: /Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Sequencer/channelsequencer.scd

maxSteps = 64;
stepValues = OceanodeParameter.ar(\stepvalues, 0, maxSteps, 0, 1, "vf");
// ...
stepValue = Select.ar(currentStep, stepValues);
```

A 64-wide `OceanodeParameter.ar` allocates 64 control inputs on the SC Synth. C++ sets these via `synth->set(name, value)` / `synth->setMultiple(...)` whenever parameters change. `Select.ar(index, array)` picks the value at `index` at audio rate — zero-latency lookup.

For the arpeggiator, the following arrays will be shared this way (all `maxSteps = 64`):

| SC parameter name | Content | C++ source |
|---|---|---|
| `\pitchvalues` | MIDI pitch floats 0..127 | `currentPitches[]` |
| `\eucgate` | Euclidean gate pattern (0.0/1.0) | `euclideanPattern[]` |
| `\eucaccent` | Euclidean accent pattern (0.0/1.0) | `euclideanAccents[]` |
| `\eucdur` | Euclidean duration pattern (0.0/1.0) | `euclideanDurations[]` |

---

## 5. SynthDef Architecture

### Family naming

Following framework convention, the SynthDef is parameterized by **polyphony count** (number of simultaneous voices). Compiled variants: `ScPolyArp1`, `ScPolyArp2`, ..., `ScPolyArp8` (or up to 16). The C++ node selects the right variant when `polyphony` changes, exactly like `MetroSequential` and `ChannelSequencer`.

`seqSize` is a **runtime KR parameter** (not a SynthDef dimension) since the arrays are fixed at `maxSteps=64` width and `seqSize` just clamps the modulo.

### Core SC signal flow (pseudocode for voice i of N)

```supercollider
~synthCreator.value("ScPolyArpeggiator", {|n|  // n = polyphony

    var maxSteps = 64;

    // ── INPUTS ──
    var trigIn       = In.ar(OceanodeInput.kr(\trig), 1);
    var resetIn      = OceanodeParameter.ar(\reset, 0, 1, 0, 1, "vi");
    var reseedIn     = OceanodeParameter.ar(\reseed, 0, 1, 0, 1, "vi");

    // ── SEQUENCE DATA (from C++) ──
    var pitchValues  = OceanodeParameter.ar(\pitchvalues,  60, maxSteps, 0,   127,  "vf");
    var eucGate      = OceanodeParameter.ar(\eucgate,       1, maxSteps, 0,   1,    "vf");
    var eucAccent    = OceanodeParameter.ar(\eucaccent,     0, maxSteps, 0,   1,    "vf");
    var eucDur       = OceanodeParameter.ar(\eucdur,        0, maxSteps, 0,   1,    "vf");

    // ── SCALAR PARAMETERS ──
    var seqSize      = OceanodeParameter.kr(\seqsize,       16, 1, 1, maxSteps,     "i");
    var eucLen       = OceanodeParameter.kr(\euclen,         8, 1, 1, maxSteps,     "i");
    var eucAccLen    = OceanodeParameter.kr(\eucacclen,      4, 1, 1, maxSteps,     "i");
    var eucDurLen    = OceanodeParameter.kr(\eucdurlen,      4, 1, 1, maxSteps,     "i");
    var polyInterval = OceanodeParameter.kr(\polyinterval,   2, 1, 1, 12,           "i");
    var velBase      = OceanodeParameter.ar(\velbase,      0.8, 1, 0, 1,            "vf");
    var velRndm      = OceanodeParameter.ar(\velrndm,      0.1, 1, 0, 1,            "vf");
    var accStr       = OceanodeParameter.ar(\accstr,       0.2, 1, 0, 1,            "vf");
    var durBase      = OceanodeParameter.ar(\durbase,      100, 1, 1, 5000,         "vf");
    var durRndm      = OceanodeParameter.ar(\durrndm,       20, 1, 0, 1000,         "vf");
    var durEucStr    = OceanodeParameter.ar(\dureucstr,     50, 1, -5000, 5000,     "vf");
    var stepChance   = OceanodeParameter.ar(\stepchance,   1.0, 1, 0, 1,            "vf");
    var noteChance   = OceanodeParameter.ar(\notechance,   1.0, 1, 0, 1,            "vf");
    var strumTime    = OceanodeParameter.ar(\strum,        0.0, 1, 0, 500,          "vf");
    var seed         = OceanodeParameter.kr(\seed,           0, 1, 0, 65536,        "i");
    var skipSteps    = OceanodeParameter.kr(\skipsteps,      0, 1, 0, 32,           "i");

    var instanceID   = (Date.localtime.rawSeconds * 1000 + 1000.rand).asInteger;

    // ── RESET ──
    var combinedReset = Trig.ar(
        Trig1.ar(trigIn > 0.1, SampleDur.ir) * 0 // placeholder — see below
        + K2A.ar(resetIn[0]) + K2A.ar(reseedIn[0] > 0.5),
        SampleDur.ir
    );

    // ── INCOMING TRIGGER (normalised) ──
    var trig = Trig1.ar(trigIn > 0.1, SampleDur.ir);

    // ── STEP COUNTER (advances by 1 + skipSteps per trigger) ──
    // PulseCount gives ever-incrementing integer; wrap by seqSize
    var rawCount  = PulseCount.ar(trig, combinedReset);
    var stepIndex = (rawCount * (skipSteps + 1)) % seqSize;

    // ── EUCLIDEAN GATE FILTER ──
    var eucGateIndex  = stepIndex % eucLen;
    var isEucActive   = Select.ar(eucGateIndex, eucGate) > 0.5;
    var stepChanceOK  = Trig1.ar(trig, SampleDur.ir)
                        * (TRand.ar(0, 1, trig) < stepChance.asArray[0]);
    var gateTrig      = trig * isEucActive * stepChanceOK;

    // ── PER-STEP VELOCITY (computed once, shared across all voices) ──
    var accentIndex   = stepIndex % eucAccLen;
    var isAccented    = Select.ar(accentIndex, eucAccent) > 0.5;
    RandID.ir(instanceID + 500);
    var velRand       = TRand.ar(0, 1, gateTrig);
    var stepVel       = (velBase.asArray[0]
                         + (velRand * velRndm.asArray[0])
                         + (isAccented * accStr.asArray[0])).clip(0, 1);

    // ── PER-STEP DURATION (computed once, shared across all voices) ──
    var durAccIndex   = stepIndex % eucDurLen;
    var isDurAccented = Select.ar(durAccIndex, eucDur) > 0.5;
    RandID.ir(instanceID + 600);
    var durRand       = TRand.ar(0, 1, gateTrig);
    var stepDur       = (durBase.asArray[0]
                         + (isDurAccented * durEucStr.asArray[0])
                         + (durRand * durRndm.asArray[0])).clip(1, 60000);

    // ── PER-VOICE OUTPUTS ──
    var gateArray  = Array.new(n);
    var pitchArray = Array.new(n);
    var velArray   = Array.new(n);
    var durArray   = Array.new(n);

    n.do {|i|
        // Voice slot index into pitch sequence
        var voiceStep   = (stepIndex + (i * polyInterval)) % seqSize;

        // Pitch lookup
        var pitch       = Select.ar(voiceStep, pitchValues);

        // Per-voice strum delay
        var strumDelay  = i * strumTime.asArray[0] * 0.001; // ms → seconds
        var maxDelay    = 0.6; // 600ms max strum
        var voiceTrig   = DelayN.ar(gateTrig, maxDelay, strumDelay);

        // Note chance per voice (independent random draw)
        RandID.ir(instanceID + (i * 1000));
        var noteOK      = TRand.ar(0, 1, voiceTrig) < noteChance.asArray[0];
        voiceTrig       = voiceTrig * noteOK;

        // Latch pitch, velocity, duration at voice trigger time
        var pitchLatched = Latch.ar(pitch,    voiceTrig);
        var velLatched   = Latch.ar(stepVel,  voiceTrig);
        var durLatched   = Latch.ar(stepDur,  voiceTrig) * 0.001; // ms → seconds

        // Gate: held for durLatched seconds
        var gate        = Trig.ar(voiceTrig, durLatched);

        gateArray[i]  = gate;
        pitchArray[i] = pitchLatched;
        velArray[i]   = velLatched;
        durArray[i]   = durLatched * 1000; // back to ms for output
    };

    Out.ar(OceanodeOutput.kr(\gateout),  gateArray);
    Out.ar(OceanodeOutput.kr(\pitchout), pitchArray);
    Out.ar(OceanodeOutput.kr(\velout),   velArray);
    Out.ar(OceanodeOutput.kr(\durout),   durArray);

}, description: "Polyphonic arpeggiator with euclidean patterns, audio-rate trigger and gate",
   category: "Modulation/Sequencer");
```

> **Note:** The above is design pseudocode illustrating the intended logic, not final compilable SC. KR/AR rate mismatches, `OceanodeParameter` exact signatures, and `~synthCreator` conventions will need to match the framework exactly when implemented.

---

## 6. C++ Node Design

The C++ node (`scPolyphonicArpeggiator`) inherits from `scSynthdef` (or `scNode` directly), following the same pattern as all other SC nodes in the framework.

### What C++ manages

1. **All the same parameters as the current `polyphonicArpeggiator`** — scale, pattern, deviations, euclidean parameters, velocity, duration, strum, snapshot/morph, etc.
2. **Rebuilding and uploading arrays** to SC whenever parameters change:
   - `rebuildPitchSequence()` → calls `synth->set("pitchvalues", currentPitches)` (64 floats)
   - `rebuildEuclideanPattern()` → `synth->set("eucgate", ...)` (64 floats, 0.0/1.0)
   - Same for accent and duration euclidean patterns
3. **The `nodePort` trigger input** — wired from any upstream SC clock/metro node
4. **The `nodePort` gate/pitch/vel/dur outputs** — audio-rate buses available for downstream SC synths

### What C++ no longer manages

- Step counter (no `currentStep`, no `processStep()`)
- Gate on/off timing (no `noteDurationsMs`, no `noteStartTimes`, no `update()` loop)
- Strum scheduling (no timer-based delayed gate-on)
- Velocity and duration computation per trigger
- Slot-collision detection (no `slotIsSustaining` check — each voice has a fixed dedicated output channel, so collisions are architecturally impossible)

### Key difference from current node

The current node's `onTrigger()` / `processStep()` / `update()` loop is entirely replaced by the SC SynthDef. The C++ side is now purely a **parameter manager and data uploader** — it reacts to parameter changes and pushes data to SC, but never processes individual note events.

The critical bug in the current C++ node — where `outputIndex = (baseIndex + voice * polyInt) % sz` was used for **both** pitch lookup **and** output slot — is resolved by separating the two concerns:
- **Pitch lookup index**: `(baseIndex + i * polyInterval) % seqSize` — into `currentPitches[]`
- **Output slot**: always `i` (voice index) — fixed channel, never collides

---

## 7. Reference Files

### Existing C++ node (current implementation — all logic to be understood before writing SC version)

| File | Description |
|---|---|
| `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxSantiNodes/src/polyphonicArpeggiator.cpp` | Main implementation — processStep(), computeStepVelocity(), computeStepDuration(), rebuildPitchSequence(), euclidean generation |
| `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxSantiNodes/src/polyphonicArpeggiator.h` | Header — all parameter declarations, state vectors, constants |

### SC framework — C++ side

| File | Description |
|---|---|
| `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/scSynthdef.h` | Base class for all dynamic SC nodes — how params are parsed, synth created, buses mapped |
| `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/scNode.h` | nodePort system, bus allocation, graph ordering |
| `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/serverManager.h` | Graph topology, bus allocation (MAX_NODE_CHANNELS=128), topological sort |
| `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/scA2k.h` | A2K bridge (audio bus → C++ float vector, for reference) |

### SC framework — SynthDef side (key reference patterns)

| File | Pattern / Why Relevant |
|---|---|
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Sequencer/channelsequencer.scd` | **Most relevant**: 64-wide `OceanodeParameter.ar` array + `Select.ar` lookup + `Stepper.ar` channel routing + `Latch.ar` value holding |
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Trigger/metrosequential.scd` | `PulseCount.ar` step counter + `Phasor.ar` clock + `combinedReset` + `BinaryOpUGen('==')` channel demux + `TRand.ar` seeded per-channel chance |
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Trigger/gateduration.scd` | `Trig.ar(input, duration)` — exact gate envelope pattern to use per voice |
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Sequencer/durationsequencer.scd` | `PulseCount.ar` + `% n` index wrapping + per-channel `TRand.ar` + `BinaryOpUGen('==')` demux |
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Random/chance.scd` | `TRand.ar` + `RandID.ir` + `RandSeed.kr` seeded random pattern — use for stepChance and noteChance |
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Random/random.scd` | `TRand.ar` + `Latch.ar` per trigger — use for velRndm and durRndm |
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Trigger/metro.scd` | `Phasor.ar` + `HPZ1.ar` wrap-trigger — reference for how AR trigger inputs are normalized |
| `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SPECIAL_SYNTHDEFS/KR/a2k.scd` | A2K bridge SynthDef — how audio buses are read back into C++ if needed |

---

## 8. Critical SC Patterns to Replicate

### Pattern 1 — Normalising incoming trigger
From `metrosequential.scd` and `gateduration.scd`:
```supercollider
trig = Trig1.ar(input > 0.1, SampleDur.ir);
```

### Pattern 2 — Combined reset
From `metrosequential.scd`:
```supercollider
combinedReset = Trig.ar(
    K2A.ar(reset.asArray[0]) + K2A.ar(reseedKr > 0.5),
    SampleDur.ir
);
```

### Pattern 3 — Step counter with wrap
From `durationsequencer.scd`:
```supercollider
counter   = PulseCount.ar(stepTrig, combinedReset);
index     = (counter - 1) % n;
```
For the arpeggiator, `n` is replaced by `seqSize` (a KR parameter).

### Pattern 4 — Value lookup from 64-wide array
From `channelsequencer.scd`:
```supercollider
stepValues  = OceanodeParameter.ar(\stepvalues, 0, maxSteps, 0, 1, "vf");
stepValue   = Select.ar(currentStep, stepValues);
```

### Pattern 5 — Per-channel latch
From `channelsequencer.scd`:
```supercollider
valueArray[i] = Latch.ar(stepValue.asArray[0], chanTrig);
```

### Pattern 6 — Gate envelope from trigger + duration
From `gateduration.scd`:
```supercollider
trig = Trig1.ar(input > 0.1, 0.001);
env  = EnvGen.ar(
    Env.new([0, 1, 1, 0], [0, duration * 0.001, 0], \lin),
    gate: trig
);
```
Or more simply, using `Trig.ar` directly:
```supercollider
gate = Trig.ar(voiceTrig, Latch.ar(durSeconds, voiceTrig));
```

### Pattern 7 — Strum via DelayN
```supercollider
var voiceTrig = DelayN.ar(gateTrig, maxStrumSeconds, i * strumSeconds);
```

### Pattern 8 — Per-trigger random with seeding
From `metrosequential.scd` and `chance.scd`:
```supercollider
instanceID = (Date.localtime.rawSeconds * 1000 + 1000.rand).asInteger;
RandID.ir(instanceID + (i * 1000));
TRand.ar(0, 1, trig);  // new random float on each trigger
```

### Pattern 9 — Channel demux (polyphony output routing)
From `metrosequential.scd`:
```supercollider
signals = Array.fill(n, {|i|
    var index_match = BinaryOpUGen.new('==', currentIndex, i);
    trigWithReset * index_match * chance_test;
});
```
For the arpeggiator, replace `currentIndex` with `(stepIndex + i * polyInterval) % seqSize`.

---

## 9. Implementation Steps

### Step 1 — Write the SynthDef file
- Create `/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SYNTHDEFS/Modulation/Sequencer/scpolyarpeggiator.scd`
- Follow `~synthCreator.value("ScPolyArpeggiator", {|n| ... })` convention
- Implement all patterns described in Section 4 and 7
- Test in isolation in SC IDE with a manual `Stepper` before hooking into the C++ node

### Step 2 — Write the C++ node
- Create `scPolyphonicArpeggiator.h` and `scPolyphonicArpeggiator.cpp` in `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxSantiNodes/src/`
- Inherit from `scSynthdef` (or the appropriate base — check `scSynthdef.h`)
- Copy all parameter declarations from `polyphonicArpeggiator.h`
- Remove: `onTrigger()`, `processStep()`, `update()` gate loop, `noteDurationsMs`, `noteStartTimes`, `currentGates`, `stepGates`
- Keep: `rebuildExpandedScale()`, `rebuildPitchSequence()`, `rebuildDeviations()`, all euclidean generation
- Add: listeners that call `uploadPitchArray()`, `uploadEucArrays()` to SC on any parameter change
- Add: `nodePort` input for trigger, `nodePort` outputs for gate, pitch, vel, dur
- Add: `numChannels` param (= polyphony) that switches SynthDef variant

### Step 3 — Register the node
- Add to the node factory / registration file (wherever `polyphonicArpeggiator` is currently registered)

### Step 4 — Test
- Connect a `Metro` node → `ScPolyArpeggiator` trigger input
- Connect gate outputs to a `GateDuration` or directly to a synth envelope
- Verify all N voices gate simultaneously on accented steps
- Verify strum offsets voices at audio rate
- Verify sustain-longer-than-step-interval works (no cutting — this is now naturally correct in SC since each voice's `Trig.ar` is independent)

---

## 10. Key Advantages of the SC Version

- **Sample-accurate gate-on**: the trigger fires at exactly the audio sample it arrives, not at the next CPU frame
- **Sample-accurate gate-off**: `Trig.ar` duration is in samples, not polled milliseconds
- **Sample-accurate strum**: `DelayN.ar` is exact, replacing the timer-based approach
- **No sustain collision bug**: each voice's `Trig.ar` envelope is independent; a new step cannot cut another voice's sustain short (the issue that required the `slotIsSustaining` check in C++)
- **Velocity/duration consistency across voices**: single `TRand.ar` roll per step, latched into all voices simultaneously — the same bug that was fixed in the C++ version is architecturally impossible here
- **Gate precedes pitch by zero samples**: `Latch.ar(pitch, voiceTrig)` and `Trig.ar(voiceTrig, dur)` happen on the same sample — no ordering issue (unlike the C++ `updateOutputs()` reordering fix)
