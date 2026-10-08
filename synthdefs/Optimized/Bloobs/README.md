# BLOOBS 78-channel SynthDefs

These metadata-backed SynthDefs reproduce the old BLOOBS 78-channel chain
without the extra audio-rate parameter selection and unused noise types in
the generic new SynthDefs.

| Model | Replaces | Preserved behavior |
| --- | --- | --- |
| `BloobsNoise` | old `noise` | White, Pink, Crackle, Dust in the original order; original density scaling |
| `BloobsBandpass` | old `bandpass78` | Two cascaded BPFs; control-rate pitch and Q |
| `BloobsStereoPan` | old `stereopan78` | Signed -1..1 Pan2 per channel, mixed to stereo |

Only `N Chan = 78` is compiled for BLOOBS. The models are used by
`MICROBUBBLES_THALASTASI` and `LLUERNIA_SONIFICATOR_2/Macro_12`.

The compiled 78-channel graphs contain 711, 244, and 137 UGens respectively.
The old reference binaries contain 711, 243, and 136. In a 30-second offline
render of the full chain, the optimized version took about 4.2 seconds, the
old version 4.2 seconds, and the generic new chain 6.0 seconds on this Mac.
The old reference binaries came from OceanodeScroller because BLOOBS' old
binary folder was removed.
