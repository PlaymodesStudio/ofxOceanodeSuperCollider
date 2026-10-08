# SynthDef sources

This folder is the source of truth for every SynthDef used by
ofxOceanodeSuperCollider. It replaces `oceanodeSynthdefsNew/NEWEST`, which is
kept only as an archive.

Compiled `.scsyndef` files are not stored here: they live in the app's
`bin/data/Supercollider/Synthdefs/`. Running a script writes into a local
`CompiledSynthdefs/` folder, which is gitignored.

## Layout

| Folder | Contents |
|---|---|
| `Oceanode/` | Shared prelude: `mainDefinitions.scd` (`~synthCreator`, `~useExactSynthCreator`) and the Oceanode pseudo-UGen classes (`oceanodeParameter.sc`, `ugenExtensions.sc`), which must also be in the SuperCollider extensions folder. |
| `Defaults/` | SynthDefs loaded by the server itself or by nodes with a C++ counterpart. One folder per node, plus `Core/` (output, info, buffer allocation) and `Analysis/` (meters, scopes, trackers). |
| `Effect/` `InOut/` `Mixing/` `Modulation/` `Routing/` `Source/` `Spatial/` `Utilities/` `Compatibility/` `Optimized/` | Generic `~synthCreator` synths, mirrored by the same folders under the app's `Synthdefs/`. |
| `WIP/` | Experiments not yet in Oceanode, run from the SuperCollider IDE (see `WIP/README.md`). Not compiled on demand, not deployed. |

## Generic synths (`~synthCreator`)

Each file calls `~synthCreator.value("Name", {|n, vars| ... }, category: ...)`,
where `n` is the channel count. Builds are named `Name<n>.scsyndef`, or
`Name<n>_<v1>_<v2>.scsyndef` for synths that declare variables, and the
unnumbered `Name.scsyndef` + `Name.txarcmeta` carry the node metadata.

To build a single channel count headlessly:

```supercollider
"Oceanode/mainDefinitions.scd".load;   // defines d and the creators
d = "/path/to/output";
~useExactSynthCreator.(23);           // or .(78, [6]) for variables
"Effect/Filter/LPF.scd".load;          // writes LPF/LPF23.scsyndef
```

Oceanode does exactly this when a node asks for a channel count with no
`.scsyndef`: after the user confirms, `scSynthdefCompiler` runs the bundled
`sclang` (see `../sclang/`) on `Oceanode/compileExact.scd` and installs the
result next to the synth's other builds. For an app to do it outside this
checkout, deploy with `tools/deploy_synthdef_compiler.sh <app>/bin/data`.

Every file in these folders must load from a script, not only block by block
in the IDE: separate top-level `( ... )` blocks with `;`.

A file whose first line is `// @oceanode-no-autocompile` must not be rebuilt
for new channel counts (currently only `Source/PhysicalModelling/oteypiano.scd`,
whose shipped binaries came from a lost source).

## Known issues in Defaults (pre-existing)

- `Core/output_stereomix.scd`, `VST/vst.scd`: multi-block, IDE-only.
- `Core/mergebuf.scd`, `RadioStation/radioStation.scd`: runtime errors when run
  headlessly.
