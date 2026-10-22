# OceanodePrimitives

Small reusable DSP kernels; no complete instrument or preset lives here.
Build universal macOS extensions with `bash build.sh [SuperCollider source]`.
Install the `.sc` and `.scx` together. Requires no third-party runtime.

- `CompensatedSVF`: input, Hz, resonance 0..1, type HP/BP/LP. Two integrators
  with frequency/damping compensation and FIR input correction.
- `FilteredFeedbackDelay`: input, milliseconds, signed feedback, filter Hz,
  resonance, HP/BP/LP, wet fraction. Four-point interpolation and one-sample
  filtered feedback. Allocates a two-second buffer at creation, never in `next`.
- `EventADSRKernel`: separate retrigger and held gate, attack/decay/release in
  milliseconds, sustain and peak. Linear attack; exponential decay/release
  time constants. On retrigger, starts from the current envelope level.
- `AsymmetricFollower`: rise/fall times in seconds, first-order Euler follower
  with half-time convention. Used for audio-rate dB dynamics.
- `CongruentialRandomKernel`: explicit wrapping 32-bit recurrence
  `state = state * 1103515245 + 12345`, signed bipolar output. Triggered mode
  advances on positive edges; sample mode advances every sample; seeded sample
  mode rewrites the integer seed before every sample. Seed changes and reset
  edges reset state. Continuously connecting an audio seed therefore has a
  different result from seeding once and generating white noise.

The SVF, interpolation, parabolic crossfade, envelope stages and follower were
reconstructed from the local Reaktor graph/DSP. Their public interfaces use
physical units and describe their behavior. Provenance is retained here for
maintenance; the library/browser names are independent of any ensemble.

`tools/organism_microdel/check_random.py BUILD_DIR` checks known integer vectors,
held triggers and continuous seeding in scsynth with block sizes 1 and 64.
