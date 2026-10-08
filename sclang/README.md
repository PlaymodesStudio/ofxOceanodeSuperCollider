# Bundled sclang (SynthDef compiler)

A headless SuperCollider language interpreter that Oceanode runs to compile
`~synthCreator` synths for channel counts that have no `.scsyndef` yet
(see `src/scSynthdefCompiler.*` and `synthdefs/Oceanode/compileExact.scd`).

| Path | Contents |
|---|---|
| `osx/sclang` | Universal (arm64 + x86_64) sclang built without Qt; links only macOS system frameworks. |
| `SCClassLibrary/` | The class library from the same source, without the Qt GUI classes. |
| `Extensions/` | Class files (`.sc` only) of the third-party UGens and quarks the synths use: `user/` mirrors `~/Library/Application Support/SuperCollider/Extensions`, `quarks/` the quarks from `sclang_conf.yaml`. |
| `build_sclang.sh` | Rebuilds `osx/sclang` and `SCClassLibrary/` from a SuperCollider checkout. |

The Oceanode pseudo-UGen classes are not copied here: the compiler includes
`synthdefs/Oceanode/` directly.

At run time Oceanode writes its own `sclang_conf.yaml` with
`excludeDefaultPaths: true`, so whatever SuperCollider install or user
extensions exist on the machine are ignored and every machine compiles the
same way.

## Deploying to an app

`tools/deploy_synthdef_compiler.sh <app>/bin/data` copies this folder to
`data/Supercollider/Sclang/` and the sources to
`data/Supercollider/SynthdefSources/`. On a development machine nothing needs
deploying: Oceanode falls back to this addon folder.

## Updating the extensions

When a synth starts using a new UGen, copy that UGen's `.sc` class files into
`Extensions/user/<Plugin>/` (the server-side `.scx` stays with scsynth).
