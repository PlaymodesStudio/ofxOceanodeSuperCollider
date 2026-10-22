# Shared DSP library conventions

Instrument names belong to presets and Oceanode macros. Shared SynthDefs are
named for one reusable audio/control function, with ordinary parameter names
and explicit units. A conversion is not a reason to create a new instrument
SynthDef. Compose existing sources in macros first.

A new source is justified when an existing source cannot reproduce the
required function. Sample feedback that must run inside a single audio sample
is a valid reason for a small UGen. UI convenience, a particular preset's
parameter grouping, and combinations of LFO/math/routing are macro work.

Keep historical algorithm provenance in comments/docs, not in browser names.
Do not rename existing DSP by rewriting it. Migrate node IDs, saved values,
connections and GUI bindings together; back up the installed state and verify
that the compiled DSP remains unchanged.

## Reusable additions from the ensemble conversions

| Browser name | Function | New native DSP needed? |
|---|---|---|
| LadderMultimode | Four-pole resonant ladder with eight output mixtures | Uses existing OceanodeAnalog UGen |
| ParabolicSaturator | Symmetric parabolic soft saturation, unity small-signal slope | No |
| CompensatedSVF | Frequency/damping-compensated two-pole HP/BP/LP | Per-sample integrators |
| FilteredFeedbackDelay | Four-point delay, filtered sample feedback, parabolic dry/wet | Avoids a server-block feedback delay |
| EventADSR | Explicit retrigger, linear attack/exponential decay, times in ms | Separate event and held-gate semantics |
| CongruentialRandom | Triggered, sample-rate or continuously seeded 32-bit random sequence | Exact integer overflow and seed timing |
| RandomBeatClock | Random integer beat durations or fixed beat clock | No |
| SaturatingCompressor | Linked soft-knee dynamics, auto makeup, polynomial saturation | Per-sample asymmetric dB follower |

RandomBeatClock does not follow host playhead phase. TransportPulse is the
existing choice for a transport-aligned periodic clock. Envelope remains the
existing general envelope; EventADSR provides different retrigger/shape behavior.

The renamed SpaceSystem sources have exactly the same DSP and parameters:
`ReaktorFilter4` → `LadderMultimode`, `ReaktorSaturator` → `ParabolicSaturator`.
Run `tools/migrate_synth_renames.py <app>/bin/data` to migrate saved presets and
reusable macros before removing the old discovery entries. The migration is
idempotent and retains backups. The Organism installer does this automatically.
