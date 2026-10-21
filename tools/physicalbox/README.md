# PhysicalBox render tests

Offline (NRT) checks of the Phase 0 voice in
`synthdefs/WIP/PhysicalBox/physicalbox.eel2`. They need no sound device and
no running server, but do need DynGen in the user extensions.

```bash
/Applications/SuperCollider.app/Contents/MacOS/sclang render_cases.scd /tmp/physicalbox-render
python3 analyze.py /tmp/physicalbox-render
```

`render_cases.scd [outDir] [filter|all] [script.eel2]` renders one WAV per
case: a filter keeps the cases whose name contains it, and a script path
renders a variant of the voice. `PB_CASES=<file>` replaces the built-in cases
with the array written in that file, which is useful for parameter scans.
`PB_ENGINE=ugen` renders the same cases through the Phase 1 C++ `PhysicalVoice`
instead of the DynGen script, to compare the two.

`analyze.py` prints, for each case:
- peak and RMS (0.4–1.4 s and the last 0.3 s);
- the autocorrelation pitch and its clarity;
- the spectral centroid;
- how long it stays within 60 dB of the peak;
- the L/R correlation;
- the lowest partial within 20 dB of the strongest one.

The measured results are listed in `synthdefs/WIP/PhysicalBox/README.md`.

## Body models (C++ PhysicalVoice)

```bash
/Applications/SuperCollider.app/Contents/MacOS/sclang render_models.scd /tmp/physicalbox-models [filter]
```

Renders every body model (Family / Shape, see `ugens/OceanodePhysical/PhysicalModes.h`)
struck, the doublets (Split cents, Beat Hz), a near-zero Split against none
(must match) and bowed cases. Needs OceanodePhysical in the user extensions.

## Factory sounds and the flute reference

- `factory_sounds.py` holds the factory sounds: `cpp` prints the C++ list for
  `scPhysicalBox.cpp`, `scd` the render cases (`PB_CASES=<file> sclang
  render_models.scd <dir>`), `levels <dir>` balances Level from those renders
  (loudest 50 ms to the same RMS; written to `factory_levels.json`),
  `library <nodeSnapshots/PhysicalBox>` rewrites an app's snapshot library with
  the factory set followed by the user's own snapshots (backed up first).
- `render_old_flute.scd` renders the old `Source/PhysicalModelling/flute.scd`
  voice; `compare_flute.py` measures harmonics, breath noise, tuning and pitch
  wobble of any renders (MIDI note in the file name, `_<note>`). The factory
  Flute was matched to it this way (open Bore, Air 1, Breath 0.9, Jet Ratio 0.5).

## CPU and null tests

- `bench_voice.scd [filter]` times 20 s of `PB_VOICES` (default 16) voices per
  case offline. The wall time includes ~3.4 s of scsynth start-up and plugin
  loading: the cost of a voice is (T16 - T1) / (15 x 20 s). Measured
  2026-10-21: about 0.26 % of a core per voice for a string or flute, 0.29 %
  for a bar, 0.39 % for a membrane or a bar with Split (all 32 modes below
  Nyquist, or doubled).
- `render_models.scd` seeds its noise (`PB_SEED`, default 1234), so two builds
  can be null-tested sample for sample. Struck sounds must match; bowed and
  blown ones are chaotic (a rounding change gives another realisation, as
  another seed does), so compare their level and spectrum against the
  variation between two seeds instead.
