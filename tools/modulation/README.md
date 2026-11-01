# Shared LFO outputs

Right-click the bars at the top of an LFO card and choose **Publish LFO output
to Node GUI**. The `LFO 1 Out` (etc.) output carries a float vector in 0..1,
one value per voice. AdditiveBox exports the displayed partial vector when
that LFO is running across partials. SampleBox pads and GrainBox engines
have separately named outputs.

The outputs keep updating when the editor is closed. Server values are polled
at approximately 30 Hz and forwarded on node updates; these are Oceanode
control values, rather than an audio-rate cable. A removed/inactive LFO emits
zeros. Disconnect an output before unpublishing it.

Publication is saved with the patch and restored before connections, separately
from sound snapshots. Removing a pad or engine removes its output pins too.
No output parameters or extra server polling are created until publication.

Run `python3 tools/modulation/lfo_outputs_test.py` for lifecycle, persistence,
hidden-window polling, vector sizing and connection protection checks.

## Scalar controls and voice modulation

A voice control can remain a scalar in the editor and node GUI. Hosts register
these controls with `addVoiceParam()` and read `voiceValue()` / `voiceValues()`
when sending their DSP inputs. The parameter itself stays at its shared base;
each voice receives that base plus its own modulation, clipped to the control's
range. For LFOs the offset is `(value - 0.5) * amount * range`. Envelopes and
velocity add unipolar offsets. Shared effects and structural selectors keep
shared controls.

`voiceParamsChanged` also refreshes DSP inputs when only later voices change.
Snapshots save the scalar base and modulation routes, without storing live
per-voice values. Existing generic routes gain per-voice behavior automatically
when their host registers the control; matrix target indices are unchanged.

Noise LFOs interpolate random values with a smooth quintic curve, consistently in shared
ModLfo/ModLfoX, AdditiveBox, GrainBox and C++ previews. Each voice has staggered interpolation knots. There is no gradient lattice
forcing all voices through the middle value at integer times. Noise Scale
zero intentionally gives all voices the same noise trajectory.

Additional checks:

- `python3 tools/modulation/voice_params_test.py`: production routing code,
  stable scalar bases, edits, secondary-voice changes, resize and removal.
- `python3 tools/modulation/noise_algorithm_test.py`: production C++ algorithm,
  collective crossings/stops, continuity, field independence and shared SC code.
- `python3 tools/modulation/noise_render_test.py`: offline audio regression for
  noise independence and independent envelope attacks (compile SynthDefs first).

### Validation status (2026-10-22)

The combined OceanAudio Release build and all default SynthDef compilations
passed. The latest sources and matching compiled definitions are installed in
OceanAudio. The full audit found no duplicate filenames or missing generic
metadata/mono/stereo builds across 418 generic source files.

`noise_render_test.py` and `tools/steampipebox/panning_render_test.py` pass
against OceanAudio's installed SynthDefs, including independently phased LFO
indices, live Pan telemetry, ring wrapping and independent envelope attacks.
Both render tests accept `--synthdefs /path/to/Supercollider/Synthdefs`.

The production C++ routing, noise algorithm, LFO publication and factory panning
regression checks also pass. The original-mode SteamPipe kernel reference
hashes remain unchanged. Restart the application/server to replace any synths
already running with older definitions.

## Shared Ramp generators

**+ Add Ramp** adds a Ramp to any box using the shared modulation editor.
Ramps and LFOs share eight source slots, preserving existing route IDs. Route
**Ramp N** to Pos or any other target in Routing. The GrainBox editor calls
this tab **Modulators**. Gate / Go starts each voice independently; seconds
or Beats set the duration. After Time can hold, loop, or continue the final
slope. GrainFxBox also offers **Stop GO**, which releases the Go latch when
that gated Ramp's first voice completes. Completion comes from the server,
including when the editor is closed; externally connected gates keep input
control. Disabled or free-running Ramps do not release Go. Bounds can clamp or wrap the output, or leave it unbounded.

From / To set values; **Integrate rate** instead integrates those rates in
spans per cycle, which supports acceleration and tape braking. Curve shapes
the transition. **Pow** and **BiPow** ease normalized time, using the shared
LFO's rational shaping. Both default to zero. In rate mode, easing time also
changes instantaneous speed; continuation follows the eased endpoint slope.
The preview and AdditiveBox partial tables use the same C++ reference math
as the Ramp DSP. Existing LFO presets still select the LFO engine.

GrainFxBox factory Motion effects and saved Motion settings translate on
recall to Ramp -> Pos. **Freeze On Gate** controls recording independently;
**Follow Scan Pitch** derives grain pitch from Pos movement where needed.
The separate Tape / legacy motion UI and DSP have been removed. Saved preset
files are not rewritten. Migration uses an unused generator slot and preserves
other routes; a save using all eight slots needs a slot freed for conversion.

Run `python3 tools/grainfxbox/routing_test.py` for migration, easing/preview
agreement, Reaktor reference trajectories and rendered tape/stutter checks.

Run `python3 tools/modulation/ramp_stop_go_test.py` for server completion timing
and the host's Stop GO feedback handler. `python3 tools/synthdef_sync/replaced_source_test.py`
checks retirement of the obsolete BeatRepeatPro bootstrap source and builds,
including repeat-sync stability and preservation of backups and user sources.

## Transport linking

Every shared box editor offers an optional **Transport** switch per LFO or
Ramp. Enable **Sync / Beats** as well. The module uses the common anchored
transport clock, including playing/stopped state, seeks, loop points and tempo
changes. Classic, Noise, Random and sample/hold use this clock; AdditiveBox's
partial lanes share it too. Free-running linked generators stay on the global
grid even when created halfway through a beat. Gate / Go modes wait for the
next cycle boundary, then start their full progression. Start/seek/loop
anchors restart gated progressions. Random and Noise offer **Repeat loop**: on reuses the sequence at each
transport loop; off continues drawing fresh targets across loops. Earlier
presets without this flag keep their previous repeating behavior. New LFOs
default to fresh draws across loops.

**Seed 0** means no fixed seed: fresh entropy per LFO instance, recall, Reset,
and linked transport restart/seek. The private random salt is shared across
that LFO's voice chunks and servers, so Random Same still works. A positive
seed reproduces the sequence. Reseeding and loop repetition are independent:
a fresh sequence can still repeat within each loop when Repeat loop is on.

**Reset LFOs / Ramps** restarts local/gated sources. Reset the global transport
to restart linked free-running sources. Envelopes keep their note Gate;
**Beats + Transport** uses the global tempo for their stage durations,
including host amplitude-envelope copies. All Transport flags default off
when loading earlier sounds.

Jump timing and destination spacing are separate: **Random Stepped, Beats 1**
draws once per beat. **Pos span 8 beats, amount 1, Steps 9** selects destinations
0..8 beats (nine levels include both endpoints). The region can stay in ms;
it must cover the selected beat span at the current tempo. A smaller region
clips destinations at its edge. Use zero random smoothing/glide for abrupt
jumps, or the delay line's Jump crossfade for click-free transitions.

`python3 tools/modulation/transport_test.py` renders late joining generators,
play/stop, seek, tempo changes, unlinked compatibility and RANDOMPOS spacing.

## Region units and random coverage

The GrainBox/GrainFxBox **Region** section now exposes the existing node-wide
**Units** selector directly: Relative, ms, or Beats. Switching units converts
In/Out, Position, Duration and their jitter without a timing jump. A region
stored in Beats follows tempo, so an eight-beat span remains eight beats.

Steps controls possible destinations, not a guarantee that each is visited
within one transport loop. Eight draws can include duplicates. With Repeat
loop on those same draws recur each time; turn it off to keep exploring the
nine destinations across successive loops. RANDOMPOS uses a 0–8 beat region,
a one-beat Random interval, Steps 9, Pos amount 1, Repeat loop off, Seed 0.

`python3 tools/modulation/random_loop_test.py` tests all nine destinations
across successive eight-beat loops at 100 BPM, repeat behavior, fixed/unfixed
seeds, and the native ahead/late loop-anchor calculation.

## LFO card layout

Smp (where available), Transport and Sync share a row above the Timing/Shape
and Voices columns. Cards wrap to fewer columns in narrower windows, keeping
controls within the display width. Timing **Pow** is phase progression;
Shape **Pow / BiPow** reshape values. Random/Noise hide the unused phase-power
and ramp-wave shortcuts. Detune hides while transport overrides it; S&H Glide
appears only when S&H is active, and Repeat loop only in its free-running
transport mode. Hidden controls retain their stored values.

GrainFxBox Jump transitions normalize their crossfade gains. This prevents
correlated beat-position taps, including zero delay, adding a roughly 3 dB
peak during overlap and clipping an otherwise clean input. The Glide path
keeps its tape-speed behavior. `python3 tools/grainfxbox/zero_position_test.py`
checks static zero and repeated jumps at near-full input level, correlated
and uncorrelated phases, 64/512-sample blocks and live-buffer wraparound.
