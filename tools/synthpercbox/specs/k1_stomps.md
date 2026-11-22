# Bits.ens — K1 and STOMPS voices (decode spec)

Sources: `~/Downloads/Bits_r6.ens`, extractor report `r6.json` (REPORT), `bits_decoded.json` (DECODED).
K1 = Instrument object 24044, STOMPS = Instrument object 2051. Everything below was read from the wires
(`dump_macro.py`, raw `connections` indices) and Core cells (`core_view.py`); anything not read directly is marked **GUESS**.

## 0. Conventions used for both instruments

- **Snapshot values are normalised** 0..1 (f32, mostly on a 1/127 grid). Value = `e1 + n·(e2−e1)`, and the
  fader then snaps to its step grid. The panel readouts fit **floor to step** (with ε): K1 "Decay" n=0.9606 →
  600−96·5.99 = **24.96** (panel 24.96), Feed n=0.6693 → **0.6** (panel 0.6, rounding would give 0.7),
  Base pitch 0.2598·127 = **33**, STOMPS pw 0.811 → **0.62**, base F 0.132·250 = **33**. The step rule is a
  **GUESS** that fits all panel readouts; tables give stepped value and (raw) value when they differ.
- **DECODED `saved` values are not panel values** (almost all are the range midpoint, norm 0.5); ignore them. Use the snapshots.
- **Lists / Switch** store a 1-based index in snapshots: 1 = HP, 2 = BP, 3 = LP (List entries in the file:
  HP=0, BP=1, LP=2 → Selector Pos; Multi 2-Pole outputs HP/BP/LP go to Selector inputs 0/1/2). Confirmed by
  the panels: K1 Delay menu shows LP (stored 3), STOMPS Delay menu shows BP (stored 2).
- **Buttons (toggles)** store 0.5 when on, 0 when off. **GUESS: on = 1.0** in the DSP (range 0..1, On value 1).
  If Reaktor really outputs 0.5 the Penv contributions would halve — check by ear.
- **Reaktor log times** (ADSR A/D/R, D-module decay): `t_ms = 10^(x/20)` (0 = 1 ms, 20 = 10 ms, 40 = 100 ms,
  60 = 1 s, 80 = 10 s; documented for ADSR, **GUESS** for the D module). Project rule from AnimaBox
  (memory): Reaktor decay/release times are **time constants τ** (segment ≈ 5τ, curve −5). **GUESS** for D too.
- **Pitch P inputs** (Multi 2-Pole cutoff, Expon. (F)): semitones, `Hz = 440·2^((P−69)/12)` (84 = 1047 Hz).
- **Level** faders feed Stereo Amp/Mixer Lvl in **dB** (range −50..−20 dB, so max gain 0.1). Pan −1..1.
- **Voice count**: not in the report. Audio Voice Combiners are present (standard boilerplate); all gate
  sources are monophonic (Button, Sel. Note Gate). **GUESS: 1 voice** (retrigger restarts the same voice).

## 1. Triggering (identical scheme in both instruments)

```
Sel. Note Gate (note k) ─► Separator(Thld 0) ─Hi─► Value(n) ─┐            (n = 1..14)
                                         └─Lo─► (0) ───────┤ M ─► M ─► presets Out
preset Button n (On value n, sid 28..41 K1 / 46..59 STOMPS) ─┘
presets Out ─► Order: (1) Snapshot.Snp = v  (2) Snapshot.Recall (only if v>0)  (3) Compare v>0 ─► gate 1/0
```
- A MIDI note-on of note k (macro names: K1 "MIDI 0-13", STOMPS "MIDI 14-27" → **GUESS** note k = base+n−1)
  or a click on preset button n **recalls snapshot n of bank 1, then sends gate = 1**. Note-off / button
  release sends gate = 0 (no recall: Recall needs a positive event).
- **Velocity is discarded** (Value outputs the constant n; Compare gives exactly 1). Gate peak is always 1.
- The panel trigger button (K1 sid 24, label decoded as "(", STOMPS sid 2 "trigger") is merged into the
  same gate stream: press = 1, release = 0, no snapshot recall.
- **Incoming MIDI pitch affects nothing**: no Note Pitch module exists in either instrument; notes only
  select presets. K1 has 9 snapshots for 14 slots, STOMPS 10 (slots past the bank only trigger).
- Gate *length* matters: K1 filter ADSR (S, R) and STOMPS amp ADSR (S=0, R≈1 ms → note-off cuts) and filter ADSR.

## 2. K1 (object 24044)

### Signal flow (exact)
```
gate ─┬─► Pitch.G  ─► D1(Trig, A=Amt1, D=Decay1)·Penv1 ┐
      │             D2(Trig, A=Amt2, D=Decay2)·Penv2  ┼─ + Log(p2f(BasePitch)) = P [semitones]
      │                                                ┘
      ├─► Ramp Osc 24053 Snc   (F = Expon.(F)(P) Hz, A = 1, Ph unconnected = 0)
      ├─► Base.G  (time ramp reset)
      └─► Filter.G (Filter Env ADSR gate)
Ramp(0..1) ─► Shaper: Sine(x) = sin(2π·x) ─► Base: ×Vol×10^(−"Decay"·t/20) ─► PASS (DRIVE) ─► Filter ─► Delay(L) ─► Stereo Amp(Lvl=Level dB, Pan) ─► out L/R
```
- **Pitch macro 24056**: Core Cell 24072 is plain mtof (`2^((p−69)/12 + log2 440)`), then `Log` (Hz→semitones)
  turns it back into p; it exists only to drive a Hz Meter. So **P = BasePitch + Penv1·D1 + Penv2·D2**,
  all in semitones. Base pitch fader 24059 (sid **0**, 0..127, step 1, unlabelled in DECODED; panel "Base pitch" = 33 = A1, 55 Hz).
- **D modules** (kind 264, "Decay envelope, positive slope at Trig starts it, decay starts immediately"):
  out = A·(decay from 1 to 0), A = Amt (semitones 0..68), D = Decay (0..80 log time). Penv toggles multiply.
  Pitch envelope therefore **adds up to +68 st per envelope** above Base pitch, decaying back to Base pitch.
- **Oscillator**: Ramp Osc 0..1 with **hard sync on the gate** (restarts at phase 0 when gate rises), shaped by
  `Sine` (1.0 = one period) → a sine that **always starts at phase 0** on a trigger. Linear FM by P only.
- **Amp envelope (Base macro 24078)**: Ramp Osc 24084 with F = 1/9001 Hz, A = 9001 → outputs *seconds since
  trigger* (resets on gate, wraps after 9001 s). `amp = Vol · Expon.(A)(−"Decay"·t)` = `Vol·10^(−Decay·t/20)`:
  instant attack, **exponential decay at "Decay" dB per second** (fader 600..1 inverted: n=0 → 600 dB/s,
  n=1 → 1 dB/s). Independent of gate-off. Before the first trigger the ramp runs from load time (silent in practice).
- **Shaper 24074**: only the Sine (no extra waveshaping).
- **PASS 24357 (DRIVE)**: `y = x / (|x| + DRIVE²)` (Rectify → +DRIVE·DRIVE → Divide). DRIVE fader 2..0.1
  (inverted). Small-signal gain 1/DRIVE² (0.25 at 2, 100 at 0.1), output bounded |y|<1. The Clipper(±0.5)
  inside receives the signal but **its output is unwired** (dead).
- **Filter 24091** (shared macro, other agent): Multi 2-Pole, P = Cut + Env·A/E(ADSR(gate)), Res = Reson,
  output selected by List sid43 (HP/BP/LP). ADSR peak = gate value = 1; A/E samples the env at control rate.
- **Delay 24112** (shared): mono in → L out. Time = Delay st × (250 ms / tempo_bps) (= 16th notes) + fine ms.
  "on" (sid42) relays the *input* into the feedback loop; WET scans dry↔delayed; feedback path filtered by
  Multi 2-Pole (P Cutoff, Reson, List sid21) × Feed.
- Output: Stereo Amp/Mixer (Lvl = Level dB, Pan) → Audio Voice Combiners → instrument outs → master `+` (1094/1878).

### Panel controls (K1)
| label (panel) | sid | oid | range (e1→e2, step) | DSP mapping |
|---|---|---|---|---|
| Base pitch | 0 | 24059 | 0→127, 1 | semitones, P base |
| Penv1 Decay | 1 | 24062 | 0→80, 1 | D1 decay, log time `10^(x/20)` ms (τ, GUESS) |
| Penv1 Amt | 2 | 24063 | 0→68, 0.68 | D1 amplitude, semitones |
| Penv2 Decay | 3 | 24065 | 0→80, 1 | D2 decay, as above |
| Penv2 Amt | 4 | 24066 | 0→68, 0.68 | D2 amplitude, semitones |
| Penv1 | 5 | 24067 | toggle 0/1 | × D1 |
| Penv2 | 6 | 24069 | toggle 0/1 | × D2 |
| "Decay" (Base) | 7 | 24089 | 600→1, 5.99 | amp decay rate, dB/s |
| Vol (Base) | 8 | 24090 | 0→1, 0.01 | linear gain |
| Reson (Filter) | 9 | 24095 | 0→0.98, 0.01 | Multi 2-Pole Res |
| Cut | 11 | 24097 | 20→120, 1 | cutoff, semitones |
| Filter Env A | 12 | 24100 | −20→100, 1 | ADSR attack log time |
| Filter Env D | 13 | 24101 | −20→100, 1 | ADSR decay log time |
| Filter Env S | 14 | 24104 | 0→1, 0.01 | sustain factor |
| Filter Env R | 15 | 24105 | 0→80, 1 | ADSR release log time |
| Env | 16 | 24110 | −120→120, 2 | semitones of cutoff per unit env |
| Feed | 17 | 24118 | 0→1, 0.1 | delay feedback |
| Delay st | 18 | 24119 | 0→16, 0.0625 | 16th notes |
| P Cutoff | 19 | 24126 | 20→120, 1 | feedback filter cutoff, semitones |
| Reson (Delay) | 20 | 24127 | 0→0.98, 0.01 | feedback filter Res |
| Delay filter type | 21 | 24129 | List HP/BP/LP | feedback filter output |
| fine | 22 | 24131 | 0→75, 0.5906 | + ms |
| WET | 23 | 24134 | 0→1, 0.0125 | dry↔wet scan |
| trigger button "(" | 24 | 24135 | 0/1 | gate (not in snapshots) |
| Pan | 26 | 24149 | −1→1, 0.02 | pan (not in snapshots) |
| Level | 27 | 24150 | −50→−20, 0.6 | dB |
| preset buttons 1..14 | 28..41 | 24277.. | On value n | recall n + gate |
| Delay on | 42 | 24302 | toggle | relay input into delay |
| Filter type | 43 | 24309 | List HP/BP/LP | filter output |
| DRIVE | 51 | 24361 | 2→0.1, 0.01496 | PASS k = DRIVE² |

## 3. STOMPS (object 2051)

### Signal flow (exact)
```
gate ─► M#2102 ─► Order#24393: (1) Filter.G  (2) Pitch.In (D triggers)  (3) ADSR#2063.G
Pitch 2125: f[Hz] = base F + Higher F · (Penv1·D#2128(A=1, D=Decay sid27) + Penv2·D#2135(A=1, D=Decay sid30))
ADSR#2063: G = gate, A = sid8, D = sid6, S = UNCONNECTED (0), R = sid36  ─► Core Cell input a
Core Cell 2054 (f, pw = sid60, a) ─► outputs sw, pls, "sin" ─► Selector/Scanner#2069 (Pos = Wave sid10, inputs 0/1/2)
 ─► PASS 16776 (DRIVE sid45) ─► Filter 16756 ─► Delay 24369 (L) ─► Stereo Amp (Level sid23 dB, Pan sid21) ─► out L/R
```
- **Pitch is linear in Hz** (not semitones): base F 0..250 Hz plus Higher F (0..2000 Hz) times the sum of
  the two toggled decay envelopes (each 0..1). Both on → peak f = base F + 2·Higher F (or +1·Higher F if
  toggles are really 0.5: see §0). Pairing: **Penv1 ↔ Decay sid27, Penv2 ↔ Decay sid30**.
- **Core Cell 2054 decoded**: it wraps NI's library `MultiWave Osc` (534 modules):
  - F is scaled `F·2/SR`, clipped 0..1 (Nyquist), accumulated into a phase in [−1, 1) that wraps by −2.
    **There is no sync/reset input: the oscillator is free-running** (phase not reset by the trigger).
  - `saw` = the phase (rising −1→1) + an anti-aliasing correction at the wrap ("Transition" macros =
    polyBLEP-style). `pulse` uses pw clipped to −1..1 (**GUESS** duty = (1+pw)/2, pw 0 = square), also
    anti-aliased. `tri` = 2·(...) built from the saw phase, anti-aliased. A parabolic `sine` is computed but
    **unused**.
  - Outputs: `sw = saw·a`, `pls = pulse·a`, **`sin = tri·a`** (the port named "sin" carries the triangle!).
    `pw·1000` is computed and unused. So the amp ADSR is applied inside the cell, before the selector.
- **Wave** (0→3, step 0.0375): Pos into a 3-input Selector/Scanner (0 = saw, 1 = pulse, 2 = tri). Snapshots
  use 1.5, 1.69, 1.39, 3.0, so the mode matters. The Curve property is not decodable from the bytes.
  **GUESS: Scanner (linear crossfade)**, Pos clipped to 0..2 (3.0 = pure tri). Selector-mode alternative:
  floor(Pos) (1.5 → pulse).
- **Amp ADSR #2063**: S input unconnected → **S = 0** (Constant 2165 = 1.0 drives nothing). With A = 0
  (1 ms) in every snapshot and R at its minimum 1 (≈1.1 ms) in every snapshot, it is an AD envelope that
  is **cut off in ~1 ms at gate-off** (note-off / button release). Gate length therefore shortens sounds.
- **f = 0 case** (snapshots 9, 10: base F 0, Higher F 0): the phase freezes, so the oscillator outputs a
  **DC level** (wherever the phase stopped, in [−1, 1]) × ADSR → a thump/click shaped by the HP filter.
  The DC value is effectively random in Reaktor; **GUESS** use a fixed DC (e.g. saw = 1, pulse = ±1, tri = value at phase 0).
- **PASS**: same law as K1, `x/(|x| + DRIVE²)`.
- **Filter 16756**: like K1 but the type selector is a **Switch** (sid38, label decoded as "6", options HP/BP/LP,
  1-based in snapshots). P = Cut + Env·A/E(ADSR(gate)).
- **Delay 24369**: same macro as K1. Note STOMPS uses **Delay st = 0 and fine = 3.54 ms with Feed 0.9** in
  every snapshot, so it acts as a **comb resonator at ≈ 282 Hz** when on (slots 2 and 10), filtered by P Cutoff.

### Panel controls (STOMPS)
| label (panel) | sid | oid | range (e1→e2, step) | DSP mapping |
|---|---|---|---|---|
| trigger | 2 | 2058 | 0/1 | gate (not in snapshots) |
| D (amp) | 6 | 2064 | 0→80, 1 | ADSR decay log time |
| A (amp) | 8 | 2065 | 0→80, 1 | ADSR attack log time |
| Wave | 10 | 2070 | 0→3, 0.0375 | selector Pos (saw/pulse/tri) |
| Pan | 21 | 2109 | −1→1, 0.02 | pan |
| Level | 23 | 2116 | −50→−20, 0.6 | dB |
| Decay (Penv1) | 27 | 2130 | 0→80, 1 | D#2128 log time |
| Higher F | 28 | 2132 | 0→2000, 20 | Hz per unit env |
| base F | 29 | 2134 | 0→250, 1 | Hz |
| Decay (Penv2) | 30 | 2137 | 0→80, 1 | D#2135 log time |
| Penv2 | 31 | 2140 | toggle | × D#2135 |
| Penv1 | 32 | 2142 | toggle | × D#2128 |
| R (amp) | 36 | 3068 | 1→80, 0.9875 | ADSR release log time |
| Reson | 37 | 16760 | 0→0.98, 0.01 | filter Res |
| Filter type ("6") | 38 | 16761 | Switch HP/BP/LP | filter output |
| Cut | 39 | 16762 | 20→120, 1 | semitones |
| Filter Env A | 40 | 16765 | −20→100, 1 | log time |
| Filter Env D | 41 | 16766 | −20→100, 1 | log time |
| Filter Env S | 42 | 16769 | 0→1, 0.01 | sustain |
| Filter Env R | 43 | 16770 | 0→80, 1 | log time |
| Env | 44 | 16775 | −120→120, 2 | semitones per env unit |
| DRIVE | 45 | 16780 | 2→0.1, 0.01496 | PASS k = DRIVE² |
| preset buttons 1..14 | 46..59 | 21373.. | On value n | recall n + gate |
| pw | 60 | 24368 | −1→1, 0.02 | pulse width |
| Feed | 61 | 24375 | 0→1, 0.1 | delay feedback |
| Delay st | 62 | 24376 | 0→16, 0.0625 | 16ths |
| P Cutoff | 63 | 24383 | 20→120, 1 | semitones |
| Reson (Delay) | 64 | 24384 | 0→0.98, 0.01 | Res |
| Delay filter type | 65 | 24386 | List HP/BP/LP | |
| fine | 66 | 24388 | 0→75, 0.5906 | ms |
| WET | 67 | 24390 | 0→1, 0.0125 | dry↔wet |
| on | 68 | 24391 | toggle | relay input into delay |

## 4. Draft SynthDefs

Shared helpers are named here and defined by the shared-macro spec (other agent):
`~bitsPass.(sig, drive)`, `~bitsFilter.(sig, type /*0 HP,1 BP,2 LP*/, cutSt, reson)`,
`~bitsFilterEnv.(gate, aX, dX, s, rX)` (Reaktor ADSR, log-time args), and
`~bitsDelay.(sig, on, delaySt, fine, feed, pCutoff, pReson, pType, wet)` (mono). Panel List/Switch values
must be sent 0-based (snapshot value − 1).

```supercollider
// Reaktor log time -> seconds (0 = 1 ms, 20 = 10 ms, 60 = 1 s)
~rkTime = { |x| 10 ** (x / 20) / 1000 };
// Reaktor D module: jump to 1 on a rising edge, decay with time constant tau (GUESS: 5 tau, curve -5)
~rkDecay = { |t, x| EnvGen.ar(Env([0, 1, 0], [0, 5 * ~rkTime.(x)], [0, -5]), t) };
~bitsPass = { |sig, drive| sig / (sig.abs + drive.squared) };

SynthDef(\bitsK1, { |out = 0, basePitch = 33,
        penv1 = 1, decay1 = 15, amt1 = 34.68, penv2 = 0, decay2 = 36, amt2 = 68,
        baseDecay = 24.96, vol = 1, drive = 2,
        filtType = 2, cut = 120, reson = 0, env = 0, fA = 47, fD = 7, fS = 0.48, fR = 69,
        delOn = 0, delaySt = 3, fine = 0, feed = 0.6, pCutoff = 88, pReson = 0.64, pType = 2, wet = 0,
        level = -20.6, pan = 0, gateLen = 0.1|
    var trig = \trig.ar(0);
    var t = Trig1.ar(trig, SampleDur.ir);          // rising edge (Reaktor: gate event > 0)
    var gate = Trig1.ar(trig, gateLen);            // GUESS: gate length = note/button hold
    var p = basePitch
        + (penv1 * amt1 * ~rkDecay.(t, decay1))
        + (penv2 * amt2 * ~rkDecay.(t, decay2));  // semitones
    var phase = Phasor.ar(t, p.midicps * SampleDur.ir, 0, 1);   // Ramp Osc, hard sync to 0
    var sig = sin(2pi * phase);                                // Shaper = Sine
    var amp = vol * (baseDecay.neg * Sweep.ar(t, 1)).dbamp      // Base: -Decay dB per second
        * SetResetFF.ar(t, 0);                                  // silent until first trigger
    var fenv;
    sig = ~bitsPass.(sig * amp, drive);                        // PASS
    fenv = ~bitsFilterEnv.(gate, fA, fD, fS, fR);
    sig = ~bitsFilter.(sig, filtType, cut + (env * fenv), reson);
    sig = ~bitsDelay.(sig, delOn, delaySt, fine, feed, pCutoff, pReson, pType, wet);
    Out.ar(out, Pan2.ar(sig, pan, level.dbamp));               // Stereo Amp (pan law GUESS)
}).add;

SynthDef(\bitsStomps, { |out = 0, baseF = 33, higherF = 2000,
        penv1 = 1, decay1 = 17, penv2 = 1, decay2 = 0,
        wave = 1.5, pw = 0, ampA = 0, ampD = 45, ampR = 1, drive = 0.504,
        filtType = 2, cut = 120, reson = 0, env = 0, fA = 35, fD = 7, fS = 1, fR = 36,
        delOn = 0, delaySt = 0, fine = 3.543, feed = 0.9, pCutoff = 70, pReson = 0, pType = 1, wet = 0.425,
        level = -29, pan = 0.02, gateLen = 0.1|
    var trig = \trig.ar(0);
    var t = Trig1.ar(trig, SampleDur.ir);
    var gate = Trig1.ar(trig, gateLen);            // GUESS; release is ~1 ms, so this length is audible
    var f = (baseF + (higherF * ((penv1 * ~rkDecay.(t, decay1)) + (penv2 * ~rkDecay.(t, decay2)))))
        .clip(0, SampleRate.ir * 0.5);              // Hz, linear
    var a = EnvGen.ar(Env.adsr(~rkTime.(ampA), 5 * ~rkTime.(ampD), 0, 5 * ~rkTime.(ampR), 1, [0, -5, -5]), gate);
    // MultiWave Osc: free-running, band-limited. f = 0 must give DC (frozen phase), not silence: GUESS DC = 1
    var live = f > 0.01;
    var saw = Select.ar(live, [DC.ar(1), Saw.ar(f.max(0.01))]);
    var pls = Select.ar(live, [DC.ar(1), Pulse.ar(f.max(0.01), (pw.clip(-1, 1) + 1) * 0.5)]);   // duty GUESS
    var tri = Select.ar(live, [DC.ar(0), LFTri.ar(f.max(0.01))]);   // Reaktor tri is anti-aliased
    var sig = SelectX.ar(wave.clip(0, 2), [saw, pls, tri]) * a;     // Scanner GUESS
    var fenv;
    sig = ~bitsPass.(sig, drive);
    fenv = ~bitsFilterEnv.(gate, fA, fD, fS, fR);
    sig = ~bitsFilter.(sig, filtType, cut + (env * fenv), reson);
    sig = ~bitsDelay.(sig, delOn, delaySt, fine, feed, pCutoff, pReson, pType, wet);
    Out.ar(out, Pan2.ar(sig, pan, level.dbamp));
}).add;
```
Notes on the drafts:
- Default args = slot 1 of each bank (stepped values). Lists/Switch defaults are 0-based.
- Both voices are mono (1 voice, GUESS); a retrigger restarts K1's phase, time ramp and pitch envelopes.
  STOMPS restarts its envelopes only (oscillator phase keeps running).
- `SetResetFF` keeps K1 silent before the first trigger (Reaktor's ramp has run since load, so it is silent too).

## 5. Snapshot banks
Bank order = file order (K1: the last 9 snapshot blocks; STOMPS: the 10 blocks with ids up to 68).
Slot n is recalled by preset button n / MIDI note base+n−1. Values are stepped (raw in parentheses);
List/Switch shown by name (stored 1-based); toggles on/off.

### K1 bank (file order; slot n = preset button n)

| sid | label | 1 Snap 1 | 2 Snap 2 | 3 Snap 3 | 4 Snap 4 | 5 Snap 5 | 6 Snap 6 | 7 Snap 7 | 8 Snap 8 | 9 Snap 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | Base pitch | 33 | 33 | 33 | 33 | 33 | 33 | 33 | 33 | 33 |
| 1 | Penv1 Decay | 15 (15.75) | 7 (7.559) | 32 (32.13) | 32 (32.13) | 24 (24.57) | 28 (28.35) | 24 (24.57) | 24 (24.57) | 0 |
| 2 | Penv1 Amt | 34.68 (35.34) | 68 | 28.56 (28.91) | 28.56 (28.91) | 68 | 68 | 68 | 68 | 68 |
| 3 | Penv2 Decay | 36 (36.54) | 36 (36.54) | 7 (7.559) | 7 (7.559) | 7 (7.559) | 0 | 0 | 0 | 0 |
| 4 | Penv2 Amt | 68 | 68 | 68 | 68 | 68 | 68 | 68 | 68 | 68 |
| 5 | Penv1 | on | on | on | on | on | on | on | on | on |
| 6 | Penv2 | off | off | on | on | on | on | on | on | on |
| 7 | "Decay" | 24.96 | 162.7 (161.4) | 126.8 (123.6) | 126.8 (123.6) | 450.2 (449.1) | 336.4 | 336.4 | 336.4 | 600 |
| 8 | Vol | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 9 | Filter Reson | 0 | 0 | 0 | 0.07 (0.07717) | 0 | 0 | 0 | 0 | 0.56 (0.5633) |
| 11 | Cut | 120 | 120 | 120 | 87 (87.72) | 120 | 120 | 51 (51.5) | 51 (51.5) | 101 (101.9) |
| 12 | FEnv A | 47 | 47 | 47 | 47 | -20 | -20 | -20 | -20 | -20 |
| 13 | FEnv D | 7 | 7 | 7 | 7 | 7 | 7 | 7 | 7 | 7 |
| 14 | FEnv S | 0.48 | 0.48 | 0.48 | 0.48 | 0.48 | 0.48 | 0.48 | 0.48 | 0.48 |
| 15 | FEnv R | 69 (69.29) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 16 | Env | 0 (0.9449) | 106 (106.8) | 106 (106.8) | 34 (34.96) | 8 (8.504) | 8 (8.504) | 0 (0.9449) | 0 (0.9449) | 120 |
| 17 | Feed | 0.6 (0.6693) | 0.6 (0.6693) | 0.6 (0.6693) | 0.5 (0.5039) | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9685) |
| 18 | Delay st | 3 | 3 | 3 | 3 | 0 | 0 | 0 | 3 | 4 |
| 19 | P Cutoff | 88 (88.5) | 88 (88.5) | 88 (88.5) | 88 (88.5) | 75 (75.12) | 99 (99.53) | 99 (99.53) | 99 (99.53) | 99 (99.53) |
| 20 | Delay Reson | 0.64 (0.6482) | 0.64 (0.6482) | 0.64 (0.6482) | 0.64 (0.6482) | 0 | 0 | 0 | 0 | 0 |
| 21 | Delay filter type | LP | LP | LP | LP | LP | BP | BP | BP | BP |
| 22 | fine | 0 | 0 | 0 | 0 | 18.31 | 21.26 | 21.26 | 0 | 0 |
| 23 | WET | 0 | 0 | 0 | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) |
| 27 | Level | -20.6 (-20.05) | -33.8 (-33.52) | -36.2 (-35.92) | -31.4 (-31.35) | -36.8 (-36.25) | -20 | -20 | -20 | -20 |
| 42 | Delay on | off | off | off | on | on | on | off | on | on |
| 43 | Filter type | LP | LP | LP | HP | LP | LP | HP | HP | HP |
| 51 | DRIVE | 2 | 0.2197 | 0.1 | 0.1 | 0.1 | 1.372 | 1.162 | 1.162 | 1.162 |

### STOMPS bank (file order; slot n = preset button n)

| sid | label | 1 Snap 3 | 2 Snap 2 | 3 Snap 3 | 4 Snap 4 | 5 Snap 5 | 6 Snap 6 | 7 Snap 7 | 8 Snap 8 | 9 Snap 9 | 10 Snap 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 6 | Amp D | 45 (45.35) | 45 (45.35) | 32 (32.13) | 35 (35.91) | 35 (35.91) | 35 (35.91) | 35 (35.91) | 35 (35.91) | 66 (66.77) | 66 (66.77) |
| 8 | Amp A | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 10 | Wave | 1.5 (1.512) | 1.5 (1.512) | 1.5 (1.512) | 3 | 0 | 0 | 3 | 1.688 (1.701) | 1.688 (1.701) | 1.387 (1.417) |
| 21 | Pan | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) | 0.02 (0.03937) |
| 23 | Level | -29 | -29 | -26.6 (-26.47) | -36.2 (-36.07) | -32.6 (-32.12) | -32.6 (-32.12) | -32.6 (-32.12) | -32.6 (-32.12) | -32.6 (-32.12) | -32.6 (-32.12) |
| 27 | Penv1 Decay | 17 (17.64) | 17 (17.64) | 0 | 0 | 0 | 0 | 0 | 36 (36.54) | 36 (36.54) | 36 (36.54) |
| 28 | Higher F | 2000 | 2000 | 2000 | 2000 | 2000 | 2000 | 2000 | 360 (362.2) | 0 | 0 |
| 29 | base F | 33 | 33 | 33 | 33 | 33 | 33 | 33 | 33 | 0 | 0 |
| 30 | Penv2 Decay | 0 | 0 | 30 (30.87) | 28 (28.98) | 28 (28.98) | 28 (28.98) | 28 (28.98) | 28 (28.98) | 62 (62.99) | 62 (62.99) |
| 31 | Penv2 | on | on | on | on | off | off | off | off | on | off |
| 32 | Penv1 | on | on | on | on | on | on | on | on | on | off |
| 36 | Amp R | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 37 | Filter Reson | 0 | 0 | 0 | 0 | 0 | 0.71 (0.7176) | 0.71 (0.7176) | 0.71 (0.7176) | 0 | 0.76 (0.7639) |
| 38 | Filter type | LP | LP | BP | LP | LP | LP | BP | HP | HP | HP |
| 39 | Cut | 120 | 120 | 120 | 120 | 81 (81.42) | 81 (81.42) | 81 (81.42) | 68 | 20 | 75 (75.91) |
| 40 | FEnv A | 35 (35.75) | 35 (35.75) | 35 (35.75) | 35 (35.75) | 60 (60.31) | 60 (60.31) | 60 (60.31) | 60 (60.31) | 60 (60.31) | 60 (60.31) |
| 41 | FEnv D | 7 | 7 | 7 | 7 | 7 | 7 | 7 | 7 | 7 | 7 |
| 42 | FEnv S | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 43 | FEnv R | 36 (36.54) | 36 (36.54) | 36 (36.54) | 36 (36.54) | 36 (36.54) | 36 (36.54) | 36 (36.54) | 36 (36.54) | 36 (36.54) | 36 (36.54) |
| 44 | Env | 0 (0.9449) | 0 (0.9449) | 0 (0.9449) | 0 (0.9449) | -120 | 120 | 120 | 120 | 120 | 120 |
| 45 | DRIVE | 0.5039 | 0.5039 | 0.878 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 | 0.1 |
| 60 | pw | 0 (0.007874) | 0.62 (0.622) | 0.72 (0.7323) | 0 (0.007874) | 0 (0.007874) | 0.8 (0.811) | 0.8 (0.811) | 0.8 (0.811) | 0.8 (0.811) | 0 (0.007874) |
| 61 | Feed | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9213) | 0.9 (0.9213) | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9685) | 0.9 (0.9213) |
| 62 | Delay st | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 63 | P Cutoff | 70 (70.39) | 70 (70.39) | 70 (70.39) | 120 | 120 | 70 (70.39) | 70 (70.39) | 70 (70.39) | 70 (70.39) | 120 |
| 64 | Delay Reson | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 65 | Delay filter type | BP | BP | BP | LP | LP | BP | BP | BP | BP | LP |
| 66 | fine | 3.543 | 3.543 | 3.543 | 3.543 | 3.543 | 3.543 | 3.543 | 3.543 | 3.543 | 3.543 |
| 67 | WET | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) | 0.425 (0.4331) |
| 68 | Delay on | off | on | off | off | off | off | off | off | off | on |

## 6. What the banks sound like (reading of the numbers)
- **K1** = sine kick at A1 (55 Hz): Penv1 is a fast pitch drop (1–40 ms τ, +29..68 st), Penv2 (on in slots 3–9) adds
  a very short +68 st blip (τ 1–2 ms, a click). Slot 1 decays 25 dB/s (long boom, DRIVE 2 = clean and quiet); slots 2–5 use DRIVE 0.22/0.1
  (hard saturation, square-ish); slots 5–9 decay 336–600 dB/s (short clicks). Delay on in slots 4, 5, 6, 8, 9.
- **STOMPS** = Hz-swept saw/pulse/tri hits from 33 Hz with 2 kHz sweeps (slots 1–7), a 360 Hz sweep (slot 8),
  and DC thumps through a sweeping HP (slots 9–10, f = 0). Its delay is used as a 3.5 ms comb resonator.

## 7. Open questions
1. Toggle on-value: does Reaktor output 1 or 0.5 for a stored 0.5 (halves Penv depths)? Render check.
2. Fader step rule (floor vs round to step): floor fits all panel readouts (Feed 0.6, Decay 24.96) — GUESS.
3. D-module time law (time constant vs time-to-silence) and ADSR attack shape: reused project rule, GUESS.
4. STOMPS Wave: Scanner (crossfade) or Selector (truncate)? Curve property not found in the bytes.
5. Pulse duty mapping of pw and DC value at f = 0 (Reaktor: frozen random phase).
6. Gate length: SC voice needs a hold (`gateLen`) since STOMPS cuts at gate-off (R≈1 ms) and filter envs
   use S/R. What hold should the Oceanode node use (step length? fixed?)
7. Voice count (assumed 1) and the MIDI note numbers of the preset slots (from macro names only).
8. Stereo Amp pan law (Pan2 equal power assumed).
