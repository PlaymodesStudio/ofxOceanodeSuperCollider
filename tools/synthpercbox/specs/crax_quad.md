# Bits.ens — CRAX and QUAD voices (decode spec)

Sources: `~/Downloads/Bits_r6.ens`, extractor report `r6.json` (REPORT), `bits_decoded.json` (DECODED).
CRAX = Instrument object 4215, QUAD ("Quadratic Iterator") = Instrument object 3069. Everything below was read
from raw `connections` records (with `target_input_name`), `dump_macro.py`, `core_view.py` and raw module bytes;
anything not read directly is marked **GUESS**. Shared macros (Filter, Delay, Micro Space) are only described as
far as how these voices feed them; their internals belong to the shared-macro spec.

## 0. Conventions (same as `k1_stomps.md`)

- **Snapshot values are normalised** 0..1; value = `e1 + n·(e2−e1)`, then floored to the step grid. This rule
  reproduces the panel screenshots exactly (CRAX P n=0.375 → 1875 → **1870**; PW 0.9213 → **0.92**; + → **2000**;
  QUAD var 0.1496 → **0.14**, Ca → **0.54**, Co → **−0.53**). Tables show stepped value and (raw) when they differ.
- **DECODED `saved` values are not panel values** (mostly the range midpoint). Use the snapshots.
- **Lists store a 1-based index** in snapshots. List *entry values* read from the module bytes:
  - CRAX Osc wave (sid74, List 4383): SAW=0, PLS=1, TRI=2, SIN=3 → Selector Pos → Core outputs Saw/Pls/Tri/Sin.
  - CRAX LFO wave (sid65, List 4356): Saw=0, SawI=1, Pls=2, Tri=3, Sin=4, Rnd=5 → Selector → SwU/SwD/Pls/Tri/Sin/Rnd.
    (Panel "Sawl" is "SawI" = saw inverted = down saw.)
  - CRAX Filter type (sid66) and Delay filter type (sid71): HP=0, BP=1, LP=2.
  - CRAX **NoteRnd/Manual (sid64, List 4354): NoteRnd = 1.0, Manual = 0.0** (entry value is the reverse of the index!).
    **GUESS** that its snapshot ints are a 1-based *index* (1 = NoteRnd) like the other lists; if they were
    entry value + 1 instead, the mode flips in all 16 slots. Only slots 2 and 8 would be NoteRnd under the index reading.
  - QUAD c source (sid81, List 25559): "manual var"=0, "random trigg"=1, "ramps"=2 (panel: manual~/random~/ramps).
  - Snapshot max indices (6 for the 6-entry LFO list, 4 for osc, 3 for filters, never 0) confirm 1-based storage;
    the CRAX Delay menu "HP" in the screenshot = stored 1.
- **Buttons** store 0.5 / 0 in snapshots (raw shown; encoding unknown, see open questions).
- **Reaktor log times** (ADSR A/D/R): `t_ms = 10^(x/20)` (0 = 1 ms, 40 = 100 ms, 60 = 1 s, 100 = 100 s).
  D/R treated as time constants τ (segment 5τ, curve −5) per the project rule used in `k1_stomps.md` (**GUESS**).
  `~rkTime = { |x| 10 ** (x/20) / 1000 }`.
- **Pitch P inputs** (Multi 2-Pole, HP/LP 1-Pole): semitones, `Hz = 440·2^((P−69)/12)`.
- **Amp/Mixer Lvl** inputs are dB.
- **Tempo Info** = beats per second (BPM/60). Call it `tempo` (BPS) in the SynthDefs (Oceanode sends BPM/60).
- **Voice count**: not in the report. Audio Voice Combiners / "Last Voice" are boilerplate; every gate source
  is monophonic (Button, Sel. Note Gate). **GUESS: 1 voice** each.

## 1. Triggering — presets, buttons, MIDI

Same preset scheme as K1/STOMPS:
```
Sel. Note Gate (note k) ─► Separator(Thld 0) ─Hi─► Value(n) ─┐
                                         └─Lo─► (0) ───────┤ M ─► presets Out ─► Order:
preset Button n (press = n, release = 0) ───────────────────┘   (1) Snapshot.Snp = v, Bnk = 1
                                                                 (2) Snapshot.Recall (only if v>0)
                                                                 (3) Compare(v > 0) → gate event 1 / 0
```
- CRAX: macro "MIDI 59-72", notes **59..72** → slots 1..14 (Sel. Note Gate 6006 = note 0x3B = 59, read from
  bytes). QUAD: "MIDI 73-86", notes **73..86** → slots 1..14 (**GUESS** by name; same structure).
- Note-on / button press: recall snapshot n of bank 1, **then** gate = 1 (Compare outputs exactly 1:
  velocity discarded). Note-off / release: gate = 0, no recall.
- CRAX has 16 snapshot blocks for 14 slots (blocks 15–16 unreachable by preset, **GUESS**: file order = slot order).
  QUAD has 9 blocks for 14 slots (slots 10–14 only trigger).
- **Incoming MIDI pitch affects nothing** (no Note Pitch module in either instrument).

### CRAX extra event sources (all merged in M#4234, the "voice gate" stream)
| source | event values | also does |
|---|---|---|
| Trigger button (sid5) | press 1, release 0 (**GUESS** trigger mode) | – |
| presets | 1 on note-on/press, 0 on note-off/release | recalls snapshot first |
| **Randomize** button (sid76, range 0..0.55) | press **0.55**, release 0 | randomizes 14 controls (§2.6) |
| Sel. Note Gate #6144, **note 58** (byte 0x3A) | velocity on, 0 off | randomizes (around velocity) |

M#4234 feeds: Amp EnV gate, Filter Env gate, LFO `Rst`, the three Random cells 4238/4239/4244, Compare/Equal 4235.
**Every event fires them, including the 0 of a release/note-off**: a note-off re-rolls the NoteRnd randoms and
resets the LFO phase too. Compare/Equal 4235 → Relay 4237 has **no output wire** (dead code).

### QUAD event sources (M#3197)
on/off button (sid2; **GUESS** toggle: on = 1, off = 0) and presets. M#3197 feeds: ADSR gate, Lamp,
Separator(Thld 0)→ Core 3104 (random~ value, positive events only), and the `ramps` macro's sync inputs.

## 2. CRAX (object 4215)

### 2.1 Signal flow (exact)
```
                 Tempo(BPS)·BPMRate ─► MultiWave LFO(Rate, W=Width, Rst=gate events) ─► Sel(LFO wave)
                                                                                         │ ×Amp, +1
P fader (sid0, Hz) ─────────────────────────────────────────────────────────────────────► × ─► Core.P
+ fader (Hz) ───────────────────────────────────────────────────────────────────────────────► Core.p+
Relay(NoteRnd/Manual): randomness | |Rnd1|        ─► Core.rnd
Relay(NoteRnd/Manual): RndmRate   | |Rnd2|·44100  ─► Core.clkhz
Relay(NoteRnd/Manual): PW         |  Rnd3 (±1)    ─► Core.PW
Amp EnV ADSR(gate) ─────────────────────────────────────────────────────────────────────► Core.A
Core (MultiWave Osc) Saw/Pls/Tri/Sin ─► Sel(Osc wave) ─► Filter(Multi 2-Pole, Cut + Env·FiltEnv, Res, HP/BP/LP)
   ─► HP 1-Pole (P = 1 → 8.66 Hz: DC blocker) ─► Amp/Mixer (Level dB) ─► Delay (mono) ─► Out L
```
Instrument has **only an L output**; the ensemble wires it to both master L (+#1094) and R (+#1878): **mono**.

### 2.2 Core 4217 "MultiWave Osc" (626 modules) — decoded
Root:
```
F    = p+ + P + rnd · 5000 · R          (Hz, linear)
R    = Random (LCG, bipolar −1..1) clocked by Clk Gen(clkhz)
out  = {saw, pulse, tri, sine} · A
```
- **Clk Gen**: phase += clkhz/SR each sample; when ≥ 1 it wraps and emits a clock → `Random` draws a new value
  (`x = x·1103515245 + 12345`, out = x·4.6567e-10). Equivalent: **`LFNoise0.ar(clkhz)`** (S&H noise ±1, frozen at 0 Hz).
- **MultiWave Osc** (NI library, same as STOMPS's cell): `f = F·2/SR` **clipped to 0..1** (0..Nyquist), phase
  in [−1, 1) rising, wraps by −2. Negative F → f = 0 → **phase freezes** (output = DC at the frozen phase).
  This happens often: P 3200 with + −4843 (slots 8, 9), P 5000 with + −5000 (slot 10) → the osc only sounds
  when `P·(1+Amp·LFO) + rnd·5000·R` pushes F above 0. That gating **is** the "crack" character.
  - `saw` = phase + polyBLEP-style transition correction (anti-aliased), `pulse` uses pw clipped −1..1
    (**GUESS** duty = (1+pw)/2), anti-aliased; `tri` anti-aliased from the saw phase.
  - `sine` = polynomial sin(π·ph) on the phase (one cycle per period, 0 at ph = 0). Unlike STOMPS's cell,
    **CRAX's Sin output really is the sine** (m18 = sine·A → Sin).
  - All four outputs are multiplied by `A` = Amp EnV (envelope applied inside the cell, before filter).
- SC: `Saw.ar`, `Pulse.ar(f, (pw+1)/2)`, `LFTri.ar`, `SinOsc.ar` at `f.clip(0, SR/2)`, with a frozen-DC path at
  f ≤ 0 (same treatment as STOMPS: **GUESS** DC values). A shared-phase version (one `Phasor` + BLEP) would be
  exact; band-limited UGens are close enough.

### 2.3 Core 4225 "MultiWave LFO" (171 modules) — decoded
- Phase `p` in [−0.5, 0.5): `p += Rate/SR` (increment clipped 0..0.5); wraps by −1.
- **Rst**: any event value v → `p = clip(v,0,1) − 0.5`, then wrap → gate 1 or 0 both give p = −0.5;
  Randomize (0.55) gives p = 0.05. Rst also clocks the Random.
- Outputs (all ±1): **SwU = 2p**, **SwD = −2p**, **Tri = 1 − 4|p|**, **Sin = poly(0.25 − |p|) ≈ cos(2πp)**
  (−1 at reset), **Pls = (p ≥ W/2) ? −1 : +1** (comparison direction **GUESS**; W = Width −1..1),
  **Rnd** = new LCG value ±1 at each wrap and each reset.
- Rate = `Tempo Info (BPS) × BPMRate` (0..8 cycles per beat) → **tempo-synced**. BPMRate 0 (slot 1) freezes it.
- Use: `P_eff = P · (1 + Amp · LFO)` — Amp 1 swings P between 0 and 2P.
- SC: `φ = Phasor.ar(edge, rate*SampleDur.ir, 0, 1, Latch.ar(trigVal.clip(0,1).wrap(0,1), edge)); p = φ − 0.5`.

### 2.4 Small Core cells 4238 / 4239 / 4244
Each = the library `Random` (LCG, ±1) clocked by its input event (the gate stream).
- 4238 → |x| → Relay 4353 upper input → **rnd** (0..1)
- 4239 → |x| × 44100 → Relay 4357 upper → **clkhz** (0..44100 Hz)
- 4244 → (bipolar) → Relay 4358 upper → **PW** (−1..1)

Relay `Ctl` = List sid64 value: **NoteRnd (1) → random values** (new on every gate event, on and off);
**Manual (0) → faders** randomness / RndmRate / PW.

### 2.5 Envelopes, filter, output
- **Amp EnV** (macro 4274): Reaktor ADSR, G = gate stream (peak = event value: 1, 0.55 Randomize, velocity on
  note 58), A sid33, D sid34, S sid104, R sid105 → Core `A`. Presets: A = −20 (0.1 ms), D = 100 (100 s τ),
  S unchanged by most snapshots → **sound length = gate length** (button hold / note length), release R.
- **Filter** (shared macro 4280): Multi 2-Pole, `P = Cut + Env · A/E(FilterEnv)`, Res, out = Selector(Filter type)
  over HP/BP/LP. Filter Env (macro 6148) = ADSR(gate, sid91/92/102/103). A/E = control-rate sampling.
- **DC blocker**: HP/LP 1-Pole #21099, P = Constant 1.0 → **8.66 Hz**, **HP** output used (out 0).
- **Level** (sid24, −60..−28 dB) → Amp/Mixer.
- **Delay** (shared macro 4359): mono. Time = `Delay st × 250/BPS` ms (16th notes) + fine (0..75 ms);
  feedback path filtered by Multi 2-Pole (P Cutoff, Reson, HP/BP/LP) × Feed; output = Selector(Pos = WET) of
  [dry, dry + delayed] (**GUESS** scanner = dry + WET·wet). WET = 0 in 15 of 16 slots.

### 2.6 Randomizer macros "50-59" … "110-119" (7 macros)
- Each macro: `In(>>)` ─► Separator(Thld unwired = 0) ─Hi─► 10× Randomizer(Rng = 0.5) ─► 10× **Controller**
  (MIDI CC, Min 0 Max 1 → 0..127). CC numbers read from bytes: macro "50-59" sends CC 50..59, etc. (CC 50..119).
- Input = M#6145 = **Randomize button (0.55) or note 58 (velocity)**. Only positive events pass, so release does
  not randomize. Each CC gets `v + U(−0.5, 0.5)` (v = 0.55 → 0.05..1.05, clipped) → **≈ uniform over the
  control's normalised range** (from velocity v when played by note 58).
- Which controls listen: a byte field in each panel control (12 bytes after its panel index) reads as a CC
  number. On faders/lists it gives the table below (**strong GUESS**: 14 contiguous CCs = exactly the 14 voice
  controls). The same field reads 50..56 on preset buttons 8..14 of every instrument and 57 on QUAD on/off, so on
  buttons it is probably something else; the "MIDI in active" flag was not decoded.

| CC | control | CC | control |
|---|---|---|---|
| 50 | Osc wave (sid74) | 57 | LFO wave (sid65) |
| 51 | **P** (sid0, Fader 4218) | 58 | BPMRate (sid10) |
| 52 | PW (sid1) | 59 | Width (sid7) |
| 53 | + (sid2) | 60 | Amp (sid8) |
| 54 | NoteRnd/Manual (sid64) | 61 | Filter type (sid66) |
| 55 | RndmRate (sid3) | 62 | Cut (sid39) |
| 56 | randomness (sid4) | 63 | Res (sid40) |

  No fader/list reads CC 64..119. The route Controller → instrument MIDI out → back into CRAX's panel is a **GUESS**
  (Reaktor internal MIDI loopback). Other instruments also have Cut on CC 62 (K1, NOIX, STOMPS) and QUAD
  on/off maybe CC 57 — whether Randomize also hits them depends on the ensemble MIDI routing (open question).
- The same press also fires the voice gate (0.55) and re-rolls the NoteRnd randoms. Event order between the
  randomizers and M#4234 is not decodable; **GUESS**: randomize first, then gate.
- SC/Oceanode: implement Randomize host-side (C++): for each of the 14 controls set a uniform random
  normalised value in [0.05, 1] (list/selector controls → random entry), then fire `trig` at 0.55.

### 2.7 CRAX panel controls

| label | sid | range (step) | DSP mapping | file default (DECODED saved; not a preset) |
|---|---|---|---|---|
| P | **0** (Fader 4218, sid 0 missing from DECODED) | 0..5000 Hz (10) | Core P = P·(1+Amp·LFO) | 2500 |
| PW | 1 | −1..1 (0.02) | Core PW (Manual mode) | 0 |
| + | 2 | −5000..5000 Hz (100) | Core p+ (Hz offset) | 0 |
| RndmRate | 3 | 0..10000 Hz (2.268) | Core clkhz (Manual) | 5000 |
| randomness | 4 | 0..1 (0.01) | Core rnd (Manual): depth ×5000 Hz | 0.5 |
| Trigger | 5 | button 0/1 | gate stream | 0 |
| Width | 7 | −1..1 (0.02) | LFO W (pulse width) | 0 |
| Amp | 8 | 0..1 (0.01) | LFO → P depth | 0.5 |
| BPMRate | 10 | 0..8 (0.125) | LFO cycles per beat | 4 |
| Level | 24 | −60..−28 dB (0.267) | Amp/Mixer | −44 |
| Amp EnV A / D / S / R | 33 / 34 / 104 / 105 | −20..100, −20..100, 0..1, 0..80 | ADSR → Core A | 40 / 40 / 0.5 / 40 |
| Cut / Res | 39 / 40 | 20..120 st (1) / 0..0.98 | Multi 2-Pole P / Res | 70 / 0.49 |
| Filter type | 66 | List HP/BP/LP | filter output | – |
| Filter Env A / D / S / R | 91 / 92 / 102 / 103 | as amp env | Filter Env ADSR | 40 / 40 / 0.5 / 40 |
| Env | 106 | −120..120 st (2) | filter env depth (semitones) | 0 |
| NoteRnd/Manual | 64 | List (NoteRnd=1, Manual=0) | Relay Ctl | – |
| LFO wave | 65 | List Saw/SawI/Pls/Tri/Sin/Rnd | LFO selector | – |
| Osc wave | 74 | List SAW/PLS/TRI/SIN | osc selector | – |
| Feed / Delay st / P Cutoff / Reson / type / fine / WET | 67 / 68 / 69 / 70 / 71 / 72 / 73 | 0..1, 0..16 16ths, 20..120, 0..0.98, HP/BP/LP, 0..75 ms, 0..1 | Delay macro | 0.5 / 4 / 70 / 0.49 / – / 37.5 / 0.5 |
| Randomize | 76 | button, On value 0.55 | §2.6 + gate 0.55 | 0.55 |
| preset 1..14 | 77..90 | buttons, On value n | §1 | 0 |
| scope Y/X/X Scale/ExtT/Freeze | 19..23 | display only | – | – |

### 2.8 Draft SynthDef (CRAX)

**`trig` contract (differs from `k1_stomps.md`)**: here `trig` is an audio-rate *held level*, not an impulse plus
`gateLen`: >0 = gate on with peak = value (1 preset/Trigger, 0.55 Randomize, velocity on note 58), back to 0 = gate
off. CRAX needs this because the sound lasts as long as the gate (D = 100) and the release event itself re-rolls
the NoteRnd randoms and resets the LFO. QUAD only needs the rising edge plus R if the gate drops early, so an
impulse + `gateLen` host would also work for it. SynthDef defaults = slot 6 of each bank (the screenshot presets);
list args are 0-based entry values (snapshot int − 1, except NoteRnd/Manual: `noteRnd` = 1 for NoteRnd).
```supercollider
~rkTime = { |x| 10 ** (x / 20) / 1000 };

SynthDef(\bitsCrax, { |out = 0, tempo = 2 /*BPS*/,
        pHz = 1870 /*panel P*/, pw = 0.92, plus = 2000, rndmRate = 10000, randomness = 1, noteRnd = 0 /*0 Manual, 1 NoteRnd*/,
        width = -0.22, amp = 1, bpmRate = 5, lfoWave = 0 /*0..5*/, oscWave = 1 /*0..3*/,
        ampA = -20, ampD = 100, ampS = 0.5, ampR = 40,
        filtType = 0 /*HP (slot 6)*/, cut = 120, res = 0, env = 0, fA = -20, fD = 100, fS = 0.5, fR = 40,
        feed = 0.7, delaySt = 2, pCutoff = 92, pReson = 0, pType = 0, fine = 0, wet = 0,
        level = -40|
    // trig: audio-rate gate level (Reaktor event stream): >0 = on with peak = value, 0 = off.
    var trig = \trig.ar(0);
    var edge = Changed.ar(trig);                 // every event incl. release (GUESS: repeated equal values are missed)
    var gate = trig > 0;
    var peak = Latch.ar(trig, trig > 0);
    // NoteRnd randoms (cells 4238/4239/4244), re-rolled on every event
    var r1 = TRand.ar(-1, 1, edge), r2 = TRand.ar(-1, 1, edge), r3 = TRand.ar(-1, 1, edge);
    var useR = noteRnd > 0;
    var rnd   = Select.ar(useR, [K2A.ar(randomness), r1.abs]);
    var clkhz = Select.ar(useR, [K2A.ar(rndmRate), r2.abs * 44100]);
    var pwv   = Select.ar(useR, [K2A.ar(pw), r3]).clip(-1, 1);
    // MultiWave LFO (tempo-synced, reset by every gate event)
    var ph = Phasor.ar(edge, tempo * bpmRate * SampleDur.ir, 0, 1, Latch.ar(trig.clip(0, 1).wrap(0, 1), edge));
    var p = ph - 0.5;
    var wrap = (HPZ1.ar(ph) < 0) + edge;
    var lfo = Select.ar(lfoWave, [
        2 * p, -2 * p,
        (p >= (width * 0.5)).linlin(0, 1, 1, -1),     // Pls polarity GUESS
        1 - (4 * p.abs),
        cos(2pi * p),
        TRand.ar(-1, 1, wrap)]);
    // Core: F in Hz, clipped 0..Nyquist (negative -> frozen phase)
    var f = (plus + (pHz * (1 + (amp * lfo))) + (rnd * 5000 * LFNoise0.ar(clkhz))).clip(0, SampleRate.ir * 0.5);
    var aEnv = EnvGen.ar(Env.adsr(~rkTime.(ampA), 5 * ~rkTime.(ampD), ampS, 5 * ~rkTime.(ampR), 1, [0, -5, -5]), gate) * peak;
    var fEnv = EnvGen.kr(Env.adsr(~rkTime.(fA), 5 * ~rkTime.(fD), fS, 5 * ~rkTime.(fR), 1, [0, -5, -5]), A2K.kr(gate)) * A2K.kr(peak);
    var live = f > 0.01, fs = f.max(0.01);
    var sig = Select.ar(oscWave, [
        Select.ar(live, [DC.ar(1), Saw.ar(fs)]),                 // frozen-phase DC: GUESS
        Select.ar(live, [DC.ar(1), Pulse.ar(fs, (pwv + 1) * 0.5)]),
        Select.ar(live, [DC.ar(0), LFTri.ar(fs)]),
        Select.ar(live, [DC.ar(0), SinOsc.ar(fs)])]) * aEnv;
    sig = ~bitsFilter.(sig, filtType, cut + (env * fEnv), res);            // shared macro
    sig = sig - OnePole.ar(sig, exp(-2pi * 8.66 * SampleDur.ir));           // HP 1-Pole, P = 1
    sig = sig * level.dbamp;
    sig = ~bitsDelay.(sig, 1, delaySt, fine, feed, pCutoff, pReson, pType, wet);  // shared, always on
    Out.ar(out, sig ! 2);                                                    // mono to L and R
}).add;
```
No per-sample feedback is needed (LFNoise0 = Clk Gen + Random exactly; Phasor = LFO phase).

## 3. QUAD — Quadratic Iterator (object 3069)

### 3.1 Signal flow (exact)
```
sel = Selector(c source)[ var (manual~) | Rnd@gate-on (random~) | ramps macro ]       (−1..1)
c   = AudioSmoother_2ms(0.9221 + 0.0779·sel)                                           (0.8442..1)
xL  = Core 3074 "Logistic Map Integrator"(c, c offset = AudioSmoother_2ms(offset L/R))
xR  = Core 3082 "Logistic Map Integrator"(c, c offset = 0 (unconnected))
env = ADSR(G = gate, A sid21, D sid20, S = UNCONNECTED = 0, R sid22)
L = xL·env·env ; R = xR·env·env         (X3078 then X3209: the envelope is applied twice = squared)
  ─► Voice Combiner ─► Amp/Mixer (Level dB, both) ─► Micro Space (L, R) ─► Voice Combiner ─► Out L / Out R
XY display: X = xL·env, Y = xR·env (Lissajous, Y range 0..0.65)
```
**Stereo.** No DC blocker anywhere in QUAD (the iterator output has a mean of ≈ +0.2).

### 3.2 Core 3074/3082 "Logistic Map Integrator" (55 modules) — recurrence
Netlist: `r = 1 + 1.55·c + 0.01·cOffset`; `equacions: axnew = (ax·r)·(1 − ax)`; `ax = AudioMix(0.5, z⁻¹(axnew))`
= **0.5 + x[n−1]** (the 0.5 constant is latched into an audio signal and summed with the one-sample delay);
the cell runs at the audio sample rate ("legacy R5 SR for AudioCell"). Hence

```
x[n] = r · (0.5 + x[n−1]) · (0.5 − x[n−1]) = r · (0.25 − x[n−1]²)        x[0] = 0 → x[1] = r/4
```
— a quadratic map (equivalent to `z' = μ − z²` with z = r·x, μ = r²/4 ∈ 1.33..1.63), **not** the plain logistic
map (which at r = 2.31..2.56 would settle to a silent fixed point). Simulated (Python, 20 k samples):

| sel | r | behaviour | range |
|---|---|---|---|
| −1 | 2.309 | period 4 (11025 Hz at 44.1 k) | −0.19..0.58 |
| −0.53 | 2.365 | period 16 | −0.24..0.59 |
| 0 | 2.429 | period 6 | −0.29..0.61 |
| +0.54 | 2.494 | chaotic | −0.35..0.62 |
| +1 | 2.550 | period 5 (windows) | −0.40..0.64 |

So `c` sweeps the iterator through its period-doubling cascade, periodic windows and chaos: pitched buzz ↔ noise.
L and R differ only by `0.01·offset` in r, which decorrelates them in chaotic regions (identical in periodic ones
once both lock).

**SC implementation**: one-sample feedback, so LocalIn/LocalOut at block size is **not** equivalent. No custom
UGen needed: the built-in **`QuadN.ar(freq, a, b, c, xi)`** iterates `x' = a·x² + b·x + c`:
`QuadN.ar(44100, r.neg, 0, r * 0.25, 0)`. **Sample-rate dependence**: one iteration per sample, so every pitch scales
with SR. Reaktor ran at 44.1 kHz (**GUESS**; CRAX hard-codes 44100 too): use `freq: 44100` to keep the pitches at any
server rate (QuadN holds between iterations, like a 44.1 k zero-order hold). `freq: SampleRate.ir` is exact only at 44.1 k.
The map runs continuously from load (state never reset by the gate).

### 3.3 c sources
- **manual~**: `sel = var` (sid23, −1..1).
- **random~**: Core 3104 = library Random (±1), clocked by positive gate events only (Separator Thld 0):
  `sel = TRand(−1, 1, gate rising)`, held.
- **ramps** (macro 25534):
  - `F = −BPS / Beat L` (Invert −x then x/y): **negative**, period = Beat L beats (0.5..16) → tempo-synced.
  - Three sources, each synced (restart) by positive gate events (`Snc` = M#3197):
    0 = Ramp Osc (A = 1, 0..1; with negative F it **falls** 1→0, **GUESS**), 1 = LFO Sin·0.5 + 0.5 (0..1; runs
    backward), 2 = `Core 25552` = |Random| clocked by a Clock Oscillator(A=1, W=0) which emits on both edges →
    new value every **half** period.
  - Selector Pos = SAW/SIN/RND fader (sid80, 0..2 step 1; snapshots store 1.18 → stepped 1 = SIN).
    Selector mode: floor; if it is actually a scanner, 1.18 = 82 % SIN + 18 % RND (**GUESS**: selector).
  - `x` = A/E (control rate) of that 0..1 signal; `ramps = Co + x·(Ca − Co)` — Selector 25536 with Pos = x,
    inputs Co (0) / Ca (1). Only meaningful as a **Scanner** (linear crossfade); curve property not decodable,
    and its header bytes match the selector-mode modules, so **GUESS: scanner**.
  - Meter = 1 − x (display only).

### 3.4 Envelope and level
ADSR with S = 0: an AD burst, `A` 1..80, `D` 0..80, `R` 1..80 (log times; R only matters if the gate drops before
A+D ends). Peak = gate value (1). Squared in the signal path. Level sid18 (−60..0 dB) on both channels, then the
shared Micro Space (Size, Position, Mod, Lo, Hi, Mix, two "on" switches sid60/sid68 for its two cores).

### 3.5 QUAD panel controls

| label | sid | range (step) | DSP mapping | file default (DECODED saved; not a preset) |
|---|---|---|---|---|
| on/off | 2 | button 0/1 (GUESS toggle) | gate stream | 0 |
| offset L/R | 3 | 0..1 (1/127) | L cell `c offset`, r += 0.01·offset | 0.5 |
| XY (display) | 10 | 0..0.65 | display only | – |
| Level | 18 | −60..0 dB (0.5) | Amp/Mixer L and R | −30 |
| D / A / R | 20 / 21 / 22 | 0..80 / 1..80 / 1..80 | ADSR (S = 0) | 40 / 40.5 / 40.5 |
| var | 23 | −1..1 (0.02) | manual c control | 0 |
| Micro Space Size/Mod/Lo/Hi/Position/Mix | 54/55/56/57/58/59 | 0..100, 0..1, ±12 dB, ±12 dB, 0..1, 0..1 | shared macro | 50 / 0.5 / 0 / 0 / 0.5 / 0.5 |
| Micro Space on, on | 60, 68 | buttons | shared macro | 0, 0 |
| Co / Ca | 77 / 78 | −1..1 (0.0074) | ramps range ends | 0 / 0 |
| Beat L | 79 | 0.5..16 beats (0.5) | ramps period | 8.25 |
| SAW/SIN/RND | 80 | 0..2 (1) | ramps shape | 1 |
| c source (manual~/random~/ramps) | 81 | List 0/1/2 | Selector 25558 | – |
| preset 1..14 | 40..53 | buttons | §1 | 0 |

### 3.6 Draft SynthDef (QUAD)
```supercollider
SynthDef(\bitsQuad, { |out = 0, tempo = 2 /*BPS*/,
        offsetLR = 0.6772, level = -36, atk = 1, dec = 43, rel = 52, cVar = 0.14 /*panel "var" (SC keyword)*/,
        co = -0.5333, ca = 0.5407, beatL = 16, shape = 1 /*0 SAW 1 SIN 2 RND*/, cSource = 1 /*0 man 1 rnd 2 ramps*/,
        size = 77, position = 1, mod = 1, msOn = 1, lo = -12, hi = 0, mix = 0.38|
    var trig = \trig.ar(0);
    var gate = trig > 0;
    var rise = T2K.kr(Trig1.ar(gate, SampleDur.ir));          // positive events only (T2K keeps sub-block pulses)
    var peak = Latch.ar(trig, trig > 0);
    var rate = tempo / beatL;                                  // Hz (Reaktor F is the negative of this)
    var ramp = Phasor.kr(rise, rate.neg * ControlDur.ir, 0, 1, 0);           // falling ramp, GUESS
    var sine = sin(2pi * Phasor.kr(rise, rate.neg * ControlDur.ir, 0, 1, 0)) * 0.5 + 0.5; // backward sine, restart phase 0
    var rnd  = TRand.kr(0, 1, Impulse.kr(rate * 2, 0) + rise);               // both clock edges
    var x = Select.kr(shape.floor.clip(0, 2), [ramp, sine, rnd]);
    var ramps = co + (x * (ca - co));                                        // scanner GUESS
    var sel = Select.kr(cSource, [cVar, TRand.kr(-1, 1, rise), ramps]);
    var c = Lag.kr(0.9221 + (0.0779 * sel), 0.002);           // Audio Smoother 2 ms (Lag ≈ ramp)
    var rL = 1 + (1.55 * c) + (0.01 * Lag.kr(offsetLR, 0.002));
    var rR = 1 + (1.55 * c);
    var xs = [QuadN.ar(44100, rL.neg, 0, rL * 0.25, 0), QuadN.ar(44100, rR.neg, 0, rR * 0.25, 0)];
    var env = EnvGen.ar(Env.adsr(~rkTime.(atk), 5 * ~rkTime.(dec), 0, 5 * ~rkTime.(rel), 1, [0, -5, -5]), gate) * peak;
    var sig = xs * env.squared * level.dbamp;                 // no DC blocker (faithful)
    sig = ~bitsMicroSpace.(sig, size, position, mod, msOn, lo, hi, mix);     // shared, stereo in/out
    Out.ar(out, sig);
}).add;
```

## 4. Snapshot banks
Bank 1, file order (slot n = row/column n, **GUESS**). Stepped value, raw in parentheses; Lists by name.
"–" = sid absent from that snapshot (behaviour on recall: **probably left unchanged**, open question).
CRAX slot 6 "sinebeats 5" and QUAD slot 6 "Snap 6" match the user's panel screenshots.

### CRAX bank

| sid | control | 1 sinebeats | 2 sinebeats 1 | 3 sinebeats 2 | 4 sinebeats 3 | 5 sinebeats 4 | 6 sinebeats 5 | 7 sinebeats 6 | 8 sinebeats 7 | 9 sinebeats 8 | 10 sinebeats 9 | 11 sinebeats 10 | 12 sinebeats 4 | 13 sinebeats 6 | 14 sinebeats 3 | 15 sinebeats 4 | 16 sinebeats 5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | P (Hz) | 150 | 350 | 2220 (2225) | 1870 (1875) | 1870 (1875) | 1870 (1875) | 2250 | 3200 | 3200 | 5000 | 5000 | 1870 (1875) | 1870 (1875) | 2950 | 350 | 3600 |
| 1 | PW | -0.86 (-0.8425) | 0.84 (0.8583) | 0.84 (0.8425) | 0.06 (0.07087) | 0.92 (0.9213) | 0.92 (0.9213) | 0.48 (0.4961) | 0.86 (0.874) | 0.86 (0.874) | 0.98 (0.9843) | 0.8 (0.811) | 0.06 (0.07087) | 0.92 (0.9213) | 0.62 (0.6378) | -0.88 (-0.874) | -0.24 (-0.2283) |
| 2 | + | -2600 (-2559) | 2100 (2165) | -1200 (-1142) | -1300 (-1299) | 2000 (2008) | 2000 (2008) | -1000 (-984.3) | -4900 (-4843) | -4900 (-4843) | -5000 | -2100 (-2087) | -3200 (-3189) | 2000 (2008) | 3900 (3976) | 500 (511.8) | -2800 (-2795) |
| 3 | RndmRate | 10000 | 10000 | 1500 | 1500 | 9450 | 10000 | 1350 | 150 | 9300 | 10000 | 10000 | 2600 | 10000 | 0 | 10000 | 900 |
| 4 | randomness | 0.76 (0.7638) | 1 | 0.06 (0.06299) | 1 | 1 | 1 | 0.59 (0.5906) | 1 | 1 | 1 | 0.12 (0.126) | 1 | 1 | 0 | 0.74 (0.7402) | 0.07 (0.07087) |
| 5 | Trigger btn (raw) | 0 | 0 | 0 | 0 | 0.5 | 0.5 | 0.5 | – | – | – | – | – | – | – | – | – |
| 7 | Width | -0.02 (-0.007874) | -0.34 (-0.3228) | -0.22 (-0.2126) | -0.22 (-0.2126) | -0.22 (-0.2126) | -0.22 (-0.2126) | 0.48 (0.4803) | 1 | 1 | 1 | 1 | -0.22 (-0.2126) | -0.22 (-0.2126) | -0.16 (-0.1496) | 0.26 (0.2756) | -0.42 (-0.4016) |
| 8 | Amp | 1 | 0 | 0.11 (0.1102) | 0.8 (0.8031) | 1 | 1 | 0.42 (0.4252) | 1 | 1 | 1 | 0 | 0 | 1 | 1 | 0.52 (0.5276) | 0.54 (0.5433) |
| 10 | BPMRate | 0 | 2 (2.016) | 8 | 1.5 (1.512) | 4 (4.031) | 5 (5.039) | 3.375 (3.465) | 6.75 (6.803) | 6.75 (6.803) | 6.75 (6.803) | 4.75 (4.787) | 1.5 (1.512) | 5 (5.039) | 8 | 1.75 (1.764) | 4.375 (4.472) |
| 24 | Level dB | -40.53 (-40.35) | -31.47 (-31.28) | – | – | – | – | – | – | – | – | – | – | – | – | – | – |
| 33 | AmpEnv A | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 |
| 34 | AmpEnv D | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 30 | 100 | 100 | 100 |
| 104 | AmpEnv S | – | 0.06 (0.06299) | – | – | 0.06 (0.06299) | – | – | – | – | – | – | – | – | 0.06 (0.06299) | – | – |
| 105 | AmpEnv R | – | 30 (30.87) | – | – | 30 (30.87) | – | – | – | – | – | – | – | – | 30 (30.87) | – | – |
| 39 | Cut | 120 | 120 | 120 | 120 | 120 | 120 | 120 | 120 | 20 | 20 | 20 | 88 (88.5) | 120 | 120 | 120 | 120 |
| 40 | Res | 0.52 (0.5247) | 0.52 (0.5247) | 0.52 (0.5247) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.3 (0.3087) | 0.3 (0.3087) | 0.3 (0.3087) |
| 66 | Filter type | LP | LP | LP | LP | LP | HP | LP | HP | HP | HP | HP | LP | HP | LP | LP | LP |
| 91 | FiltEnv A | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 | -20 |
| 92 | FiltEnv D | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 30 | 100 | 100 | 100 |
| 102 | FiltEnv S | – | 0.22 (0.2205) | – | – | 0.22 (0.2205) | – | – | – | – | – | – | – | – | 0.22 (0.2205) | – | – |
| 103 | FiltEnv R | – | 51 (51.65) | – | – | 51 (51.65) | – | – | – | – | – | – | – | – | 51 (51.65) | – | – |
| 106 | Env | – | 120 | – | – | 120 | – | – | – | – | – | – | – | – | 120 | – | – |
| 64 | NoteRnd/Manual | Manual | NoteRnd | Manual | Manual | Manual | Manual | Manual | NoteRnd | Manual | Manual | Manual | Manual | Manual | Manual | Manual | Manual |
| 65 | LFO wave | Saw | Saw | Saw | Saw | Saw | Saw | SawI | Saw | Saw | Saw | Sin | Saw | Saw | Pls | Tri | Rnd |
| 74 | Osc wave | PLS | PLS | PLS | PLS | PLS | PLS | SAW | PLS | PLS | PLS | PLS | PLS | – | SIN | SIN | TRI |
| 67 | Feed | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.7 (0.7008) | 0.9 (0.9213) |
| 68 | Delay st | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 0 |
| 69 | P Cutoff | 92 (92.44) | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 | 92 |
| 70 | Reson (delay) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 71 | Delay filt type | HP | HP | HP | HP | HP | HP | HP | HP | HP | HP | HP | HP | HP | HP | HP | HP |
| 72 | fine | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 30.12 |
| 73 | WET | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.55 (0.5512) | 0 | 0 | 0 |
| 19–23 | scope Y / X / X Scale / ExtT / Freeze | 4.5 / 5 / x1 / ext / 0 in all slots (display only) | | | | | | | | | | | | | | | |

(RndmRate is shown rounded: its step 2.268 Hz floors 10000·n to e.g. 9999.)

### QUAD bank

| sid | control | 1 Snap 1 | 2 Snap 2 | 3 Snap 3 | 4 Snap 4 | 5 Snap 5 | 6 Snap 6 | 7 Snap 7 | 8 Snap 8 | 9 Snap 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | on/off btn (raw) | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | 0.5 | – | – | – |
| 3 | offset L/R | 0.4567 | 0.4567 | 0.5984 | 0.5984 | 0.2441 | 0.6772 | 0.6772 | 0 | 0.7008 |
| 18 | Level | -34 (-33.54) | -34 (-33.54) | -34 (-33.54) | -34 (-33.54) | -36 (-35.91) | -36 (-35.91) | -36 (-35.91) | -36 (-35.91) | -36 (-35.91) |
| 21 | A | 25.69 (25.88) | 33.59 (33.97) | 33.59 (33.97) | 1 | 27.66 (27.75) | 1 | 1 | 1 | 1 |
| 20 | D | 13 (13.86) | 80 | 80 | 80 | 80 | 43 (43.46) | 52 (52.28) | 80 | 80 |
| 22 | R | 42.48 (43.3) | 42.48 (43.3) | 42.48 (43.3) | 1 | 29.64 (30.24) | 52.35 (53.25) | 1 | 1 | 1 |
| 23 | var | -0.38 (-0.3701) | -0.38 (-0.3701) | -0.38 (-0.3701) | -0.38 (-0.3701) | 0.08 (0.08661) | 0.14 (0.1496) | 0.14 (0.1496) | 0.14 (0.1496) | 0.14 (0.1496) |
| 81 | c source | random~ | random~ | manual~ | random~ | manual~ | random~ | ramps | ramps | ramps |
| 77 | Co | -0.5333 (-0.5276) | -0.5333 | -0.5333 | -0.5333 | -0.5333 | -0.5333 | -1 | -0.04444 (-0.03937) | -1 |
| 78 | Ca | 0.5407 (0.5433) | 0.5407 | 0.5407 | 0.5407 | 0.5407 | 0.5407 | 1 | 0.9037 (0.9055) | 1 |
| 79 | Beat L | 16 | 16 | 16 | 16 | 16 | 16 | 1 (1.476) | 2 (2.331) | 0.5 |
| 80 | SAW/SIN/RND | 1 SIN (1.181) | 1 (1.181) | 1 (1.181) | 1 (1.181) | 1 (1.181) | 1 (1.181) | 0 SAW | 0 SAW | 2 RND |
| 54 | Size | 65 (65.35) | 65 | 65 | 65 | 65 | 77 (77.95) | 77 (77.95) | 66 (66.93) | 66 (66.93) |
| 55 | Mod | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 56 | Lo | 0 (0.09449) | 0 | 0 | 0 | 0 | -12 | -12 | -12 | -12 |
| 57 | Hi | 0 (0.09449) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 58 | Position | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 59 | Mix | 0.38 (0.3858) | 0.38 | 0.38 | 0.38 | 0.38 | 0.38 | 0.38 | 0.38 | 0.38 |
| 60 | Micro Space on (raw) | 0.5 | 0 | 0 | 0 | 0.5 | 0.5 | 0 | 0 | 0 |
| 68 | Micro Space on (raw) | 0.5 | 0 | 0 | 0 | 0.5 | 0.5 | 0 | 0 | 0 |

## 5. Open questions
1. Snapshot slot ↔ file-order block mapping (no slot index found in the block header). CRAX: which 14 of the 16 blocks are slots 1..14.
2. Sids absent from a snapshot (CRAX 102–106, 24, 5, 74): left unchanged on recall (assumed) or reset?
3. Button encoding: snapshots store 0.5 for "on". Does recalling QUAD on/off = 0.5 fire a gate event (peak 0.5?)
   before the preset's own gate 1? Is CRAX Trigger a trigger-mode button (press 1 / release 0)?
4. Randomize CC loopback: is the decoded byte field really the MIDI-learn CC (and is MIDI-in enabled)? Does
   Controller → MIDI out reach CRAX's own controls, and other instruments (Cut CC 62 in K1/NOIX/STOMPS; the
   same field reads 50..56 on every instrument's preset buttons 8..14 and 57 on QUAD on/off)? Event order randomize vs gate.
5. QUAD Selector 25536 (Co/Ca) and Selector 25556 (SAW/SIN/RND): scanner or selector mode (curve property not decoded).
6. Ramp Osc / LFO / Clock Oscillator with negative frequency (−BPS/BeatL): falling ramp and backward sine assumed.
7. Reaktor sample rate when the presets were made (iterator pitches and CRAX's ×44100 scale assume 44.1 kHz).
8. MultiWave Osc frozen-phase DC value at F ≤ 0 (CRAX slots 8–10 sit below 0 Hz much of the time).
