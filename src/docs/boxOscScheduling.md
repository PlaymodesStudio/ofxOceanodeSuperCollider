# Box OSC and note scheduling audit

Piano-roll note boundaries are ordered by the timeline's routed roles: Pitch is
prepared 2 ms early, Velocity 1 ms early, and Gate retains the musical boundary.
Each instrument must preserve that ordering when it turns parameter events into
OSC. Merely dispatching Pitch first is insufficient if another listener resends
Gate or overwrites the prepared Pitch.

## Rules used by the implementations

- A sound-control edit must not resend Gate, note trigger counters, or note-off
  state. Full note/routing resends remain explicit for creation, reconnection,
  mode changes, and non-realtime capture.
- Honor backend suppression when the GUI catches up with an already scheduled
  event. Send note events immediately within their existing scoped timetag;
  coalesce ordinary sound controls rather than deferring notes.
- Group related controls with `scSynthControlBatch`. Batches have a 4096-byte soft
  budget and clear their payload after a flush. A single vector remains intact,
  even when it exceeds the soft budget. Existing per-voice value caches remain
  authoritative; optional batch caches require invalidation on recreation and
  an explicit capture/state resend.
- Use `scControlBusBatch` for disjoint `/c_setn` ranges. Keep gate bus writes out
  of modulation-matrix refreshes after initialization.

## Coverage

| Model | Audit result / changes |
| --- | --- |
| FMBox | Independent pitch/gate packets; sound/FX changes coalesced per update; unchanged auxiliary controls cached; note/routing and NRT resends explicit. |
| AliasingBox | Removed note resends from sound automation; independent pitch/gate packets; cached voice changes and spreader controls batched; FX changes coalesced. |
| PhysicalBox | Same separation, retaining pulse routing, Follow Gate, voice creation, and explicit NRT resends. Voice changes now use batches. |
| SteamPipeBox | Retained its existing cached voice batches; separated remaining note/sound paths and coalesced spreader/FX traffic. Pulse enable controls share one batch. |
| AdditiveBox | Retained partial/curve caches and dirty computation. Existing gates are sent by gate events, routing/drone transitions or forced resends, not sound/pitch refreshes. Changed per-synth controls use bounded batches. |
| GrainBox | Pitch, region and full-restore controls batched. Modulation header/voice ranges share bounded bus messages; modulation edits no longer rewrite an initialized gate range. Creation and full restore still initialize gates. |
| AnalogBox | Allocator pitch preparation no longer resends Gate. Note events send only their input; pitch/oscillator/filter/amp/spreader control groups use batches. |
| PolyComb | Retained existing independent `notesOnly` handlers, scoped input reads, suppression, and voice/morph lifecycle safeguards. |
| PulsarBox | Removed Gate from ordinary sound flushes and pitch events; explicit gate delivery retained. Changed bank controls use bounded batches and output routing participates in the cache. |
| SampleBox | Envelope/filter/modulation edits exclude note inputs and fire/stop counters. Pitch-only updates do not resend Gate. Engine, strip and effect groups use batches; initialization/replacement/NRT remain complete. |
| WavetableBox | Added scheduled input registration tied to publication and teardown, suppression, scoped current-note reads and a separate gate sender. Warp/morph sound flushes no longer resend Gate. |
| EchoBox | Tap, mix and reverb settings batched; wet-route acknowledgement/tail behavior retained. |
| SlicerBox | Sequencer configuration, amp, EQ and insert controls batched; separate gate/manual-reset handling retained. |
| SpectralFilterBox | Full control groups batched and unchanged scalar sends skipped; existing mask/transpose/gain caches retained. |
| ArpeggiatorBox, ChordBox, MetroBox | Static clock/voice configuration grouped; existing bounded buffer uploads and timestamped transport anchors retained. |
| EuclideanBox | Existing batched control/anchor path retained; no external instrument pitch/gate pair. |
| RhythmBox | Typed track configuration batched; server-side clocks and step/trigger delivery retained. |
| DataBox | Existing targeted per-control/per-voice updates retained; no instrument pitch/gate sequencing path to reorder. |

## Verification

Run from the addon root:

```sh
python3 tools/box_osc_test/run.py
python3 tools/box_osc_test/syntax.py
python3 tools/box_osc_test/native_regression.py
python3 tools/box_osc_test/bus_render_test.py
python3 tools/additivebox/drone_gate_test.py
python3 tools/audio_routing_test/box_fx_startup_test.py
python3 tools/keyboard_piano_test/run.py
python3 tools/transport_sync_test/run.py
python3 tools/steampipebox/retrigger_test.py
python3 tools/steampipebox/panning_test.py
python3 tools/additivebox/morph_test.py
python3 tools/pulsarbox/morph_test.py
python3 tools/wavetablebox/render_test.py
```

The packet contract tests use the production send methods with recording synth
adapters and sanitizers. The native regression uses real Box nodes and OF
parameters with captured OSC and simulated `/n_go` acknowledgements; it does not
boot a realtime audio server. The bus-range and SteamPipe retrigger tests render
through scsynth without a sound device. A native syntax pass covered every Box
translation unit and the combined headers.

These checks establish message contents, ordering/isolation, lifecycle behavior,
and the tested offline audio cases. Realtime `late` reports and audio underflows
also depend on host frame stalls, synth CPU use and the active audio device;
absence of those reports needs verification in the rebuilt application under
its actual workload.
