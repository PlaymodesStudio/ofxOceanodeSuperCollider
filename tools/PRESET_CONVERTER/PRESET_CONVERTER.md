# Oceanode preset converter

`oceanode_preset_converter.py` upgrades presets that use the fixed-channel
SynthDefs listed in Oceanode's legacy `Synthdefs.json`. Source presets are
never edited. Upgraded copies are written to `UPGRADED_PRESETS` by default.

The converter:

- selects the dynamic SynthDef using the old node's input/output channel count;
- maps renamed models and parameters conservatively;
- replaces `StereoMix*` nodes with the `CHAN_CONVERTER` Indexer + Panner structure;
- replaces old PanAz/Panner structures with `CHAN_CONVERTER_PANAZ`, preserving live Position connections;
- replaces `FORMS` with `Additive1080_*` plus a `Conversions` `hz-pitch` node feeding `Pitcharray`;
- maps old Analog FM pitch and Analog13ar input behavior through compatibility modes in the new `Analog`;
- maps `FMComplex13` to `FMFeedback`, `Buzz13` to `BuzzBass`, and `MultAR` to `Arithmetic` in Multiply mode;
- maps `SideChain13`, `PercMembrane13`, and `MalletIn13` through non-breaking legacy modes in their new models;
- maps `Additive13` to the dynamic-channel `Additive320_`, retaining 320 partials per output voice;
- compiles only a missing, exact channel/variable SynthDef variant with embedded SuperCollider code;
- validates every JSON file and rejects newly introduced dangling connection endpoints;
- writes `conversion_report.json`, including any nodes it deliberately left unchanged.

## Run from the presets folder

```sh
cd /Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/apps/Playmodes/OceanodeScroller/bin/data/Presets/GENERATIVE_MUSIC
```

Analyze the entire folder without writing anything:

```sh
/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/oceanode_preset_converter.py --all --dry-run
```

Convert one preset:

```sh
/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/oceanode_preset_converter.py --preset 104--ANIMA_DROPS_ASTRES_
```

Convert every immediate preset in the folder:

```sh
/Users/santiagovilanova/Documents/OF/oceanodeSynthdefsNew/oceanode_preset_converter.py --all
```

Those write to `GENERATIVE_MUSIC/UPGRADED_PRESETS`. Use `--output PATH` to
choose another destination, and `--overwrite` to replace copies already in the
destination.

Use `--require-complete` when this is part of a scripted cleanup. It returns
exit status 2 if any old node remains or any conversion issue was reported.
This is important before deleting the `OLD` SynthDefs or legacy registry: an
ordinary run safely produces partial upgrades for presets containing models
whose behavior is not yet mapped, and identifies them in the report.

Use `--no-compile` to report missing numbered SynthDefs without launching
SuperCollider. Run `--help` for path overrides and all options.
