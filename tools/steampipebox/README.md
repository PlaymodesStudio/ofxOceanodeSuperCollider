# SteamPipeBox tools

See [the instrument documentation](../../synthdefs/Defaults/SteamPipeBox/README.md).

- `import_ensemble.py`: native extractor -> controls, snapshots, audit, native
  banks and embedded node data. `--report` accepts a previously extracted report.
- `generate_ugen.py`: strict build-time expression translation of the audited
  kernels into the native UGen source and both copies of its SC class.
- `primary_test.py`: exact reference comparison for 60,000 NI ADSR samples and
  20 Multi 2-Pole coefficient pairs, without requiring Reaktor.
- `capture_primary_reference.py`: optional LLDB capture in a new isolated Reaktor
  6.5 arm64 process stopped at main; checks binary SHA before invoking primitives.
- `retrigger_test.py`: existing-voice preset recall, exact first-sample gate/velocity,
  pitch steps, repeated Tarrega/PVC notes and manual slider drags at 1/64/256 samples.
- `resonance_test.py`: dry Tarrega/Coolharp plucks; requires sustained tonal tails
  after gate-off, without reverb or post-delay masking the result.
- `render_test.py`: offline factory, block-size and production-graph validation.

The input ensemble is read-only and is not distributed here. No scripts change
installed apps, user presets or unrelated synths.

- `snapshot_banks_test.py`: native UUID slot order and ensemble/instrument scope; verifies Flute #1, Tarrega #55 and all 65 unchanged instrument sounds.

- `steampipe2_presets.py`: capture the 87 native V2 instrument snapshots. The
  subsequent `import_ensemble.py` run keeps that capture and adds a separate
  V2 bank, with R66 reverb approximations and unmapped voice settings documented.
- `v2_test.py`: compares all 172 old voice renders against `v1-voice-reference.json`
  and checks four-point interpolation, phase compensation, legato and cutoff depth.
