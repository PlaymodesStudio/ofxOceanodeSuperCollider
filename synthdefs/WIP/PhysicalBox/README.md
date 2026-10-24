# PhysicalBox, Phase 0

**Status (2026-10-21):** Phases 1 and 2 are built. The C++ UGen is
`ugens/OceanodePhysical/` and the node is `src/nodes/scPhysicalBox.*`, with
its SynthDefs in `synthdefs/Defaults/PhysicalBox/`. This folder stays as the
reference prototype: the UGen renders the same test matrix to the same
results.

A playable prototype of the PhysicalBox engine, to try out the instrument idea
before writing the C++ UGen and the node. It runs in the SuperCollider IDE
and isn't part of Oceanode yet.

| File | What it is |
|---|---|
| `physicalbox.eel2` | The voice, in EEL2 for DynGen, sample by sample: exciters, two coupled resonators, nonlinearities, two pickups. Meant to port almost line by line to C++. |
| `physicalbox_core.scd` | Loads the script for DynGen: rewrites every `_param` to `in(k)` (DynGen 0.6.1 crashes scsynth on native parameters, see `src/nodes/scDynGenPolyNode.cpp`) and defines `~pbSpecs`, `~pbDef`, `~pbVoice`. |
| `physicalbox.scd` | The playable prototype: window, Material pad, presets, A > B morph, computer keyboard, MIDI, MicroSpace. |
| `reference.scd` | MiElements, DWGBowed, DWGFlute and DWGClarinet3, to compare against. |

To play it, open `physicalbox.scd` in the IDE and evaluate its block. It
needs DynGen and mi-UGens in the user extensions.

## Engine

```
Strike -+ impulsive -+                                  +- pick1 -> L
Scrape -+            +-> A: waveguide (string <> tube) -+
Ext    -+            |        couple / mutual           +- pick2 -> R
Bow    -+ feedback  -+-> B: modal bank (bar .. membrane) +
Blow   -+ (they read the resonators' own motion at the contact)
```

- **A, the waveguide:** split at the contact point (`pos`) into a nut-side
  and a bridge-side segment, as in STK's bowed string. `abore` morphs the
  end reflections:
  - 0 is a string;
  - 0.5 is a closed tube (odd harmonics, used by the flute and the clarinet);
  - 1 is an open tube.

  The loss filter's cutoff follows the pitch, so `adecay` is the decay of the
  fundamental at every note. `astiff` adds four allpasses (dispersion), and
  tuning compensates for their delay.
- **B, the modal bank:** up to 48 two-pole modes. `bshape` morphs the mode
  ratios across harmonic, bar, glass, bell, plate and membrane. `bstretch`
  stretches them. Mode amplitudes come from where you excite (`pos`) and
  where you listen (`pick1`, `pick2`). Each mode has three input gains:
  - impulse-normalised, for strike and bow;
  - noise-normalised, for scrape, ext and coupling;
  - resonance-normalised.
- **Material** (0 wood, 0.5 glass, 1 metal) sets how fast the high modes die
  (decay tilt) and how long everything rings.
- **excite / listen** decide where the exciters touch and what the pickups
  hear (0 = A, 1 = B). `couple` sends A into B; `mutual` sends B back into A.
  Detuning B against A (`btune`) gives beating and sympathetic shimmer.
- **Exciters:**
  - Strike is a half-sine contact pulse. Hardness and velocity shorten it,
    mass lengthens it, and bounce adds re-hits.
  - Bow is stick-slip friction using STK's bow table.
  - Blow crossfades a jet (flute) into a reed (clarinet) on the same breath.
  - Scrape is micro-impacts; chatter lets the body's motion trigger more.
  - Ext is audio input.
- **Break:**
  - `rattle` is a dead-zone contact with noise (jawari, snare wires).
  - `tension` raises pitch with energy.
  - `cascade` drives the upper modes from the low modes' motion squared, so a
    hit blooms brighter before it decays. It is feed-forward only, so it
    cannot run away.
- **Life:** `drift` is a slow random walk of each mode's tuning; `freeze`
  stretches decays towards infinite.

## Measured

`tools/physicalbox/render_cases.scd` renders 24 cases offline (NRT) and
`analyze.py` measures them. Results on 2026-10-21, at 48 kHz:

- Struck strings stay in tune from 55 Hz to 1760 Hz, and a stiff string
  stretches its partials sharp. Bar, bell, wood, membrane and plate are tuned
  to their tables.
- The bowed string locks to 220.1 Hz (clarity 1.00), and the bowed metal bar
  to 221 Hz.
- The flute plays 441.8 Hz at 440 Hz (clarity 1.00) and 263.5 Hz at 262 Hz.
  Above breath 0.9 it goes sharp and overblows.
- The clarinet plays 220.1 Hz.
- Tam-tam with cascade 0.8: brightness climbs from 677 Hz to 1065 Hz over
  1.5 s, then falls. Without cascade it only darkens (582 Hz to 402 Hz).
- Nothing in the matrix goes NaN or trips the safety reset.
- CPU: about 5% of one core per voice with everything on (48 modes),
  measured from NRT render time.

## Known limits, for Phase 1

- The **bowed glass** sings near its pitch, but the pitch wanders up to about
  15% (friction on a dense modal bank catches several modes). A banded
  waveguide for the bowed modes (Essl & Cook) would fix it.
- The **flute** needs the closed-tube bore and jet ratio about 0.48 (STK's
  0.32 times its 1.5 overblowing). With an open bore the jet sets the pitch.
- The **blown modal body** has no physical amplitude limit, so its force
  backs off as the body gets loud (`kagc`).
- **Mode shapes** for the 2D bodies (plate, membrane, bell) are approximate
  (a sine with a phase per mode), and so are the higher bell partials.
- The tuning gains at the top of `@init` were set by rendering the matrix.
  If one changes, render the matrix again.
