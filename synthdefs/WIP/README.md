# WIP: experiments

Ongoing SuperCollider experiments that are not part of Oceanode yet: no node,
no `~synthCreator` builds, nothing deployed to apps. They run from the
SuperCollider IDE on their own. When one becomes a node, move it to its
category (or to `Defaults/<Node>/` if it gets a C++ counterpart).

The on-demand compiler ignores this folder, and
`tools/deploy_synthdef_compiler.sh` does not copy it into apps.

## PhysicalBox/

Phase 0 of the PhysicalBox node: a physical-modelling voice written in EEL2
for DynGen. It has strike, bow, blow, scrape and external exciters, a
waveguide and a modal bank (coupled), rattle, tension and cascade
nonlinearities, and two pickups. `physicalbox.scd` is the playable prototype
(window, presets, morph, keyboard, MIDI). See `PhysicalBox/README.md`; the
offline test matrix is in `tools/physicalbox/`.

## Voice/

Two speech synthesizers, moved here from `ofxOceanodeTextures/examples`.

| File | What it is |
|---|---|
| `DECtalkKlatt.scd` | DECtalk-inspired text-to-speech: five-formant voice, parallel consonant noise, nasal resonance, stop closures, coarticulation and pitch contours. English pronunciations come from `cmudict.dict`. |
| `VotraxSC01A.scd` | Votrax SC-01A-inspired phoneme synthesizer: pulse-and-noise excitation, four formant filters, frication, aspiration and plosive bursts, with formants gliding between phonemes. |
| `cmudict.dict`, `cmudict-LICENSE` | The CMU Pronouncing Dictionary (BSD-style licence, kept with it), read by `DECtalkKlatt.scd` from its own folder. |

To try one: open it in the SuperCollider IDE, evaluate the first parenthesized
block (it boots the server and loads the model), then evaluate the examples at
the bottom of the file one at a time.

## RawData/

`rawDataAudio.scd`: the first experiment playing any file's bytes as audio
(read in sclang as int8, uint8 or int16 and played with `PlayBuf`). It became
the **SC DataBox** node (`src/nodes/scDataBox.*`, synth in
`Defaults/DataBox/databox.scd`); kept here as the original sketch.

## Core/

`outputVariants.scd`: earlier and alternative versions of the output SynthDef
(plain `~maxVoices` channels, 13 channels, circular stereo mix of 100). The one
in use is `Defaults/Core/output.scd`.

## CustomBuffer/

`custombufferplayer.scd`: a ping-pong wavetable player (`customBufferPlayer`)
that `.add`s itself to a running server. No node uses it.

