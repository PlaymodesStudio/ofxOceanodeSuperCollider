# Organism / Microdelay conversion

The target is the ensemble's global opening state, including its modified
MicroDelay state. Snapshot banks are not imported. The source hash and recovered
controls are in `opening-state.json`.

Each engine is an Oceanode macro: events, voice, delay motion, feedback delay,
and stereo/dynamics. The live GUI exposes their relevant performance controls.
Instrument names remain in these macros. Shared SynthDefs describe reusable DSP
functions; existing routing, math, conversion and modulation sources are composed
inside the macros rather than collected in a complete instrument SynthDef.

## Reference checks and important corrections

- Reaktor's live Instrument properties show **one voice**, with minimum and
  maximum unison voices both one. The earlier 18-voice build was wrong; 18 was
  not a verified polyphony value.
- The selected source is labelled noise, but the Core generator receives an
  **audio-rate pitch seed**. This continually overwrites its integer state.
  Substituting free-running white noise or pitch-clocked sample-and-hold noise
  changes the sound substantially. `CongruentialRandom` reproduces the integer
  operation with an explicit continuously seeded mode.
- Pitch, cutoff, pan and width random cells have **bipolar** outputs. Several
  contain an Abs module that is not connected to the output. Attack and decay
  modulation do use the Abs output.
- The cascade was checked visually in Reaktor: positive cutoff modulation
  sweeps the HP with the LP at pitch 120; negative modulation holds the HP at
  -60 and sweeps the LP using the absolute value.
- Core pin property 9 is not a safe substitute for the cached external Primary
  port order. The actual input wiring was checked in Reaktor. Do not remap root
  inputs purely by sorting this property.
- The stereo stage uses a parabolic pan law, unity at centre, and delays either
  side according to signed width modulation. Equal-power Balance is different.

Reference captures were made with Reaktor's recorder at 48 kHz: full opening
state, delay/compressor bypassed, and a temporary filter bypass. Routing and
controls were restored afterward without saving the source ensemble. Captures
and comparison renders are working files under `/tmp/organism-*`.

The corrected 90-second render is finite. Its stereo RMS is approximately
0.090/0.107, compared with 0.093/0.093 in the opening-state reference. Most
energy is below 100 Hz in both. These are statistical comparisons of different
random performances, not a claim of sample-identical reproduction. The earlier
white-noise candidate was dominated by high-frequency energy instead.

## Remaining fidelity limits

The stochastic beat clock matches the main clock's two events per cycle and
nonzero random integer periods. It does not reproduce Reaktor's random-number
state, secondary event-clock scheduling, or sample-identical event ordering.
The delay-motion macro reproduces the saved tempo relay selection (GO value 1
divided by beat length), the two random updates per clock cycle, and the seeded
absolute random sequence. HOST SYNC exposes the other relay input. Its short
portamento event graph is represented by the final time smoothing.
Reaktor's audio-to-event control sampling is represented by continuous audio
modulation. These differences still affect individual gestures and delay motion.

## Build, check and install

```sh
bash ugens/OceanodePrimitives/build.sh
python3 tools/organism_microdel/prepare.py /tmp/organism-build
python3 tools/organism_microdel/check_random.py /tmp/organism-build
python3 tools/organism_microdel/render_preset.py \
  /tmp/organism-build/data/Presets/DEBUG/23--ORGANISM_MICRODEL \
  /tmp/organism-build/defs /tmp/organism-build/preview.wav 90 48000
python3 tools/organism_microdel/install.py \
  /tmp/organism-build /tmp/geiger-generic /path/to/OceanAudio/bin/data
```

Installation backs up replaced files, migrates SpaceSystem names in place, and
retires obsolete discovery entries. It does not regenerate SpaceSystem settings.
Install `.sc` and `.scx` together and restart the SuperCollider server after an
extension update. Existing servers retain the previously loaded native code.

SpaceSystem's `LadderMultimode` and `ParabolicSaturator` renames were checked
against the original compiled definitions and deterministic quad audio renders:
the DSP payloads match, and the maximum sample difference was zero.
