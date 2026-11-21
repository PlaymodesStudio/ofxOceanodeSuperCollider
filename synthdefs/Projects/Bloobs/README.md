# BLOOBS project SynthDefs

Synths made for BLOOBS. Like every `Projects/` folder, an app gets them from
"Sync with SynthDefs Git" only when it ticks the **Bloobs** project (kept in
its `data/Supercollider/Config/SynthdefProjects.json`). Moved here from
`Optimized/Bloobs/` on 2026-11-21.

## 78-channel models

These metadata-backed SynthDefs reproduce the old BLOOBS 78-channel chain
without the extra audio-rate parameter selection and unused noise types in
the generic new SynthDefs.

| Model | Replaces | Preserved behavior |
| --- | --- | --- |
| `BloobsNoise` | old `noise` | White, Pink, Crackle, Dust in the original order; original density scaling |
| `BloobsBandpass` | old `bandpass78` | Two cascaded BPFs; control-rate pitch and Q |
| `BloobsStereoPan` | old `stereopan78` | Signed -1..1 Pan2 per channel, mixed to stereo |

Only `N Chan = 78` is compiled for BLOOBS. The models are also used by
`MICROBUBBLES_THALASTASI` and `LLUERNIA_SONIFICATOR_2/Macro_12`: an app that
loads those macros must tick the Bloobs project to keep getting their sources
(builds it already has stay installed either way).

The compiled 78-channel graphs contain 711, 244, and 137 UGens respectively.
The old reference binaries contain 711, 243, and 136. In a 30-second offline
render of the full chain, the optimized version took about 4.2 seconds, the
old version 4.2 seconds, and the generic new chain 6.0 seconds on this Mac.
The old reference binaries came from OceanodeScroller because BLOOBS' old
binary folder was removed.

## SONIF_ENGINES CPU models (2026-11-21)

Drop-in replacements used by `BITXOS_BODY_SONIF` and `BITXOS_BUBBLES_BIRTH`,
with the same controls as the generic synth they replace (minus the ones
noted). Costs are % of one core per instance, NRT at 48 kHz on the studio Mac.

| Model | Replaces | Dropped controls | Before | After |
| --- | --- | --- | --- | --- |
| `BloobsSine` (8 ch) | `SinOs` | none (no 4x oversampling) | 4.0 | 0.06 |
| `BloobsLowPass` / `BloobsHiPass` (8 ch) | `Filter4Pole` Type LowPass / HighPass | Type, Fm In; Cutp and Q at control rate | 0.73 | 0.09 |
| `BloobsDust` (78 ch) | `BloobsNoise` Type Dust | Type | 3.5 | 0.8 |

Compiled with the metadata SynthDef plus 1, 2 and 8 channels (78 for
`BloobsDust`); other counts compile on demand.
