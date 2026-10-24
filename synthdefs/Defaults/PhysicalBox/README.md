# PhysicalBox

A physical-modelling synth node (`src/nodes/boxSynths/physicalBox/scPhysicalBox.*`). Each voice is
one `PhysicalVoice` UGen (`ugens/OceanodePhysical/`) with:

- strike, bow, blow, scrape and external exciters;
- a waveguide (string to tube) and a modal body (bar, glass, bell, plate,
  membrane), coupled;
- rattle, tension and cascade nonlinearities;
- two pickups.

Around the voices are the shared modular parts:

- ModGate, and the scModulation LFOs, envelopes and routes, per voice;
- a stereo-pickup spreader;
- MixerPro's Saturation, EQ, Echo and Reverb;
- Space (MicroSpace).

The engine was prototyped first in DynGen (`synthdefs/WIP/PhysicalBox/`); the
UGen ports it line by line and renders the same test matrix (see below).

## SynthDefs (`physicalBox.scd`)

| SynthDef | What it does |
|---|---|
| `PhysicalBoxVoice` | One voice. Reads its gate on `\gatebus`: ModGate's gate × level, and the level is the velocity. Reads Pitch (MIDI, or `\pitch_ar` with `\pitch_sel`), every PhysicalVoice input by name, and the per-voice modulation buses `\mpitch \mmat \mexc \mpos \mhard \mpress \mspeed \mbreath \mscrape \mbright \mdecay \mshape \mcouple \mcascade \mamp`. `\in` feeds the Ext exciter. Writes two channels (the pickups), plus level, gate and note to `\vis` at voice × 4. |
| `PhysicalBoxSpread<C>` | 16 voices × 2 pickups onto C speakers. Voice *i* sits at `\pan[i]` + `\mpan`, with its pickups `\width` apart. Placement and level compensation are PolyCombSpread's. |
| `PhysicalBoxSpace<C>` | MicroSpace on the C channels (in pairs when C > 2): `\mix \time \damp \hp \diff \freeze`. `\on` crossfades it out, then the node pauses it. |

Compile it with SuperCollider. It needs the `PhysicalVoice` class
(`ugens/OceanodePhysical/OceanodePhysical.sc`) and mi-UGens. It also uses
`Defaults/Modulation` and `Defaults/MixerPro`. "Sync with SynthDefs Git"
builds all of this, and installs the UGen into the app's Extensions.

## The node

- **Node controls:** Show, Voices (1–16), Channels (1–16), Gate, Pitch,
  Master, Reset, an audio input In (Ext exciter) and the Out port.
- **Gate:** a level (the velocity). It takes Oceanode values or an audio
  cable, and so does Pitch. A strike fires once the gate level settles: up to
  one block after an Oceanode value, one sample after a cable.
- **Gate Mode** (window header): Auto, Gated or Drone, as in AliasingBox and
  PolyComb.
- **Strike** (window header): strikes every voice, including while droning
  or held.

### The window

- **Material pad:** wood → glass → metal across; waveguide → body up and down
  (sets Excite and Listen).
- **Object view:**
  - the waveguide's shape just after a pluck or strike at the contact;
  - the contact (triangle) and the two pickups (circles), all draggable;
  - the body's modes, computed by `PhysicalModes.h` exactly as the voice
    computes them. Height shows how much the contact and pickups get of each
    mode; colour shows how long it rings.
- **Voice meters.**
- **Tabs:**
  - Sound: five columns of exciters, contact, output, waveguide, body,
    coupling, break and life;
  - Modulation: envelopes and LFOs;
  - Routing;
  - Effects: Saturation, EQ, Echo, Reverb and Space. Space is on by default
    at mix 0.25.

### Modulation

Every voice is its own instrument, so LFOs, envelopes and random sources move
each voice separately. The server-side targets, one bus channel per voice,
are:

- Pitch (st), Material, Excite, Position, Hardness;
- Bow Pressure, Bow Speed, Breath, Scrape Speed;
- Brightness (A and B), Decay (octaves of the decay time), Shape;
- Couple, Cascade, Amp, Pan.

An envelope routed to Amp becomes the VCA. Any other control can be
modulated from its right-click menu, at frame rate.

### Snapshots, presets and other conventions

- **Snapshots and presets:** the shared snapshot bar. There are 20 factory
  sounds: the Phase 0 prototype's presets plus `init`.
- **Published combos:** a published combo is an Oceanode dropdown.
- **Timeline:** right-click any control to add it to the timeline; the node
  is a timeline instrument (Gate and Pitch).

## Checks

- `tools/physicalbox/render_cases.scd` with `PB_ENGINE=ugen` renders the
  24-case matrix through the UGen. The results match the DynGen prototype
  case by case. Only noise-driven cases differ, because the random
  generators differ.
- Through the real chain (ModGate → voices → spreader → Space, offline):
  - the strike velocity follows the gate level (0.9 against 0.3 gives a
    3.16× peak ratio);
  - per-voice modulation buses change each voice independently.
- CPU: about 0.35% of one core per voice with everything on (48 modes).
