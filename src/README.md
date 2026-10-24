# Source organization

Only the addon entry points, controller, server manager, base `scNode`, startup
helper and compile-time configuration live directly in `src/`. Registration
units live in `registration/`; concrete nodes live in `nodes/` by function.

| Folder | Responsibility |
| --- | --- |
| `nodes/boxSynths/` | One folder per Box instrument, effect or sequencer; see [conventions](nodes/boxSynths/CONVENTIONS.md) |
| `nodes/boxSynths/shared/` | Box modulation, snapshots, morph policies, voice-count transitions, gates, chord links and piano helpers |
| `nodes/boxSynths/polyComb/` | PolyComb node, pitch maps and its specialized morph implementation |
| `nodes/boxSynths/fmBox/` | FMBox node, DX7 data and AFX import |
| `nodes/analysis/` | Metering, scopes, FFT, pitch and beat tracking |
| `nodes/buffers/` | Buffer allocation, browsing and custom buffers |
| `nodes/effects/` | Dedicated effect nodes |
| `nodes/generators/` | Dynamic synthesis and other generators; generator helpers live in `shared/` |
| `nodes/recording/` | Recording and the current RecBuf implementation |
| `nodes/routing/` | Mixers, channel routing, output, block delay and feedback |
| `nodes/sequencing/` | Other sequencing nodes |
| `nodes/synthdefs/` | Generic and code SynthDef nodes |
| `nodes/system/` | System information and freezer nodes |
| `nodes/timeline/` | Timeline-specific node adapters |
| `nodes/diagnostics/` | Explicit diagnostic nodes |
| `audio/` | Shared routing, insert effects, startup, control batching and morph audio |
| `ui/` | Reusable parameter menus, typed value entry, dropdowns and rendering helpers |
| `timing/` | Scheduled inputs and transport compatibility |
| `samples/` | Sample browsing and embedding |
| `synthdefs/` | Compilation, synchronization and runtime/plugin path discovery |
| `integration/` | Bridges to other addon features, currently MacroMixer |
| `external/` | Third-party code |
| `docs/` | Design and behavior references |

Place a helper with its feature when only that feature uses it. Promote it into
an appropriate shared functional folder when unrelated nodes need it. The
parameter context-menu integration is centralized in
[`ui/scTimelineMenu`](ui/scTimelineMenu.h): nodes register their controls and
supply publication callbacks; MIDI learn and timeline actions use the host's
shared facilities. Typed entry and Box modulation menus have their own shared
helpers. Individual editors still compose the menu and implement publishing
for their own control registry.

Use explicit relative includes for addon-local headers. Update `__FILE__`-based
asset fallback paths when moving a source file; source and runtime data layouts
are separate. SynthDefs remain in the repository's `synthdefs/` tree and bundled
assets in `data/`. Moving a C++ class does not rename its saved node model.

## Build after a move

Update each consuming application's project with openFrameworks Project
Generator, then rebuild. Existing generated projects retain the old source
paths. The addon source scanner discovers these subfolders; no manual source
list is needed in `addon_config.mk`.

## Approved retirements

The following C++ implementations have been removed with user approval. Their
SynthDefs and preset assets are retained.

| Removed implementation | Replacement / status |
| --- | --- |
| `scSimpleSamplePlayer` | SampleBox; the old diagnostic player was unregistered |
| `scFM7` (`SC SuperFM`) | FMBox |
| `scFM7Drone` (`SC SuperFMDrone`) | FMBox in Drone mode |
| `scPolyphonicArpeggiator` | ArpeggiatorBox; the old node was unregistered |
| `scRecbuf` | Registered `scRecbufHold` retains the `Sc RecBuf` model |

FM patch migration is explicit rather than a silent model rename: controls,
envelopes, carrier normalization and connections differ. Use
`tools/migrate_superfm_to_fmbox.py DATA --stage /tmp/fm-migration` to inspect a
conversion, then `--apply` to back up and write it. It also creates a six-sound
bank for the BIENAL data set when those six original instances are present.
