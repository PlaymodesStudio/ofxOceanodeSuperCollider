# GrainStretch reference

The user-supplied `grain_stretch.ens` (2008) was opened in Reaktor 6 and
saved to `/tmp/grain_stretch_inspection.ens` to inspect its old serialization.
The original file was not modified. The repository's Reaktor Patch Extractor
resolved all 74 connections, 58 modules and eight instrument snapshots in the
inspection copy. `grain_stretch_reference.json` records the source hash,
recovered controls and mapping; the ensemble itself is not redistributed.

The RAMPS macro contains three controls, not three envelope generators:
SpeedA, GrainA and PitchA. Go starts/resets a Ramp Oscillator whose amplitude
is BeatsBack / beats-per-second and whose frequency is SpeedA divided by
that duration. Its output controls the two Grain Cloud Delay modules' delay,
independently of their Pitch inputs. A clock resets it every BeatsBack beats.
Thus delay slope = SpeedA and source scan speed = 1 - SpeedA. Pitch does not
follow that scan speed. Grain interval = GrainA / beats-per-second; grain
length = interval + D milliseconds. These are the adapted presets' equations.

`GrainStretch 01` through `08` retain source order and distinguish duplicate
names. Go is deliberately not stored as part of a sound; click Go after
selecting a preset. Existing tape presets retain Motion Mode 0, including
when loading a saved sound from before Motion Mode existed. A wrapped delay
window automatically grows an undersized recording; allow it to fill before
using long rewinds. Windows remain limited by the engine's recording range.

The adaptation preserves source channel identity. The reference wires its
input channels to the opposite output cloud. Source placement intentionally
retains GrainFxBox's existing 1:1 routing instead. Pan jitter is available
through panned placement; Source placement keeps channels independent.

These are close adaptations, not sample-identical Reaktor emulation:

- GrainBuf and Reaktor use different grain interpolation and envelope shapes.
  The custom envelope maps attack (clamped to 0..1) and the tail's fraction of
  grain length at 120 BPM. Attack + release are normalized by GrainFxBox.
- Grain interval and the added tail follow tempo correctly; the custom
  envelope's fractional release stays at its reference-tempo setting.
- Reaktor's randomized grain interval (Jitter) is recorded in the reference
  data but is not emulated. In particular, the irregularity of raro1 and the
  second SlowDown2 is approximate.
- Fast-forward scans wrap into older audio within the delay window rather
  than attempting to read input that has not arrived.

`python3 tools/grainfxbox/render_test.py` compiles the production SynthDefs
and renders them offline. Tests measure independent pitch and scan speed,
audible zero-speed freeze, retained tape behavior, all eight saw-delay
trajectories, periodic reset, and a new Go after Stop.
