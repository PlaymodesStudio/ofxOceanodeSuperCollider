# Performance review, 2026-11-22

Read-only review. Four parallel audits covered core/transport, the shared Box
helpers, the largest nodes, and code added since 2026-10-31 together with
SynthDef DSP. Items fixed in `review2026-10-31.md` are not repeated. Nothing
has been profiled: impact is estimated from how often the code runs. The items
marked ✔ were re-read by hand after the audits.

## Tier 1: highest impact

### Server CPU (scsynth)

1. ✔ **Most Box voices never pause when silent.** Only AdditiveBox has a
   DetectSilence + C++ pauser (`additiveBox.scd`, `scAdditiveBox.cpp:1717`).
   FMBox, Aliasing, SteamPipe and Physical voices run forever after release.
   Fix: port the AdditiveBox silbus/pauser pattern to them. Skip voices driven
   by a gate cable, because a paused voice cannot see its gate.
2. ✔ **Grains spawn while the gate is closed.** GrainBox `trig = rawTrig * okK`
   (`scGrainBoxPoly.scd:299`) and PulsarBox `trig` (`pulsarbox.scd:98`) apply
   the envelope only to the output. Up to 500 playheads × 512 grains compute
   silence, and GrainBox's `estK` counts the silent grains, so Max Grains
   thinning starves the audible playheads. Fix: gate the trigger with
   `((gate > 0) + (env > 1e-4)).min(1)`.
3. ✔ **ModEnv runs at audio rate even in its control-rate variant.**
   `modulation.scd:182-188` uses two `EnvGen.ar` per voice and then `A2K`.
   Every Box envelope uses this def (GrainBox: about 1000 audio-rate EnvGens
   per envelope). Fix: `EnvGen.kr` when `audio` is false, latching short gates
   with `Trig1`.
4. ✔ **Timeline Wave Track sends `/n_set` for every clip every frame and never
   pauses its voices.** `timelineWaveTrack.h:354` (`sendParams`, no cache) is
   called from every `applyState` branch, inactive clips included. Since
   4a2bd2b a stretch clip runs Warp1 × 8 overlaps continuously. Fix: send on
   change only, and `/n_run 0` a voice once play=0 and its tail has finished.

### Main thread / transport (ofxSuperCollider)

5. ✔ **Any packet over 1 KB throws and allocates 320 KB.**
   `ofxOscSenderReceiver.cpp:205` serializes into a 1024-byte stack buffer, and
   on overflow it catches `OutOfBufferMemoryException`, allocates 320 KB and
   serializes again. `scSynthControlBatch` and `scControlBusBatch` fill up to
   4 KB on purpose, so every big morph/glide batch, `/n_setn` and `/s_new` pays
   this. Fix: a member scratch buffer (8–16 KB, grows, never shrinks), sending
   straight from `packet.Data()` without the vector copy.
6. ✔ **One UDP datagram per message.** `ofxSCServer::sendMsg` (`:454`) sends
   each `/n_set` immediately. Fix: collect untimed messages into a per-frame
   bundle and flush it at about 8 KB, before any timed or explicit bundle, and
   at the end of the frame. Careful: preserve the `b_latency` stamping and the
   ordering of mixed timed and untimed sends.
7. ✔ **`/status` every frame per server** (`ofxSCServer.cpp:145`). Fix: throttle
   to about 10 Hz.
8. ✔ **Spectrogram:** a full 1920×1080 ping-pong shader pass per instance per
   frame, plus a `textureOut` notify, whether the window is visible or not
   (`scSpectrogram.h:115`). Fix: render only when shown or connected, and
   replace the ping-pong with a ring texture plus `glTexSubImage2D` of one
   column. (Open item from 10-31.)

## Tier 2: medium

- **Morph frames (per Box instance at 60 Hz).**
  - `applyState` in AnalogBox and PolyComb copies both snapshots' JSON several
    times (`scDroneMode::migrated`, the pinned copies) and calls `save()` per
    control just to compare (`scAnalogBox.cpp:1853`, `scPolyComb.cpp:3182`).
  - `scMorphPolicies::preparePair` deep-compares both snapshots every frame
    (`scMorphPolicies.h:266`).
  - The policy accessors build `getEscapedName()` (a string copy plus 14
    replace passes) before every lookup, and `blockedControls` is never cleared
    in `end()`.
  - `scModulation::sendLfoPart` rebuilds all ~35 controls of every LFO per
    frame before the cache discards them.
  - Fix: key the pair by slot plus a revision counter, cache escaped names at
    registration, and use per-LFO dirty flags.
- **`scSynthControlBatch`** allocates a `vector{value}` and a `std::string` per
  scalar `set`, then does two map lookups (`scSynthControlBatch.h:24, 80`). Fix:
  a scalar fast path and assignment through the `find` iterator.
- **scVST main-thread sleeps** in open, preset read/apply/save, create and free
  (500 ms–5 s stalls; `scVST.cpp:1797, 3393, 4277, 4423, 4989, 5160`). Fix:
  deadlines checked in the existing 50 ms tick. (Open item from 10-31.)
- **VelvetReverb:** each parameter tick frees and reallocates the IR buffers.
  The settle frame then generates up to 384k samples per server on the main
  thread and sends about 94 `/b_setn` (`scVelvetReverb.cpp:103, 382, 419`).
  Fix: keep the buffer when its size is unchanged, generate once on
  `std::async`, and reuse the raw encoder `gbpEncodeBSetn`.
- ✔ **PolyMixer meters** poll and notify all tracks every frame
  (`scPolyMixer.cpp:457`). Fix: do it like MixerPro, at 30 Hz and only when
  shown or connected. `updateVUMeters()` (`:2077`) is dead code.
- **Wavescope** renders an FBO preview and notifies `previewOut` every frame
  (`scWavescope.h:141`). Analyzers (FFT, FFTHD, A2K, PitchTracker) notify
  unchanged outputs every frame, and FFT also allocates a vector per frame.
- **EchoBoxTap** runs PitchShift, 4 blur allpasses and 5 filter branches at mix
  0 (`echoBox.scd:86-98`), × 8 taps × channels. **DrumKitBox, PolyComb and
  SampleBox** filters and drives sit behind `Select`, so every branch computes.
  **The SampleBox pad** always builds 8 voices (`samplebox.scd:134`). Fix:
  lean def variants swapped from C++, the same pattern as the morph-only
  variants.
- **Sample embedding** FNV-hashes every sample file in full on every save
  (`scSampleEmbedding.h:49`), including already-embedded files and Span bakes.
  Fix: skip `samples/<hash>` paths and cache by (path, size, mtime).
- **GrainBox `loadWaveformData`** reads the whole file synchronously on the
  main thread (`scGrainBoxPoly.cpp:3507`). Fix: run it async like DrumKitBox,
  or read with a stride.
- **The node-feedback index** in ofxSCServer is rebuilt in full after any new
  synth (`ofxSCServer.cpp:836`). Fix: incremental insert and erase.
- **On / Mute When Stopped toggles** run a full graph rewire
  (`scNode.cpp:56`, `serverManager.cpp:1375`). Fix: a fast path when the
  topology is unchanged.
- **AdditiveBox** recomputes every voice's tables on any sound change
  (`scAdditiveBox.cpp:489`). Fix: per-table dirty flags.

## Tier 3: low

- RhythmBox `trackControlPulls`: about 800 `std::function` calls and 64-element
  vector copies per frame. Its step bus is polled with the window closed.
- MixerPro builds `allStrips()` 4–5 times per frame. The Pause Muted pass is
  O(n²) with an allocation per track.
- `snaps.slotKeys()` and AnalogBox `syncDropdowns` rebuild escaped names and
  strings every frame. SampleBox `rebuildPadControlIndex` runs every frame.
- `requireSynthdefs` and `scBake::synthdefFolder` (on a miss) walk the whole
  synthdef tree, per node construction or per rebuild. Fix: one static
  filename index.
- SteamPipeBoxVoice makes about 47 redundant `K2A` / audio-rate wrappers.
  GrainBoxPolyOut computes biquad coefficients at audio rate with the filter
  off.
- Transport: `ofxOscMessage` is deep-copied into bundles, `/s_new` vector args
  are copied by value, `appendOrderedNodes` takes `parents` by value, and a
  sender-address string is built per received message.
- `setBpm` during tempo ramps: FM, Physical, SteamPipe and Aliasing resend the
  whole sound frame, and Echo, SampleBox and GrainBox send uncached `/n_set`.
- `serverManager.cpp:1769` sleeps 100 ms per synthdef (only with "Load
  Synthdefs On Preset" on).

## Checked and clean

- serverManager rebuild coalescing, the suspension pass, and `/c_getn`
  merging.
- Meters of GrainBox, SlicerBox, BeatRepeat and MixerPro (window-gated, 30 Hz).
- Idle `scModulation::update`.
- The Freesound, SynthDef sync and Bake workers.
- HarmonyBox and DrumKitBox name caching.
- FFT sizes in SpectralFilterBox.

## Suggested order

1. Transport: items 5 + 7, which are small and isolated. Then item 6, the
   bundle coalescing, with a wire-timing test (`tools/` wire_timing_test).
2. Server CPU: items 2 and 3. They are SynthDef edits only and need recompiled
   Defaults (see the Defaults compile + install notes).
3. Item 4, the Wave Track cache and pause.
4. Item 1, the voice pausers, one Box at a time.
5. The morph-frame cleanup (Tier 2, first bullet).

## Status after the fix pass (2026-11-22, uncommitted)

The fix pass ran the same day. Every touched .cpp file and header was
syntax-checked with the BLOOBS Xcode flags. Tests: `tools/box_osc_test/run.py`
and `wire_timing_test.py`, the latter on a fresh object cache (the cache keys
on source mtime only and ignores header edits, so delete it after editing a
header). Per-node tests are listed where they ran. **Nothing has been run in
the app.**

Test repairs, made because both tests already failed to build on the
unchanged tree:

- `wire_timing_test.py` builds without `scBake.cpp` and adds a stub for
  `scBake::drawNodeMenu`.
- `fmbox/osc_test.py` has a `scGlobalPitch` stub.

Other tests that still fail through stub rot, already failing before this
pass:

- macro_morph_test, wavetablebox morph/dropdown: missing `scBake` and
  controller symbols.
- rhythmbox_test: `ofJson` and `trackSent`.
- lfo_outputs, voice_params, engine_contract, box_gate_mode, most of
  tools/polycomb, analogbox osc_swap.

### Done

- **1 Voice pausing**: done for FM, Aliasing, SteamPipe and Physical.
  - In the defs: `PauseSelf` via DetectSilence before the VCA, with the gate
    closed. Controls `\pauser` (default 0) and `\wake`.
  - In C++: `/n_run 1` goes in the note's own gate bundle, so it shares the
    note's timetag (wire test).
  - Pausing is off for gate cables, Drone, running Pulse, Physical with
    audio in, and Aliasing filter LFO without Gate Sync.
  - It composes with Box On and Mute Pauses Sources (NRT-checked).
  - Not sample-exact after a pause: free-running oscillator phases freeze,
    and the difference is within the spread between two notes of the old
    def.
  - Presets that never fall silent before the VCA (Physical Flute, Metal
    pad) never pause.
- **3 ModEnv**: `EnvGen.kr` in the kr variant; edges are read through
  PulseCount, so no gate is lost.
  - Renders differ from the old def by up to 0.03 (5 ms attack).
  - One behaviour change: a cable or Pulse gate shorter than one block opens
    the envelope further than before.
- **4 Wave Track**: the play=0 branch is cached. An idle plain voice gets
  `/n_run 0` and resumes with `/n_run 1` before the reset `/n_set`.
  - Stretch voices are not paused, because in-flight Warp1 grains would
    replay.
- **5 Serializer**: a 24 KB stack buffer, sending in place with no vector
  copy (`withPacket`).
- **7 `/status`** at 10 Hz.
- **8 Spectrogram, Wavescope, FFT, FFTHD**: no poll or notify while neither
  drawn nor read (`src/nodes/analysis/scOutputDemand.h`). While read, they
  still notify every frame. Ping-pong kept.
- **Morph frames**:
  - `scMorphPolicies` pair identity token: no deep compare on A/B and glide
    frames; recall still compares.
  - Single-lookup accessors.
  - AnalogBox/PolyComb `MorphPairCache`.
  - LFO keys cached, `updatePhases` skip.
  - Hoisted `voiceValue` names.
- **`scSynthControlBatch`**: a scalar path with no vector, and one lookup.
- **PolyMixer meters**: 30 Hz while drawn, every frame while a VU output is
  read, otherwise off. Dead `updateVUMeters` removed.
- **MixerPro**: `forEachStrip` (no per-frame vectors), an allocation-free
  pausing pass, and a cached N_Chan lookup.
- **VelvetReverb**: the buffer is kept when its size is unchanged, and the
  IR is generated once, async, shared by the servers.
  - An RT60 change now takes about 0.6 s instead of 0.3 s.
- **Sample embedding**: already-embedded files are skipped, and hashes are
  cached by (path, size, mtime).
- **GrainBox**: an async peak scan; `setBpm` skips an unchanged tempo.
- **Node-feedback index** (ofxSCServer): kept up to date incrementally
  (`reindexNode`). Single-addressee dispatch makes no copy.
- **Transport**: const-reference `/s_new` args, and no sender-address
  string per received message.
- **Tempo ramps**: FM, Physical, SteamPipe and Aliasing send only the
  tempo-dependent controls; Echo and SampleBox send only on a change.
- **RhythmBox**: pulls only for tracks whose config, data or controls
  changed, with a full pass every 60 frames. Step bus polled only with the
  window open.
- **Snapshots and AnalogBox**: cached `slotKeys`; AnalogBox `syncDropdowns`
  lists rebuilt only on change.
- **SampleBox**: `rebuildPadControlIndex` no longer runs every frame.
- **`scSynthdefIndex.h`**: one synthdef-tree walk per process (AnalogBox,
  PolyComb, SampleBox, EchoBox).
  - Sync code may call `scSynthdefIndex::invalidate()` after a sync.
- **SC Bake**: a negative cache in `synthdefFolder`.
- **Bake windows**: NRT progress stat at 5 Hz.
- **Preset share**: SHA cached.
- **Routing matrix and Load bank**: a per-frame cell index; the bank list is
  read once a second.
- **SteamPipeBoxVoice**: 48 `SteamPipeStep.ar` wrappers removed (exact
  null).

Compiled defs installed:

- ModEnv1..32, FMBoxVoice, SteamPipeBoxVoice, AliasingBoxVoice and
  PhysicalBoxVoice in OceanAudio `bin/data`.
- FMBoxVoice also in the BLOOBS, Lightnet and OceanodeScroller `bin/data`
  (nested `FMBox/FMBox/`), and SteamPipeBoxVoice in OceanodeScroller. Those
  copies were older builds and now carry the current source.
- The previous files are in `tmp/perf_def_backups_2026-11-22/`.

### Held

- **2 Grain gating** (GrainBox, PulsarBox): it removes grains that started
  before note-on. Onset rms fell 0.39→0.19 with 1 s grains, and 44 ms
  pulsarets differ at onset.
- **6 Per-frame bundle coalescing**: sends can come from several threads,
  and a shared queue would change live-control timestamps and arrival.
- **scVST sleeps**: each one paces or orders plugin messages, or a caller
  needs the reply before it returns. Deadlines would leave the loading flags
  set across frames.
- **On / Mute When Stopped fast path**: conflicts with the rule that
  recomputeGraph always calls `moveSynthBefore`.
- **Synthdef-load sleep** (`serverManager.cpp:1769`, preference off by
  default): `/s_new` would race the `/d_loadDir`.
- **`appendOrderedNodes` by reference**: a virtual signature overridden in
  four classes, only run per rebuild.
- **AdditiveBox**: it already recomputes at most once per frame, and
  `computeVoice`, the dominant cost, reads all the swept controls.
- **Lean def variants** (EchoBoxTap, DrumKitBox, PolyComb filters, SampleBox
  8-voice pad, spreaders, GrainBoxPolyOut filter): redesigns that need
  per-mode defs swapped from C++ when settled (as with the morph-only
  variants). They multiply compile time, and a swap mid-note clicks.
- **Smaller sub-items**:
  - Clearing `blockedControls` in `end()`: read after end.
  - Typed compare instead of `save()`: int listeners would stop firing.
  - `sendLfoPart` dirty skip.
  - RhythmBox gate-bus gating.
  - Wavetable and PolyComb `.inl` files not changed.

## Timeline and transport review (2026-11-22, not fixed yet)

Read-only. Two audits: the ofxOceanodeTimeline engine, and transport /
scheduling plus the timeline editor GUI. The items marked ✔ were re-read by
hand. Paths are relative to `addons/`.

### High

1. ✔ **Parameter paths are resolved from strings every frame, and the
   second apply pass has no cache at all.**
   - `beginParameterCache` (`ofxOceanodeTimeline.cpp:5366`) clears the
     cache every frame.
   - `ofxOceanodeContainer.cpp:302` ends the cache before the post-update
     `applyAutomation()` (`:323`). Every automated path then goes through
     `findTimelineParameter` (`ofxOceanodeContainerPaths.cpp:114`):
     `ofSplitString`, `dynamic_cast` walks over the macros, and
     `getEscapedName` scans.
   - A missing target scans every parameter of every node, every frame.
   - Fix: a persistent path → pointer cache invalidated by a container
     graph-generation counter (add, delete, rename, preset or macro load),
     with misses cached and retried on a throttle.
2. ✔ **The whole timeline model is hashed about 4 times per frame**,
   playing or stopped:
   - `syncPlayer` (`Players.cpp:683`);
   - the engine's `ensureEvaluationIndexes` (`:812`) and again in
     `evaluateAutomation`, because `beginParameterCache` resets
     `evaluationHashedThisPass`;
   - `adoptInstrumentDurations` (`ofxOceanodeTimeline.cpp:5461`), before
     its 15-frame gate.

   Each hash covers every note, step and point. It also calls the virtual
   `getTimelineInstrument` per piano lane, and SampleBox's version allocates
   names, colours and waveforms. Fix: an edit revision counter bumped by
   mutators, the editor and history; at minimum, one hash per frame reused
   by the engines.

### Medium

3. ✔ **MIDI clock re-anchors every synced clock every frame.**
   - `MidiClockCore.h:204` EMA-smooths bpm on every tick, and
     `scTransportSync.h:193/265` treats a change above 1e-3 bpm as a tempo
     change. The result is a new anchor plus resent deadlines:
     6-control timed `/n_set` per clock per server.
   - It hits scModulation synced LFOs in every Box, MetroBox, Arp, ChordBox,
     GrainBox and Bake. Tempo-lane ramps hit the same path.
   - Fix: a relative threshold (~0.05 %), letting the periodic drift anchor
     absorb the rest, or quantize the external bpm.
4. **Automation values make a string round trip** (format, combine,
   broadcast, clamp, parse), up to 3 times per engine per frame
   (`Evaluation.cpp:1599`, `:2272`; `ofxOceanodeTimeline.cpp:625`). Fix:
   compute each path's final value once in `evaluateAutomation`; later
   passes apply the stored value.
5. **Scheduler rebuilds string containers every frame while playing**
   (`Scheduler.cpp:183-425`):
   - `selectSchedulablePaths` is O(n²) and takes the registry mutex per
     path;
   - targets map, active/suppressed sets, `scheduledBackendPaths` copy,
     latency map over tracks × bindings.

   Fix: cache by evaluation signature and use interned path ids.
6. **A stopped transport still evaluates everything**: collect plus both
   apply passes at an unchanged beat. Fix: reuse `activeAutomationValues`
   when beat, generation, signature and live overrides are unchanged.
7. **Piano Roll editor draws every note**, about 9 primitives each, with
   no x culling (`PianoRollEditor.cpp:824`, `drawPreview :1480`). Drum Grid
   already culls.
8. **Timeline rows are never culled vertically**
   (`TimelineController.cpp:4155`). Folder counts are O(n²) per frame
   (`:4186`).
9. **LFO Track / Timeline Transport nodes**:
   - each `lfoTrack::update` runs another `syncPlayer` hash;
   - the timeline and track lists are rebuilt every frame;
   - `phaseOut`/`valueOut` are set unguarded (`lfoTrack.cpp:126-127`).
10. **Wave clips**:
    - a missing file is stat'ed per clip per server every frame
      (`timelineWaveTrack.h:327`);
    - the loop at `ofxOceanodeTimeline.cpp:5767` builds strings and does
      lookups for every clip, however far from the playhead.

### Low

- **Clips off-screen to the left are still drawn**, since `drawClip`
  clamps to the scrolling `laneMin.x`. `drawClipMenu` calls `BeginPopup`
  per clip per frame (`:5602`).
- **The MIDI clock builds a new `ofxMidiIn` once a second** to list ports
  (`MidiClock.cpp:92`).
- **`syncPlayerList` and `pruneMacroSequences` work every frame**:
  `syncPlayerList` rebuilds per frame, and `pruneMacroSequences` runs 4+
  times per frame over undo histories.
- **LFO lane evaluation**: about 9 linear name lookups, and a curve-cycle
  cache keyed by lane id that macro copies thrash.
- **Scheduled notes go value → text → value**, and the input vectors are
  copied twice per event.
- **The Drum Grid rebuilds row colours and sounds per frame.**

### Already cheap

- The scheduling registry, transport latch, Follower with a steady tempo,
  `updateTransportGates` and Global Pitch buses.
- Hidden timeline windows, bounded waveform drawing, and Drum Grid x
  culling.
- The cursor-based scheduler window and `upper_bound` curve evaluation.
- The `setBpm` change guards.

### Status after the timeline/transport fix pass (2026-11-22)

Repositories: ofxOceanodeTimeline, ofxOceanode and ofxOceanodeSuperCollider.
Every touched file was syntax-checked with the BLOOBS flags. Tests:

- The ofxOceanodeTimeline suite: 57/60 before and after. The three failures
  were already there: attachment, controller_clarity, unsaved_changes.
- `tools/transport_sync_test`, extended with a clock-jitter case that fails
  on the old code.
- 12 screenshots of folder rows and clips, before vs after: byte-identical.

`tests/perf_benchmark.py` (eight instruments × 400 notes, playing):
`update()` went from about 337 µs to about 264 µs. The machine was loaded, so
the figure is noisy. **Nothing has been run in the app.**

Done:

- **1 Persistent path → parameter cache**, kept across frames and across the
  post-update apply pass.
  - It is cleared when the node graph generation (node constructor and
    destructor) or the canvas-id generation changes.
  - An entry is trusted only while three things hold: the group's lifetime
    token is alive, the parameter sits at the same index, and it has the
    same name (a rename in place re-resolves it).
  - Misses hold for one pass; the apply path keeps its 30-frame retry.
  - New core overload `findTimelineParameter(path, weak_ptr<void>& alive)`.
  - An independent review traced every deletion path and found no
    use-after-free.
- **2 One model hash per frame for the source**, through the
  `sourceSequenceSignature` memo used by syncPlayer, adopt and LFO Track.
  The engine copy is hashed once instead of twice.
- **4 The blend is cached per path** by `activeValuesRevision`. Broadcast
  and clamp still run per pass, because they read the live width and range.
- **5 Scheduler**:
  - a set instead of the O(n²) find;
  - targets as a vector in path order;
  - `scheduledBackendPaths` moved, not copied;
  - a null-parameter guard.
- **3 MIDI clock re-anchoring**: `scTransportSync` compares against the
  tempo of the last sent anchor, with a 0.05 % relative threshold. The
  periodic anchor (every 2 s) absorbs the rest, about 1 ms.
- **MIDI clock port check** uses the open input's own client instead of a new
  CoreMIDI client every second.
- **Wave clips**: a missing file is retried every 2 s, not stat'ed per clip
  per server per frame. The per-track key and `isTrackAudible` are hoisted.
- **LFO Track** reuses the frame's signature and rebuilds its track list only
  on change.
- **LFO cycles cache** is keyed by lane pointer (content-checked).
- **Rests** are built only for heard lanes.
- **Editor drawing**:
  - Piano Roll notes culled by x (canvas and preview);
  - the folder-count index is built once per frame;
  - folder rows and `drawGrid` skip drawing when out of view (interaction
    kept);
  - clips outside the lane's visible area return early, with hit rects still
    recorded;
  - clip menus are drawn only while a popup is open.

Held:

- **Edit revision counter instead of hashing**: the signature also covers
  node-derived voice counts and engine overrides.
- **Stopped-transport reuse**: `collectActiveValues` reads live ranges, solo,
  retrigger gaps and rolls.
- **Scheduler caching by signature**: `hasParameterTarget` has no revision.
- **Vertical culling of whole track rows**: the row body builds drop
  targets, hit rects and editor spans used later in the frame.
- **Drum Grid row cache**: a delayed refresh would show pad changes late, and
  the instrument holds raw pointers.
- **`phaseOut` / `valueOut` guard**: the policy is that read outputs notify
  every frame.
- **Skipping inactive wave clips**: `updateWaveClip` preloads voices.
- **Smaller items**: `syncPlayerList` / `pruneMacroSequences` rebuild
  signals, and the scheduled-note text round trip (crosses into
  `scScheduledInputs`).

Known side effects:

- A parameter that appears without a node being created is still found the
  next pass on the scheduler and GUI paths, but up to 30 frames late on the
  apply path, as before.
- An edit made to a sequence during node updates is seen one frame later.

### Correction (2026-11-22, evening): timeline engine commit reverted

In the app (Lightnet, project GENERATIVE_MUSIC/181--SPECTRAL_TECH_MACRO_TIMELINE),
the timeline engine commit made scsynth log `late` for bundles stamped 1–17 s
in the past, with audible timing errors. The commit is ofxOceanodeTimeline
969fe10 (persistent parameter cache, source signature memo, blend cache,
scheduler changes), together with its core lookup ofxOceanode bc8eb19.

Both are reverted (ofxOceanodeTimeline 77ae0a0, ofxOceanode 54da1ef), and the
benchmark commit with them. Items 1, 2, 4 and 5 above are therefore **not in**.

The other commits of that pass are back in and tested clean in the app:

- editor drawing;
- MIDI clock port listing;
- the scTransportSync relative tempo threshold;
- the wave-clip missing-file throttle.

A simulation of the old and new transport follower, through ramps, jitter,
seeks, loops and stop/start, produces no past anchors.

The timeline suite missed the bug. The root-cause investigation is in
progress; the engine work comes back only with a regression test that fails
on 969fe10.

### Resolution (2026-11-22, night): it was Spotlight; everything restored

The late bundles and the slow server start were a Spotlight indexing storm,
not code:

- `mds` plus a dozen `mdworker_shared`, load average about 30, after a day of
  builds and test runs;
- scsynth sat at 0 % CPU, its NRT thread blocked in `open()` during the
  SynthDef tree load and in `sendto()` during playback;
- so all three servers stalled together, shown by a `/status` probe with
  0.3–3.6 s reply gaps.

With indexing off (`sudo mdutil -a -i off`), the same probe shows no gaps.

Both suspects were restored and tested with that probe:

- **Silent-voice pausing**: STEAMING_STONES, no gaps, worst reply 62 ms.
- **The timeline engine commit**: re-applied as ofxOceanodeTimeline 93b9b30
  and ac17606, with ofxOceanode fe91c08. Tested on SPECTRAL_TECH_MACRO_TIMELINE
  after a clean build: no gaps, no `late` lines.

All of today's work is in. Items 1, 2, 4 and 5 of the timeline pass are back.

For the next time `late` bursts or a slow init appear, check Spotlight and
the load first. Exclude `~/Documents/OF` in System Settings → Spotlight →
Search Privacy.
