# SpaceSystem-Geiger → Oceanode

Resumes the interrupted Claude Code conversion of `SpaceSystem-Geiger.ens`.
Targets the saved state ("espills"), using reusable macros and library SynthDefs:
three stochastic sources, two banks of six resonators per quad channel,
MicroSpace, random LFO automation and the original quad output fold.
The other five Reaktor snapshots are not imported.

Installed preset: `OceanAudio/bin/data/Presets/DEBUG/22--SPACESYSTEM_GEIGER`.
Reusable macros: `Macros/AUDIO/REAKTOR/{GEIGER_CLICK,GEIGER_NOISE,GEIGER_SAW,NI_RESONATOR_QUAD}`.

From the addon root, with SuperCollider installed in `/Applications`:

```sh
python3 tools/spacesystem_geiger/prepare.py /tmp/spacesystem-geiger
python3 tools/spacesystem_geiger/render_preset.py /tmp/spacesystem-geiger/data/Presets/DEBUG/22--SPACESYSTEM_GEIGER /tmp/spacesystem-geiger/defs /tmp/spacesystem-geiger/preview.wav 60 48000
python3 tools/spacesystem_geiger/validate.py /tmp/spacesystem-geiger
python3 tools/spacesystem_geiger/check_controls.py /tmp/spacesystem-geiger
python3 tools/spacesystem_geiger/check_transport.py /tmp/spacesystem-geiger
python3 tools/spacesystem_geiger/install.py /tmp/spacesystem-geiger /path/to/OceanAudio/bin/data
```

Installation backs up overwritten files and complete replaced preset/macro folders inside `Supercollider/_backups`.
Restart OceanAudio after installation so it discovers the new metadata.
The generator overwrites its own output preset and four macro directories;
use a scratch output directory when rebuilding.

## Repairs and validation (2026-10-22)

- Fixed RandomMetro's demand arithmetic: audio-rate MulAdd optimization caused
  a null calculation callback and a scsynth crash. Demand streams prevent it.
- Explicit mono-to-quad expansion of dry/wet and gain automation.
- Saw octave mapping spans all eight octaves instead of clamping at one.
- Reaktor's parabolic saturator restored after each source filter.
- Linear resonator crossfades replace the equal-power library crossfade.
- Offline renderer broadcasts constant parameters across vector controls like
  Oceanode and frees synths before NRT shutdown.
- Built 24 required SynthDef variants; 60 seconds at 48 kHz passed with finite,
  nonzero audio on every channel (largest peak 0.542). Fixed-clock regression:
  50 events, subsequent intervals exactly 960 samples at 48 kHz.
- User confirmed the installed preset works in OceanAudio.

## Live instrument revision

- ScBus fan-in replaces audio addition nodes, including the six resonator voices
  and source/output sums. The only Arithmetic nodes calculate LFO power curves.
  Routing uses ChannelDuplicator, with explicit mono-to-quad LFO duplication.
- Macro controls are numeric `v_f` inputs. Audio LFOs forward through Level and
  DryWet; unplug their external cables to edit those values manually.
- Source macros expose On, Level, Chance, interval range/minimum/curve, cutoff
  range/minimum, resonance range/minimum, Gate and manual Fire. Noise and saw
  also expose decay range/minimum; saw adds attack, pitch, fine and octaves.
- The native Custom GUI opens with the preset. Each bank has a PianoKeyboard
  for its Notes vector (MIDI 24–84 shown), plus transpose, detune, feedback,
  color, wet/dry, level, low-pass and reverb controls. Source, master and LFO
  controls are also available. Each piano click adds/removes a note. Six voices
  use the six lowest selected notes; absent voices stop receiving excitation
  and their tails decay. More than six selections remain visible but notes
  above those six are unused. The editor itself is unchanged.
- ColorComb adds optional Detune/Spread (default zero). CrossfadeAB adds an
  optional linear mode; equal power remains its library default.
- Offline control checks verify source On switches produce silence, an empty
  wet-only chord produces silence, and a three-note manual chord produces
  finite nonzero quad audio with the external LFO cables removed.
- MacroMixer has no changes and is not used by this preset's implicit sums.

## Optional transport clock

The Periodic macros in `SpaceSystem-Periodic_midiin.ens` use Song Position,
24 ticks per quarter note, and a `beat` control described as steps per quarter.
Its saved rates are click 4, noise 1.5 and saw 2 steps/quarter. The new source
controls use equivalent musical defaults (1/16, 1/4-triplet and 1/8).

Each source has **Quantize to Clock** (the GUI's CLOCK switch), off by default,
and **Clock Division**: 1/64 through quarter/half notes, triplets, dotted notes,
and one, two or four bars. **Bar Beats** / BEATS PER BAR defines the meter
(4 for 4/4, 3 for 3/4). This bar length is explicit; it does not automatically
read the timeline's time signature.

With CLOCK off, the original random millisecond intervals remain active.
With CLOCK on, periodic transport pulses replace them; Range/Min/Curve ms
no longer determine the timing. Chance still omits hits (1 = every hit).
The other random timbral parameters, envelopes and audio processing remain
unchanged. Manual Fire bypasses the clock and remains available when stopped.
The mode follows host Play/Stop, BPM, position changes and loops. To make a
regular rhythm: enable CLOCK, choose a division, set Chance to 1, press Play.

`TransportPulse` interpolates audio-rate position between host Beat Transport
anchors and suppresses duplicate boundaries after small anchor corrections.
Tests cover periodic spacing, 60 Hz anchors, stop/resume, tempo changes, seeks,
triplets/dotted notes, 3/4 bars, clock selection and manual Fire. The new
RandomMetro free-mode impulse output is bit-identical to the prior SynthDef
with a fixed seed (compared by passing the old SynthDef as check_transport's
optional second argument). Full preset NRT tests also exercise CLOCK on at
120 BPM; render_preset accepts an optional transport BPM after sample rate
(zero/default means stopped).

The clock's reference ensemble is inspected for timing only: its extra delay,
MIDI routing and other sound changes are not imported.

## Source gain balance

Noise and saw now start at Level 0.0625 instead of 0.25 (-12.04 dB); click
stays at 1. In isolated 35-second renders at 44.1 kHz, with identical 500 ms
trigger spacing and Chance 1, the original post-bank average levels were
click -51.1 dBFS, noise -37.1 dBFS and saw -39.1 dBFS. Their peaks alone did
not reveal this large energy difference. Lowering the sustained sources
before the resonators also reduces the energy stored in the reverb tails.
These measurements are a reference balance, not loudness normalization:
random draws, harmony and effect settings change the actual levels.

The adjustment changes only the two source Level defaults. Resonator/reverb
processing, click, master level and saturation curves are unchanged. No
limiter was added. The earlier claim that the saved Reaktor level values
were retained therefore does not apply to these two calibrated defaults.

This is a structural and parameter translation, not a bit-identical render of
Reaktor. CompressorPro, library envelopes, random LFO interpolation, panning,
and the ColorComb TPT filter approximate their Reaktor counterparts; random
sequences differ. The saturator curve and linear crossfade were verified from
local Reaktor DSP disassembly. MicroSpace uses the separately reconstructed
library implementation.
