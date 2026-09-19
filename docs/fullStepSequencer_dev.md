# fullStepSequencer — Development Guide

> **Status tracking document.** Updated at the end of every development session.
> Last updated: 2026-04-01 (initial planning)

---

## Overview

`fullStepSequencer` is a stereo multitrack step sequencer Oceanode node inspired by classic drum machine/loop-station workflows (Fruity Loops style). Each track plays back an audio sample loaded from disk, with per-step control over volume, probability, filter, resonance, pitch, and pan. Advanced per-track features include: arpeggiation, echo/retrigger, envelope, and mono/poly playback modes. Multiple "slots" hold independent step configurations for the same track layout.

---

## Phased Development Plan

### Phase 1 — Skeleton + File Browser + Basic Sequencer
- [ ] Node skeleton (class, SynthDef, registration)
- [ ] Floating ImGui window (toggled by `Show` parameter)
- [ ] File browser panel: navigate disk, list .wav files, click-to-preview, drag-to-track
- [ ] Per-track step sequencer row: toggles, `numBeats`, `stepsPerBeat`, `shift`, track name
- [ ] VOL tab: per-step volume sliders
- [ ] PROB tab: per-step probability sliders
- [ ] Basic SC synthdef: buffer playback per track, BPM-derived timing, volume, probability
- [ ] Audio output ports: one stereo pair per track
- [ ] Preset save/load: step data, sample paths, numTracks, numBeats, stepsPerBeat, shift, vol, prob

### Phase 2 — Full Parameter Tabs + Envelope + TIME
- [ ] CUT tab: per-step bipolar lowpass/highpass filter slider (cascade 2-pole)
- [ ] RES tab: per-step filter resonance
- [ ] PITCH tab: per-step semitone transposition (quantized) + ScaleQuant toggle
- [ ] PAN tab: per-step bipolar stereo balance (copy from balance.scd approach)
- [ ] ENV tab: global per-track attack/decay/hold/release envelope
- [ ] TIME tab: mono/poly mode toggle, fixDuration toggle, Beats float control
- [ ] Global pitch transposition parameter (affects all tracks)
- [ ] Scale float vector input port
- [ ] SC SynthDef update for all new parameters

### Phase 3 — Slots + ARP + ECHO
- [ ] Slots system: up to 16 slots per sequencer, hard cut switching, all step data saved per slot
- [ ] ARP tab: Speed (beat divisions), Interval (semitones), ScaleQuant, Modulo — runs in SC audio-rate
- [ ] ECHO tab: numTaps, FadeVol, FadeCut — runs in SC audio-rate
- [ ] `x` tab: collapses parameter panel, shows only step row + tabs
- [ ] Slot UI: slot selector buttons, slot copy/paste

### Phase 4 — Polish + Preset Embed
- [ ] Sample embed in preset folder (copy from `scBuffer` pattern)
- [ ] Sample embed in macro folder (same as scBuffer)
- [ ] Unembed / restore original path button
- [ ] Last-opened-folder persistence in preset data
- [ ] Global Reset parameter wired to SC phasor reset
- [ ] numTracks change at runtime (add/remove track rows and SC synths)
- [ ] All preset data survives Oceanode preset load/save

---

## Architecture Decisions

### Node Archetype
**Archetype A — `scNode` subclass.** The node has audio output ports that connect to downstream Oceanode nodes. It participates in `serverManager`'s graph rebuild lifecycle. Each track's synth is ordered in the SC node tree like any other scNode.

### Timing Architecture
Timing runs entirely in SC, not C++. The design:
- C++ reads BPM from `parentContainer->getBpm()` (via `setContainer()` override)
- BPM is sent to each track synth as a `\bpm` KR parameter
- Each track synthdef independently computes its step clock:
  ```supercollider
  // step rate in Hz: beats_per_second * steps_per_beat
  var stepHz = (bpm / 60.0) * (stepsPerBeat / 4.0 * 4.0);
  // wait — stepsPerBeat means "steps per beat", so:
  // stepHz = (bpm / 60.0) * stepsPerBeat_actual_per_beat
  ```
- A `Phasor` or `Sweep`+`HPZ1` approach generates per-step triggers at audio rate
- `shift` parameter offsets the starting step index
- A `\reset` trigger (audio-rate, sent from C++ via `set()`) resets the phasor
- Polyrhythm is automatic: different `numBeats`/`stepsPerBeat` per track simply yields different step lengths

### BPM Access Pattern
No `getBpm()` exists. The container exposes:
- `void setBpm(float)` — sets BPM (called by BPMControl node)
- `ofEvent<float> changedBpmEvent` — fires on every BPM change (public)
- `float bpm` — private, no getter

Correct pattern:
```cpp
ofxOceanodeContainer* parentContainer = nullptr;
ofEventListener bpmListener;

void setContainer(ofxOceanodeContainer* container) override {
    scNode::setContainer(container);
    parentContainer = container;
    bpmListener = container->changedBpmEvent.newListener([this](float& b){
        for(auto& [server, synths] : trackSynths)
            for(auto* s : synths)
                if(s) s->set("bpm", b);
    });
}
```

### SC Synth Topology
- **One SynthDef per track** (not one global SynthDef for all tracks)
- SynthDef name: `"FullStepSeqTrack"` (single variant — no per-channel SynthDef explosion needed; it always outputs 2 channels stereo)
- Per-server, per-track: `std::map<ofxSCServer*, std::vector<ofxSCSynth*>> trackSynths`
- Up to **12 tracks** max. `numTracks` is dynamic — changing it at runtime adds/removes synth instances
- Each track synth reads: one buffer, step toggle array, vol array, prob array, and all parameter arrays
- Arrays sent to SC as `vf` type parameters (float vectors)

### Step Data Arrays
Maximum steps per track: **64** (4 beats × 16 steps per beat). Arrays are always 64 floats wide; unused slots are ignored by the synthdef using the current `seqSize = numBeats * stepsPerBeat` value.

Parameters sent as arrays (all 64-wide `vf`):
- `\steps` — toggle on/off (0.0 or 1.0)
- `\vols` — step volumes [0..1]
- `\probs` — step probabilities [0..1]
- `\cuts` — bipolar filter [-1..1]
- `\ress` — filter resonance [0..1]
- `\pitches` — semitone transposition [-12..12]
- `\pans` — stereo balance [-1..1]

Scalar parameters per track synth:
- `\bpm` — beats per minute (kr)
- `\numBeats` — number of beats (kr, int)
- `\stepsPerBeat` — steps per beat (kr, int)
- `\seqSize` — numBeats × stepsPerBeat (kr, computed in C++)
- `\shift` — step offset (kr, int)
- `\bufnum` — SC buffer number (kr)
- `\globalPitch` — global transposition in semitones (kr)
- `\reset` — trigger to reset step counter (ar, vi)
- `\attack`, `\decay`, `\hold`, `\release` — envelope
- `\monoMode` — 0=poly, 1=mono (kr)
- `\fixDuration` — 0/1 (kr)
- `\fixBeats` — duration in beats when fixDuration=1 (kr)

Phase 3 additions:
- `\arpSpeed`, `\arpInterval`, `\arpModulo`, `\arpScaleQuant` (kr)
- `\echoTaps`, `\echoFadeVol`, `\echoFadeCut` (kr)
- `\scale` — scale degrees vector (vf, 12 wide max)

### Floating ImGui Window
Uses the same pattern as `scFFT` and `scWavescope`:
```cpp
// Parameter
ofParameter<bool> showWindow;

// In setup():
addParameter(showWindow.set("Show", false));

// Override draw():
void draw(ofEventArgs&) override {
    if(!showWindow) return;
    string title = "FullStepSequencer " + ofToString(getNumIdentifier());
    if(ImGui::Begin(title.c_str(), (bool*)&showWindow.get(), ImGuiWindowFlags_NoScrollbar)) {
        drawFileBrowser();   // left panel
        drawTrackArea();     // right panel: all tracks
    }
    ImGui::End();
}
```

### Output Ports
One stereo output `scNode` port per track. Implemented as a vector of ports or via multiple `addOutput()` calls with a dynamic count. The Oceanode bus routing to downstream effects (scOutput, reverb chains, etc.) works through the standard `scNode` port/bus mechanism.

**Implementation approach:** Rather than a fixed number of ports, use a `numTracks` listener that dynamically adjusts the output port list. This needs careful coordination with `serverManager` since port changes trigger graph rebuilds.

**Simpler alternative for Phase 1:** Start with a fixed 12 output ports (stereo pairs = 24 channels on one `vector<float>` output parameter), then refactor in Phase 4 to separate ports.

### File Browser
Reference: `scBufferBrowser.cpp` (working implementation of browser + preview + buffer loading).

- Buffer loading: **one `ofxSCBuffer*` per server per track**, loaded with `buf->read(path)` — NOT `readChannel()`. This loads the full stereo file into one buffer. `PlayBuf.ar(2, bufnum)` then works correctly. `readChannel()` produces a mono buffer that causes `"Buffer UGen channel mismatch"` + silence.
- Preview: same pattern as scBufferBrowser — `buf->read(path)` + `ofxSCSynth("FullStepSeqPreview")` added to tail, self-freeing via `doneAction:2`
- Portable paths: save as absolute paths in presets; resolve back on load. Mirror on all servers.
- Drag-and-drop from file browser to track row sets that track's buffer

---

## File Structure

```
src/
  fullStepSequencer.h      ← node class (header-only or split)
  fullStepSequencer.cpp    ← implementation

KR synthdefs (generate with .scd):
  /Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/NEWEST/SPECIAL_SYNTHDEFS/KR/
    fullStepSequencer.scd             ← generates FullStepSeqTrack synthdef
    fullStepSequencerPreview.scd      ← generates FullStepSeqPreview (file preview)
```

---

## SynthDef Design Notes

### FullStepSeqTrack (one instance per track)

```supercollider
SynthDef(\FullStepSeqTrack, {
    // --- Parameters ---
    var bpm        = OceanodeParameter.kr(\bpm,          120, 1, 1, 999, "f");
    var numBeats   = OceanodeParameter.kr(\numBeats,       4, 1, 1,  32, "i");
    var stepsPerBt = OceanodeParameter.kr(\stepsPerBeat,   4, 1, 1,  16, "i");
    var seqSize    = OceanodeParameter.kr(\seqSize,       16, 1, 1,  64, "i");
    var shift      = OceanodeParameter.kr(\shift,          0, 1, 0,  63, "i");
    var bufnum     = OceanodeParameter.kr(\bufnum,         0, 1, 0, 4096, "i");
    var globalPitch = OceanodeParameter.kr(\globalPitch,   0, 1,-24,  24, "f");
    var reset      = OceanodeParameter.ar(\reset,          0, 1, 0,   1, "vi");

    // Array params (64 wide) — sent as "vf"
    var steps   = OceanodeParameter.ar(\steps,   0, 64, 0, 1,   "vf");
    var vols    = OceanodeParameter.ar(\vols,    1, 64, 0, 1,   "vf");
    var probs   = OceanodeParameter.ar(\probs,   1, 64, 0, 1,   "vf");
    // ... cuts, ress, pitches, pans

    // --- Step Clock ---
    // stepHz = steps_per_second = (bpm/60) * stepsPerBeat
    var stepHz = (bpm / 60.0) * stepsPerBt.kr;
    var phas   = Phasor.ar(reset, stepHz / SampleRate.ir, 0, seqSize.kr);
    var stepIdx = phas.floor;
    var stepTrig = HPZ1.ar(stepIdx) != 0;  // trigger on each step change

    // --- Index into arrays ---
    var shiftedIdx = (stepIdx + shift.kr) % seqSize.kr;
    var isOn    = Select.ar(shiftedIdx, steps);
    var stepVol = Select.ar(shiftedIdx, vols);
    var stepProb = Select.ar(shiftedIdx, probs);
    // probability gate:
    var probOK  = CoinGate.ar(stepProb, stepTrig);
    var gateTrig = probOK * isOn;

    // --- Playback rate from pitch ---
    var semitones = Select.ar(shiftedIdx, pitches) + globalPitch;
    var rate = 2.0 ** (semitones / 12.0);

    // --- Trigger voices ---
    // IMPORTANT: bufnum must point to a STEREO buffer loaded via buf->read(path)
    // NOT readChannel() — that produces mono buffers which cause channel mismatch + silence.
    // (Phase 1: simple mono trigger — for poly mode see Phase 3)
    var sig = PlayBuf.ar(2, bufnum, rate, gateTrig, 0, doneAction: 2);

    // --- Filter, pan, volume ---
    // ... (Phase 2)

    Out.ar(OceanodeOutput.kr(\out), sig * stepVol);
}).writeDefFile(subDir);
```

> Note: `Select.ar` with a dynamic index and fixed array is the standard SC pattern for step-indexed parameter lookup. Arrays must be compiled into the SynthDef as `NamedControl` arrays via `OceanodeParameter.ar(\name, default, size, min, max, "vf")`.

### FullStepSeqPreview (file browser preview)
A minimal one-shot buffer player added to tail for preview playback:
```supercollider
SynthDef(\FullStepSeqPreview, { |bufnum=0, gate=1|
    var sig = PlayBuf.ar(2, bufnum, 1.0, 1, 0, doneAction: 2);
    sig = sig * EnvGen.kr(Env.asr(0.01, 1, 0.1), gate, doneAction: 2);
    Out.ar(0, sig);
}).writeDefFile(subDir);
```

---

## Internal Data Structures (C++)

```cpp
// Per-track step data (one slot's worth)
struct StepTrackData {
    int  numBeats     = 4;
    int  stepsPerBeat = 4;
    int  shift        = 0;
    std::string name  = "";
    bool scaleQuant   = false;

    // 64-wide arrays
    std::array<bool,  64> steps   = {};  // toggle on/off
    std::array<float, 64> vols    = {};  // [0,1]
    std::array<float, 64> probs   = {};  // [0,1]
    std::array<float, 64> cuts    = {};  // [-1,1]
    std::array<float, 64> ress    = {};  // [0,1]
    std::array<float, 64> pitches = {};  // [-12,12] semitones
    std::array<float, 64> pans    = {};  // [-1,1]

    // Envelope (global for track, not per-step)
    float envAttack  = 0.01f;
    float envDecay   = 0.1f;
    float envHold    = 0.0f;
    float envRelease = 0.2f;

    // TIME
    bool  monoMode    = true;
    bool  fixDuration = false;
    float fixBeats    = 1.0f;

    // Phase 3
    float arpSpeed      = 1.0f;
    int   arpInterval   = 12;
    int   arpModulo     = 4;
    bool  arpScaleQuant = false;
    int   echoTaps      = 0;
    float echoFadeVol   = 0.5f;
    float echoFadeCut   = 0.0f;
};

// One slot holds data for all tracks
struct SequencerSlot {
    std::vector<StepTrackData> tracks; // size = numTracks
};

// Node-level data
int                    numTracks = 4;
int                    activeSlot = 0;
std::array<SequencerSlot, 16> slots;
std::vector<std::string> samplePaths; // one per track (absolute)
std::vector<std::string> trackNames;
```

---

## Preset Serialization

### Available hooks (from `ofxOceanodeNodeModel.h`)

| Method | Signature | When called | Use for |
|---|---|---|---|
| `presetSave` | `(ofJson &json)` | On preset save | Quick scalar extras |
| `macroSave` | `(ofJson &json, string path)` | On macro save | Full step data + sample paths |
| `presetRecallAfterSettingParameters` | `(ofJson &json)` | After params loaded | Rebuild SC synths / UI from loaded data |
| `loadBeforeConnections` | `(ofJson &json)` | Before connections | Restore numTracks so ports exist before connections are wired |
| `macroLoad` | `(ofJson &json, string path)` | On macro load | Full step data + sample paths |

### Plan

1. **`loadBeforeConnections`** — restore `numTracks` first, so correct number of output ports exist before Oceanode tries to restore connections
2. **`macroSave`** — serialize all slot step arrays as JSON arrays directly into the node's json object (no separate file needed for step data). Sample paths stored as relative paths using scBuffer's pattern
3. **`macroLoad`** — deserialize step arrays, resolve sample paths, reload buffers, update SC synths
4. **`numTracks`, `activeSlot`** — regular `ofParameter<int>`, Oceanode serializes automatically

Example macroSave structure:
```json
{
  "slots": {
    "0": {
      "tracks": [
        { "numBeats": 4, "stepsPerBeat": 4, "shift": 0, "name": "Kick",
          "steps": [1,0,0,0,1,0,0,0,...],
          "vols":  [1,1,1,1,...],
          "probs": [1,1,1,1,...] },
        ...
      ]
    },
    ...
  },
  "samplePaths": ["samples/kick.wav", "samples/snare.wav", ...]
}
```

---

## Floating Window Layout

```
┌─────────────────────────────────────────────────────────────────────────┐
│  FullStepSequencer 1                                              [ x ]  │
├──────────────┬──────────────────────────────────────────────────────────┤
│              │  Track 1  [Kick     ] beats:4 stp:4 shift:0              │
│  FILE        │  [■][□][■][□][■][□][■][□][■][□][■][□][■][□][■][□]      │
│  BROWSER     │  [VOL][PROB][CUT][RES][PITCH][PAN][ENV][TIME][ARP][ECHO][x] │
│              │  ▼ (active tab content)                                  │
│  /samples/   │  ─────────────────────────────────────────────────────── │
│  kick.wav    │  Track 2  [Snare    ] beats:4 stp:4 shift:0              │
│  snare.wav   │  [□][□][■][□][□][□][■][□][□][□][■][□][□][□][■][□]      │
│  hihat.wav   │  [VOL][PROB][...][x]                                     │
│  ...         │  ...                                                     │
│              │  [ + Add Track ]                                         │
└──────────────┴──────────────────────────────────────────────────────────┘
```

**Key ImGui widgets:**
- Step toggles: `ImGui::Checkbox` or custom colored `ImGui::Button` (■/□)
- Step parameter sliders: `ImGui::SliderFloat` aligned with toggle columns
- Parameter tabs: `ImGui::BeginTabBar` / `ImGui::TabItem`
- File browser: custom `ImGui::Selectable` list with `ImGui::ListBox`
- Track rows: `ImGui::BeginChild` per track for isolated scrolling
- Drag source: `ImGui::BeginDragDropSource` on file names
- Drop target: `ImGui::BeginDragDropTarget` on track header area

---

## Key References

| Topic | Location |
|---|---|
| scNode base class | `src/scNode.h` |
| File browser / WAVE loading | `src/scBuffer.h` (read fully before Phase 1) |
| Floating window pattern | `src/scFFT.h` lines 121–134 |
| BPM container access | `ofxSantiNodes/src/BPMControl.h` lines 53–58 |
| Dynamic input ports pattern | `src/scPolyMixer.cpp` — `addTrackToGUI()`, `removeTrackFromGUI()`, `updateTrackCount()` |
| `scNode::removeInput` implementation | `src/scNode.cpp` lines 34–49 (mirror for future `removeOutput`) |
| Step array SC pattern (vf arrays, Select.ar) | `KR/scpolyarpeggiator.scd` |
| Balance / stereo pan SCD | `SYNTHDEFS/Spatial/` (verify path) |
| Cascade 2-pole filter | Look for `SVF` or `RLPF`/`RHPF` cascade in existing synthdefs |
| Node registration | `src/ofxOceanodeSuperCollider.h` |
| Full architecture guide | `DEVELOPMENT_GUIDE.md` |

---

## Session Log

### 2026-04-01 — Phase 1 implementation

**Files written:**
- `KR/fullStepSequencer.scd` — `FullStepSeqTrack` SynthDef (Impulse clock + Stepper + Select.ar arrays + PlayBuf)
- `src/scNode.h/.cpp` — added `removeOutput(int index)` (mirrors `removeInput`)
- `src/fullStepSequencer.h` — full class declaration (Archetype A, MAX_TRACKS=12, MAX_STEPS=64)
- `src/fullStepSequencer.cpp` — full Phase 1 implementation
- `src/ofxOceanodeSuperCollider.h` — include + `registerModel<fullStepSequencer>("SuperCollider")`

**Architecture implemented:**
- `scNode` subclass with `numTracks` dynamic output ports (`Out 1` … `Out N`)
- BPM: subscribed to `parentContainer->changedBpmEvent` in `setContainer()` override
- SC timing: `Impulse.ar(stepHz)` → `Stepper.ar` → `Select.ar` into 64-wide `OceanodeParameter.ar` arrays
- Preset: `loadBeforeConnections` restores port count before Oceanode re-wires; `macroSave`/`macroLoad` serialize all track/step data as JSON; `presetRecallAfterSettingParameters` reloads buffers
- numTracks decrease: ports/synths beyond n remain but are silent (no buffer); full cleanup on next preset load

**CONFIRMED BUGS that caused complete silence — must be fixed before Phase 2:**

1. **Buffer channel mismatch (root cause of "no sound" + "Buffer UGen: no buffer data"):**
   `readChannel(path, {0})` loads a **mono** buffer. `PlayBuf.ar(2, bufnum)` expects a **stereo** buffer. SC prints `"Buffer UGen channel mismatch: expected 2, got 1"` and outputs **silence** — no crash. The fix: load samples with `buf->read(path)` (stereo, no channel split), one `ofxSCBuffer*` per track. Use `PlayBuf.ar(2, bufnum, ...)` — this matches the stereo buffer correctly.

2. **`create()` + `run()` race (caused synth to be immediately paused):**
   If `create()` and `run(getActive())` were called separately, `run()` was stored as a deferred `/n_run` message. On `/n_go` callback this fired as `n_run 0`, pausing the synth immediately. Fix: always use `createAndRun(0, 1, getActive())` — single atomic bundle. scFM7Drone does this correctly.

3. **SC `var` after statement in SynthDef:** Every SynthDef rewrite introduced `var` declarations after statements, preventing the SynthDef from compiling. SC silently keeps the old compiled `.scsyndef` on disk — C++ targets the new name but SC plays the old broken version. Fix: ALL `var` must be at the very top of the function body.

**Known limitations (to fix in Phase 2+):**
- Reducing `numTracks` at runtime doesn't remove ports — requires preset reload
- No per-track color theming
- No beat-separator visual in step grid (just uniform gap)

**Next session: compile-test and fix any build errors, then begin Phase 2 (CUT, RES, PITCH, PAN, ENV, TIME tabs)**

### 2026-04-01 — Planning session
- Read spec (`stepseq.rtf`)
- Clarified architecture questions with user:
  - Timing in SC (phasor-based, BPM from container)
  - Max 12 tracks, max 16 slots
  - 1 synthdef per track
  - Audio-rate ARP and ECHO
  - Hard cut slot switching (running voices not cut)
  - Each track has its own stereo Oceanode output port
  - Floating ImGui window (not timeline-embedded)
- Explored codebase patterns: scBuffer, scFFT, BPMControl, scPolyArpeggiator SCD
- Wrote this development guide
- **Next session:** Begin Phase 1 — node skeleton + SynthDef skeleton + file browser

---

## Open Questions / Risks

- **`numTracks` dynamic output ports**: `scNode::addOutput()` exists but `scNode::removeOutput()` does NOT (unlike `removeInput()` which was added for scPolyMixer). Two options:
  - **(preferred)** Add `removeOutput(int index)` to `scNode.h/.cpp` — mirrors `removeInput` exactly (calls `removeParameter(name)`, erases from `outputs` vector). The only complication is that `nodePort` bakes its output index at construction time (`outputs.size()`), so reindexing after removal needs care — may need to rebuild the outputs vector on numTracks decrease.
  - **(safe fallback)** Add all 12 output ports upfront in `setup()`, accept unused ports when `numTracks < 12`.
- **`Select.ar` with 64-wide array**: Verify that `OceanodeParameter.ar(\steps, 0, 64, ...)` correctly compiles a 64-slot NamedControl array that `Select.ar` can index into at audio rate. Reference: scpolyarpeggiator.scd uses this pattern successfully.
- **Mono/poly track modes**: In poly mode, multiple voices of the same sample overlap. This requires multiple `PlayBuf` instances gated in a round-robin fashion — similar to the PolyMixer approach. Evaluate using `TGrains` or a fixed-voice approach (e.g. 4 voices per track).
- **ARP audio-rate precision**: The ARP feature generates successive pitch-shifted triggers at a sub-beat subdivision. This is more complex than a simple step sequencer — it's essentially a nested sequencer on top of the step trigger. The scPolyArpeggiator SCD is the best reference for this.
- **ECHO with audio-rate precision**: numTaps echoes per step at exact timing intervals. This is essentially a multi-tap delay trigger. Implementation approach: use `TDelay.ar` or `LocalBuf`-based approach to schedule future triggers at precise sample offsets from the original trigger.
