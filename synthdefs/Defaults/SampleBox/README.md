# SampleBox

SampleBox is a kit of 1–16 sampler pads. `samplebox.scd` builds, per pad:

| SynthDef | What |
|---|---|
| `SampleBoxPad_<sample channels>_<output channels>_<R\|T>` | the sampler: voices, amp envelopes, filter; resampling (`R`) or Warp1 time stretching (`T`) |
| `SampleBoxEQ_N`, `SampleBoxComp_N`, `SampleBoxSat_N`, `SampleBoxEcho_N`, `SampleBoxReverb_N`, `SampleBoxImage_N` (N > 1) | the mixer's effects, each replacing the pad's bus in place |
| `SampleBoxStrip_N` | Level, mute: into the node's Out (summed) and the pad's own output; its peak for the meter |

N is the output channel count (1–16). Run the file in SuperCollider after
changing it (a few minutes). The generated `CompiledSynthdefs/SampleBox/` folder
is ignored by Git, like other generated SynthDefs in this addon. The node finds
it beside the source during development; for an app distribution, copy it under
`bin/data/Supercollider/Synthdefs/`.

## Graph

The node is a group with one group per pad, in pad order. A pad's group holds
its sampler, its modulation's sources and routes (synthdefs/Defaults/Modulation),
an effects group, and its strip. An effect exists only while it is switched on,
so an effect that is off costs no CPU; one switched on goes before the next one
that exists, keeping the order EQ → Comp → Sat → Echo → Reverb → Stereo.

## Playing

Every pad reads one channel of the node's `Trig/Gate Audio` input
(`\trigbus` = the input's bus + the pad's channel), so a MetroBox or EuclideanBox
`Gates` output plays pad n from lane / track n. The Oceanode `Trigger/Gate`,
`Pitch` and `Velocity` values are per channel too. A rising edge starts a note;
a gate is a level (0.2 plays at 0.2). Pitch 60 plays the file at its own pitch.

- **One-shot** (Loop off, Gate Mode off): plays the In..Out region once. The
  read position stops at the end and the voice fades out over the region's last
  4 ms, so the end never clicks.
- **Loop**: repeats the region until Stop (or the gate falls in Gate Mode).
  Loop Crossfade (0–50 % of the region) fades the region's tail into its head.
- **Gate Mode**: a note lasts while the gate is high.
- **Polyphonic**: notes rotate over up to 8 voices. Mono: two voices take turns,
  and a new note fades the previous one out in 5 ms.
- **Reverse** plays the region backwards.
- **Transpose** is live: it moves notes already sounding.
- **Choke groups**: each pad writes its triggers to its slot of `\chokeblock`
  (a control bus of 16); `\chokemask` (16 values, from the node) says which
  pads' triggers cut this one's notes (5 ms).

## Modulation

Each pad has its own shared modulation (scModulation): `\modbus + k * 16`, 8
channels per target k, one per voice. The effects follow the latest note's
modulation (its voice is on `\latestbus`): Drive, Comp Threshold, Reverb Mix,
Width. `targetRanges` maps the sampler's old target ids to the node's targets;
the node's target list (scSampleBox.cpp, setupPad) must stay in step with it.

## Window feedback

`vis` is a control bus of 8 channels: each voice's position in the file (0..1),
-1 when silent. `meterbus` (1 channel) is the strip's peak.
