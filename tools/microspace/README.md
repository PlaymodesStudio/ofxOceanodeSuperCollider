# MicroSpace and Box reverbs

`microSpace.scd` and all Box reverbs share `OceanodeMicroSpace`, in
`synthdefs/Oceanode/oceanodeMicroSpace.sc`. The standalone MicroSpace keeps
its native six controls and original DSP behavior.

AnalogBox, AliasingBox, FMBox, SteamPipeBox, SlicerBox, EchoBox, PhysicalBox,
GrainBox and AdditiveBox use `boxMicroSpace<N>` (compiled by
`Defaults/MixerPro/mixerpro.scd`). SampleBox uses the same adapter inside its
in-place `SampleBoxReverb_N`, preserving its latest-voice Reverb Mix modulation.
PhysicalBox's additional Space stage and its WIP prototype also use the core.
Boxes with no reverb stage need no change. MixerPro's own SpaceMaster stays as-is.

Existing parameter names and preset data remain usable. The adapter maps Size
0–60 to MicroSpace 0–100, retains the separate Decay RT60 override and the
external Predelay/Spread/Lowpass controls, maps damping to MicroSpace's negative
Lo/Hi shelves (limited to 12 dB), and maps Mod Rate × (Spin + Wander) × 2 to
MicroSpace Mod (limited to 0–1). Position and Mix use MicroSpace's parabolic
crossfades. Bypass gates excitation and restores dry while the tail rings out.
GrainBox and AdditiveBox now send scalar reverb controls; EQ/Echo remain vectors.

PhysicalBox Space maps Time 0–1.2 to Size 0–100, Diffusion to Mod, Damp and Low
Cut to negative Hi and Lo shelves. Freeze stops excitation, removes feedback
shelf damping, and sets tank feedback gain to unity. Speaker pairs process
independently; an odd final channel uses its own pair and keeps the left output.

Recompile MixerPro, SampleBox, PhysicalBox and MicroSpace after installing the
updated Oceanode class library, then reload SynthDefs in the application.

Validation:

```sh
python3 tools/microspace/model_test.py
python3 tools/microspace/box_test.py
```
