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
