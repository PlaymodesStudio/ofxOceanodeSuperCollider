# SampleBox

`samplebox.scd` builds `SampleBox_<sample channels>_<output channels>_<mode>`
SynthDefs: one or two sample channels, 1–16 output channels, and resampling
(`R`) or Warp1 time stretching (`T`). Run it in SuperCollider once after
changing the source (about 3 minutes). The generated `CompiledSynthdefs/SampleBox/`
folder is ignored by Git, like other generated SynthDefs in this addon.

The node finds that compiled folder beside the source during development. For
an app distribution, copy it under `bin/data/Supercollider/Synthdefs/`.

## Playing

The node has a `Trigger/Gate` Oceanode parameter and a `Trig/Gate Audio`
SuperCollider audio input. A rising edge at either starts a note. Pitch 60
plays at the file's original pitch.

- **One-shot** (Loop off, Gate Mode off): plays the In..Out region once. The
  read position stops at the end (it never wraps back to the start while the
  release fades) and the voice fades out over the region's last 4 ms, so the
  end never clicks.
- **Loop**: repeats the region until Stop (or the gate falls in Gate Mode).
  Loop Crossfade (0–50 % of the region, default 0 so rhythmic loops stay
  tight) fades the region's tail into its head.
- **Gate Mode**: a note lasts while the gate is high.
- **Polyphonic**: notes rotate over up to 8 voices. Off (mono): two voices take
  turns, and a new note fades the previous one out in 5 ms (no click).
- **Reverse** plays the region backwards.
- **Transpose** (semitones) is live, unlike Pitch (taken when a note starts):
  it moves notes already sounding. In Stretch mode it changes only the grains'
  pitch, never the timeline's speed; in Resample mode speed and pitch together.

## Envelopes

Up to 8 ADSR envelopes (`env<n>target/amount/attack/decay/sustain/release`),
held while their note plays. Targets 1–6 and 28–33 are per voice (each note
its own envelope); the others (filter, EQ, dynamics, space) share one
envelope set held while any note plays. `targetRanges` gives what Amount 1
means for each target; the node's target list (scSampleBox.cpp, kTargets)
must stay in step with it.

## Effects

Voices -> two-pole filter (LP / HP / BP) -> GrainBox's five-band EQ ->
compressor -> saturator -> GrainBox's SpaceMaster reverb -> MixerPro's
generalized mid/side stereo image (Width, Diffuse, Bass Mono) -> `out`.

## Window feedback

`vis` is a control bus of 8 channels: each voice's position in the file
(0..1), -1 when silent. The node draws the playheads and voice lights from it.
