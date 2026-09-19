# SteamPipe dry reconstruction for SuperCollider/Oceanode

This implementation recreates the signal path `Steam -> Pipe -> Amp` from
`SteamPipe_santi.ens`. The complete `R66` macro is omitted, so there is no
reverb, pre-delay, or post-delay in the output.

## Why the resonator uses DynGen

The Reaktor graph does not contain a simple linear comb. Its delayed signal is
processed by an optional diffuser, asymmetric saturation, high-pass/low-pass
filtering, envelope-dependent push-pull interaction, and polarity before it is
written back into the tuned delay. `LocalIn`/`LocalOut` would add one server
block to that loop and alter pitch and stability. `SteamPipe.eel2` instead runs
the complete loop in DynGen's `@sample` section.

The surrounding `SteamPipeN` SynthDefs expose ordinary vector-addressable
Oceanode controls. One output channel is produced per voice. Stereo placement,
reverb, and delay can therefore be added as normal Oceanode nodes afterwards.

## Files

- `example/bin/data/Supercollider/DynGenDefs/SteamPipe.eel2`: sample-by-sample
  physical model.
- `example/bin/data/Supercollider/DynGenDefs/SteamPipe.json`: server-side DynGen
  registration manifest.
- `tools/SteamPipe/SteamPipe.scd`: SynthDef generator.
- `example/bin/data/Supercollider/Synthdefs/PhysicalModeling/SteamPipe/`:
  generated `SteamPipe1` through `SteamPipe128` definitions and metadata.

The server manager now loads `Supercollider/DynGenDefs/*.json` on every server
boot. This is required because a compiled SynthDef stores the DynGen hash but
not the EEL2 source itself.

## Regeneration

```bash
/Applications/SuperCollider.app/Contents/MacOS/sclang \
  tools/SteamPipe/SteamPipe.scd
```

DynGen and the Oceanode SuperCollider classes must be installed in the
SuperCollider extensions folder. The generated node appears under
`SuperCollider/Source/PhysicalModeling` as **SteamPipe**.

The default controls reproduce the `Pizz`-style values documented during the
binary extraction. Pitch units are MIDI semitones, envelope time controls use
Reaktor's logarithmic mapping (`0 = 1 ms`, `20 = 10 ms`, `40 = 100 ms`), and
filter cutoff controls use MIDI pitch units.
