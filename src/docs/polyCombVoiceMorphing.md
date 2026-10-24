# PolyComb snapshot morphs with different voice counts

See the [consolidated reference](polyCombMorphing.md) for the shared contract
and regression runner.

PolyComb keeps its normal rendering path during ordinary playback and when both
snapshots use the same voice count. There are no permanently reserved extra
voices, mixer synths or buses.

For a different-count snapshot morph, it temporarily renders the additional
count configuration and mixes the voice-bank outputs **before** the existing
shared effects. Each bank has its own count-dependent pitch maps, pan positions,
normalization, envelopes and modulation. Both banks receive the interpolated sound
parameters, gate/pitch values, audio cables and tempo. The original voice count
stays fixed in its audio graph while the editor's Voices parameter exposes the
largest contributing count from the first nonzero morph step (including steps
below 0.001). The destination bank starts then and fades in with the morph;
Morph Switch does not delay voice activation until 0.5. Exact endpoints use their
own count. Crossing 0.5 does not rebuild anything.

At an endpoint held for 200 ms, PolyComb either returns to its original bank or
adopts the already-running destination bank. Adoption moves its groups, synths,
buses and modulation server state into the main node. It does not restart the
oscillators, envelopes, LFOs or comb resonators. The main node's shared compressor,
EQ, blur, echo and reverb retain their state throughout.

During promotion, two mixer coefficients briefly read aliases of the same live
output. After another 200 ms, the temporary mixer and buses are removed. Playback
then has exactly the endpoint's normal voice-bank and effect CPU cost.

This uses two voice configurations during a typical count morph, rather than
reserving the maximum 32 voices all the time. Interrupted morphs can temporarily
retain more configurations to preserve their current audio blend. Unused banks
are released after their output has faded. A stored intermediate snapshot keeps
its count distribution in the optional `_voiceCountBlend` field; older snapshots
without a voice count retain the existing patch-count fallback.

Realtime startup waits for the mixer's and destination bank's node acknowledgments
before fading into them. Capture-only NRT uses the configured startup delay, since
it has no server acknowledgments. A connected Voices control retains precedence.
Channel-count changes keep their existing structural rebuild behavior.

MacroMixer now enters this same sound-morph path and can change Voices without
replacing the PolyComb node. See [the integration notes](macroMixerSoundMorphing.md)
for control ownership, cancellation and validation.

## Files

- `scVoiceCountMorph.h`: shared count-distribution interpolation and capture.
- `scPolyCombVoiceMorph.inl`: temporary banks, routing, adoption and cleanup.
- `scModulation::adoptServerState`: move running server state while retaining the
  owning node's frontend parameter bindings.
- `PCVoiceMorph<C>` in `polyCombExtras.scd`: temporary smoothed audio mixer.

Rebuild the application and reload/recompile PolyComb's SynthDefs to use this.

## Checks

- `python3 tools/polycomb/voice_morph_test.py`: production manager, sanitizer
  checks, no idle/same-count allocations, both endpoint cleanups, midpoint,
  interruption, delayed startup and retained node identities.
- `python3 tools/polycomb/voice_morph_modulation_test.py`: production modulation
  transfer; local frontend bindings and exactly-once ownership cleanup.
- `python3 tools/polycomb/voice_morph_render_test.py`: real DSP at 1, 2 and 16
  channels; continuity through morphing, promotion and mixer removal, plus shared
  effect-tail survival.
- Existing `note_traffic_test.py` and `voice_rebuild_test.py` cover normal note
  sends, vector widths, pending creation and ordinary structural rebuilds.
- `moving_saws_test.py --skip-dsp-hashes` checks existing snapshot recall against
  the source independently of stale local compiled-binary cache hashes. The
  default test still includes its separate historical binary audit.
