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
| `Defaults/` | SynthDefs loaded by the server itself or by nodes with a C++ counterpart. One folder per node, plus `Core/` (output, info, buffer allocation), `Analysis/` (meters, scopes, trackers) and `Recorder/`. Every SynthDef a C++ node asks for by name must be built by a script here. |
| `Effect/` `InOut/` `Mixing/` `Modulation/` `Routing/` `Source/` `Spatial/` `Utilities/` `Compatibility/` `Optimized/` | Generic `~synthCreator` synths, mirrored by the same folders under the app's `Synthdefs/`. |
| `OceanodeCode/` | Used by the SC Code node: `captureCode.scd` runs pasted code without a server and describes every SynthDef it builds (`classes/OceanodeCodeSandbox.sc` keeps it away from any real server; compiled only into those runs), `promote.scd` builds a promoted `~synthCreator` source, `examples/` are the node's bundled snippets. The node also runs any `~synthCreator` source here (its own `~synthCreator` builds one SynthDef for the node's N Chan and variables), and Promote writes a refined source back over its original file. |
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

## Syncing an app ("Sync with SynthDefs Git")

The SuperCollider menu's **Sync with SynthDefs Git...** (`src/scSynthdefSync.*`)
brings an app's `data/Supercollider/` up to date with a branch of this
repository on GitHub, or with this checkout. No git or GitHub account is needed.
It downloads what changed of `synthdefs/` (into `SynthdefSources/`), `sclang/`
and our own UGens in `ugens/` (into `Extensions/`), plus the third-party UGens
published with `tools/publish_sc_extensions.py`. Then it compiles, with the
bundled sclang and `Oceanode/syncCompile.scd`, only what the app does not
have: sources it downloaded (new or changed) and synths whose SynthDefs are
missing. "Compile everything again" rebuilds all of them:

- `~synthCreator` synths: the metadata SynthDef plus the stereo build. When a
  synth changed, its other installed builds are retired; any channel count is
  compiled on demand when a node asks for it.
- `Defaults/` scripts: run whole. A `~synthCreator` source under `Defaults/`
  (FM7) builds every channel count of `mainDefinitions.scd`.

Defaults are installed in the app's `Synthdefs/Defaults/<source folder>/`; any
other copy of the same file, and anything under `Synthdefs/Defaults/` that no
source builds, moves to `data/Supercollider/_retired_synthdefs/`. What it did is
recorded in `data/Supercollider/Config/SynthdefSync.json`.

The same sync installs the shared presets and snapshot banks of `../presets/`
(see `presets/README.md`).

A synth that is removed or renamed goes in `Oceanode/retiredSynthdefs.txt`:
the sync then retires its installed builds too. Presets using a renamed synth
are migrated with a script (`tools/migrate_synth_renames.py` for the renames
of 2026-10-20: Pulsar2, CircularBufferFFT2, RLPF4, RHPF4).

So a Defaults script must run on its own: choose its output folder with the
`nowExecutingPath` header (`d = <script folder>/CompiledSynthdefs`) and write
into a subfolder of `d`; a single `( var ... )` block has no `;` after it.

## Rebuilding default SynthDefs

The simplest way is the sync above, from "This addon checkout". By hand: run
the script in SuperCollider; it writes `.scsyndef` files under a nearby
`CompiledSynthdefs/` folder. Copy the resulting SynthDef folders into the app's
`bin/data/Supercollider/Synthdefs/Defaults/` and reload the server. In particular,
changes to `Recorder/recbuffhold.scd`, `Recorder/multirecbuf.scd`,
`Core/bufalloc.scd`, and `Analysis/beattracker.scd` require new binaries; C++
changes alone do not update the server's loaded definitions.

## Rebuilt sources

`Analysis/pitchtracker.scd`, `Analysis/bufferscope.scd` (`bufferscopeSpread1_*`),
`Core/info.scd` (`Info1`..`Info100`) and `Recorder/recbuf.scd` were rebuilt in
October 2026 from the shipped binaries, whose sources were lost; they compile to
the same bytes. Older versions of the output SynthDef are in
`WIP/Core/outputVariants.scd`.
