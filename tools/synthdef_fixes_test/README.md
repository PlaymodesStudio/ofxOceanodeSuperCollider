# SynthDef edge-case and retirement regressions

Run from the addon root:

```sh
python3 tools/synthdef_fixes_test/render.py
python3 tools/synthdef_fixes_test/migration.py
```

The render check uses the bundled sclang, the installed SuperCollider scsynth,
and a temporary isolated class library. It does not boot the live server, use an
audio device, install compiled files, or write to the user's SC extensions.
Output defaults to `/tmp/synthdef-fixes-test`; use `--output` to change it.

Fixtures preserve the pre-fix source snapshot from 2026-10-23. They are regression
references, not deployed library sources. Current and old bodies are compiled
with the same production Oceanode parameter classes at mono and stereo sizes.

- 34 NRT cases at block sizes 1 and 64 exercise sigmoid range endpoints,
  zero inflection, audio-rate modulation through zero, clipped input endpoints,
  the RBPF default and ordinary/extreme Q values.
- Fixed output must remain finite through startup and modulation. All tested
  finite static legacy sigmoid cases and ordinary RBPF settings must be
  sample-identical. RBPF's former default must become finite and audible.
- CircularBufferFFT's compiled DSP payload must remain identical. Its legacy
  `numreaders` still scales coherent copies, as now stated in its description;
  use CircularBufferFFT_B for independent diffused readers.
- The retired MonoBandPassBank32 DSP must equal MonoBandPassBank64 at both sizes.
  Migration maps to 64 bands, preserving what the former "32" actually did.
- Migration fixtures cover presets and macros, both node-name styles, arrays,
  connections, backups, disjoint existing canonical nodes and idempotency.
  Conflicting node IDs/filenames are rejected before writing the folder.

For external saved graphs using the retired name, migrate before syncing:

```sh
python3 tools/migrate_synth_renames.py /path/to/app/bin/data --dry-run
python3 tools/migrate_synth_renames.py /path/to/app/bin/data
```

No MonoBandPassBank32 references were found in the available app preset/macro
JSON or the shared preset sources during this cleanup. The migration remains for
external/older copies. It is an explicit preset migration, not a runtime alias.

OteyPiano remains marked `@oceanode-no-autocompile`. Its shipped binaries are
protected; recovering a matching source is separate work. These fixes do not
claim to repair that unreproducible build.
