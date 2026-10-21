# Modified third-party UGens

Some server plugins shipped in the apps' `data/Supercollider/Extensions/` are
built from patched upstream sources. The patches live here, so the plugins can
be rebuilt from a fresh clone; the binaries themselves stay in the apps' data.

| Patch | Upstream | Base commit | Shipped binary |
|---|---|---|---|
| `patches/sc3-plugins-oteypiano.patch` | [supercollider/sc3-plugins](https://github.com/supercollider/sc3-plugins) | `6f76da5` | `SC3plugins/OteyPianoUGens/OteyPianoUGens.scx` |
| `patches/sc3-plugins-dwgplucked.patch` | sc3-plugins | `6f76da5` | `SC3plugins/DWGUGens/DWGPlucked.scx` |
| `patches/sc3-plugins-cmake-arm64.patch` | sc3-plugins | `6f76da5` | (build fix for both, on Apple Silicon) |
| `patches/vstplugin-reaktor-fxp.patch` | [IEM vstplugin](https://git.iem.at/pd/vstplugin), `develop` | `72af654` | `VSTPlugin/plugins/VSTPlugin.scx` |
| `patches/vstplugin-v0.6-fxp-attempt.patch` | IEM vstplugin, `master` (v0.6.0) | `3f0ed8a` | none: earlier attempt, kept for reference |

Each patch applies cleanly to its base commit, on its own or together (`git apply --check`), and the
shipped binaries match these sources: VSTPlugin.scx and DWGPlucked.scx are
byte-identical to builds of the patched trees; OteyPianoUGens.scx was built
seconds after the last edit of its patched source.

## sc3-plugins patches

- **`sc3-plugins-oteypiano.patch`** (`OteyPiano.cpp`, `piano.h`, March 2025): on each rising edge
  of `gate` the piano re-reads all its parameters and reinitialises the model,
  so every new note picks up the current settings (upstream fixes them when the
  synth starts).
- **`sc3-plugins-dwgplucked.patch`** (`DWGPlucked.cpp`, January 2026): retriggering resets the
  release envelope (count, level and step) instead of continuing the previous
  one; the release level is clamped at zero.
- **`sc3-plugins-cmake-arm64.patch`** (`CMakeLists.txt`): adds the SSE compiler flags only when not building for
  arm64, which rejects them.

## vstplugin-reaktor-fxp.patch

Loading Reaktor `.fxp` programs (`VST2Plugin.cpp`, October 2025). Reaktor
writes program files whose header sizes do not match their data, which the
original code rejects:

- the program is read from the file's actual size, with a warning when the
  header disagrees and an error only when the header claims over 10% more
  data than there is;
- for Reaktor (plugin ID `0x4E695236`), a chunk shorter than its declared size
  is loaded with the data available; other plugins keep the strict check;
- debug logging of the sizes involved (the `DEBUGGING INFO FOR CHRISTOF`
  blocks, written for the vstplugin author).

Only reading is changed; writing `.fxp` files is upstream's code.

`vstplugin-v0.6-fxp-attempt.patch` (August 2025, on v0.6.0) is the first
version of the same fix: it trusts the file size for every plugin.

## Rebuilding

```bash
git clone https://github.com/supercollider/sc3-plugins.git && cd sc3-plugins
git checkout 6f76da5 && git submodule update --init
git apply <addon>/ugens/patches/sc3-plugins-*.patch
cmake -B build -DSC_PATH=<supercollider source> -DCMAKE_BUILD_TYPE=Release
cmake --build build --target OteyPianoUGens DWGPlucked
```

```bash
git clone https://git.iem.at/pd/vstplugin.git && cd vstplugin
git checkout 72af654
git apply <addon>/ugens/patches/vstplugin-reaktor-fxp.patch
# then build the SuperCollider target as described in vstplugin's README
```

Copy the resulting `.scx` files over the ones in each app's
`data/Supercollider/Extensions/`. The shipped OteyPiano and DWGPlucked
binaries are arm64 only; VSTPlugin is universal.

The OceanodeRadioIn UGen is not here: its source is maintained with its
counterpart in `ofxOceanodeOnlineRadio/sc/`.

## Own UGens

### OceanodeSpectral (`OceanodeSpectral/`)

`PV_MaskFilter` (October 2026): a spectral filter drawn by an array of gains,
for the SC Spectral Filter Box node (`src/nodes/scSpectralFilterBox.*`,
SynthDefs in `synthdefs/Defaults/SpectralFilterBox/`). Every FFT frame each bin
is scaled by the mask (its last K inputs) read on a log-frequency axis between
`lowFreq` and `highFreq`, moved by `transpose` semitones, wrapping round or
holding its edges; optional per-bin attack / release / hold.

- Source: `PV_MaskFilter.cpp`, class file `PV_MaskFilter.sc`.
- Build: `OceanodeSpectral/build.sh [SuperCollider source]` (default
  `~/Documents/SC/supercollider`) makes a universal `OceanodeSpectral.scx`
  (arm64 + x86_64; the .scx is not in git).
- Install: `OceanodeSpectral.scx` and `PV_MaskFilter.sc` in a folder of the
  user's SuperCollider extensions (the server loads it from there; sclang needs
  the class to compile the SynthDefs) and of the app's
  `data/Supercollider/Extensions/OceanodeSpectral/`.

## Our own plugins

| Plugin | UGens | Used by |
|---|---|---|
| `OceanodeAnalog/OceanodeAnalog.scx` | MorphOsc, AnalogFilter | AnalogBox |
| `OceanodeSpectral/OceanodeSpectral.scx` | PV_MaskFilter | SpectralFilterBox |
| `OceanodePhysical/OceanodePhysical.scx` | PhysicalVoice | PhysicalBox |

Their binaries are committed (universal arm64 + x86_64, ad-hoc signed), so a
fresh clone works without a SuperCollider source checkout. Install: copy each
plugin folder's `.scx` and `.sc` into the app's
`data/Supercollider/Extensions/<plugin>/` (and, to use them from sclang, into
`~/Library/Application Support/SuperCollider/Extensions/<plugin>/`), then
restart the server.

After changing a plugin's source, rebuild it with its `build.sh [path to the
SuperCollider source]` and commit the new `.scx` with the source. Replace an
installed copy by deleting it first, then copying (`rm` then `cp`): writing over
a loaded binary in place makes macOS kill scsynth when it loads it (exit 137).
`build.sh` signs the result.


### OceanodePhysical (`OceanodePhysical/`)

`PhysicalVoice` is PhysicalBox's whole voice:
- strike, bow (STK friction), blow (jet / reed), scrape and external exciters;
- a two-segment waveguide (string, closed tube, open tube) and a modal bank of
  up to 48 modes with six morphing shape tables, coupled;
- rattle, tension and a feed-forward cascade;
- two pickups.

Its inputs are named in `PhysicalModes.h` (`PHYSICAL_PARAMS`, shared with the
node's display) and in the class file (`PhysicalVoice.ar(ext, [\name, value,
...])`). It is a line-by-line port of the DynGen prototype in
`synthdefs/WIP/PhysicalBox/physicalbox.eel2`, measured against it with
`tools/physicalbox/` (`PB_ENGINE=ugen`).

Build it with `OceanodePhysical/build.sh [SuperCollider source]`. The class
file is also in `sclang/Extensions/user/OceanodePhysical/` for the bundled
compiler.
