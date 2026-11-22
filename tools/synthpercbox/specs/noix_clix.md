# Bits.ens — CLIX and NOIX voices (decode spec)

Sources: `~/Downloads/Bits_r6.ens`, extractor report `r6.json` (REPORT), `bits_decoded.json` (DECODED).
CLIX = Instrument object 1331, NOIX = Instrument object 2274. Everything below was read from the wires
(`dump_macro.py`, raw `connections` with `target_input_index`), the Core cells (`core_view.py --net`) and raw
module records in the .ens; anything not read directly is marked **GUESS**. Conventions (floor-to-step
quantisation, 1-based List/Switch values, Reaktor log times, `~rkTime`) are the same as in `k1_stomps.md` §0.

## 0. Conventions and decoder corrections specific to these two

- **Snapshot values**: normalised 0..1 on a 1/127 grid; value = `e1 + n·(e2−e1)`, then **floored to the step**.
  Floor reproduces every CLIX screenshot number: Delay Feed n=122/127 → **0.9**, MicroDel Feed → **0.6**,
  Micro Space Hi −5.008 → **−5.25**, Lo 0.094 → **0**, Size **73**, Mix **0.43**, Delay st **2.875**,
  Delay fine **9.45**, MicroDel fine **9.21**. Tables below are floored.
- **CLIX sid 0 = Res (Fader 1337)**. The extractor drops `snapshot_control_id` 0 (falsy), so DECODED has no Res
  control and snapshot key `"0"` looks unmapped. In the raw fader records the id word after the range floats is
  2 / 3 / 4 for Fader 1337 / 1339 / 1340 (= sid + 2), and the label bytes in the records read `Res`, `Co`, `Ca`
  (so the Co/Ca heuristic labels are right). Res range 0..0.99, step 0.0099.
- **DECODED `saved` is wrong for these instruments.** CLIX: the denormals are a u32 `1` read as a float.
  NOIX: 40/40/2/0.49… are range midpoints (default field). The real panel value is the float after the
  `01 00 00 00` word, before `00 00 00 3f`, normalised. Read that way:
  - CLIX panel = **snapshot 3** exactly (Res 0.99, Co 120, Ca 50, Clix Level −9.92, MicroDel WET 0.614,
    Delay WET 0.409 …), matching the user's screenshot.
  - NOIX panel = **snapshot 5** (D 80, A 0, R 0, Reson 0, Cut 90.9, FEnv A −20 / D 100 / S 1 / R 0, Env 0.95,
    MicroDel P Cutoff 120, Feed 0.37 …), except Level reads −50 (snapshot says −35; record layout uncertain).
- **Switch / List**: snapshot stores a 1-based index. Delay List entries in the file: `HP`=0, `BP`=1, `LP`=2,
  so 1 = HP, 2 = BP, 3 = LP. Cross-check: in both filter Switches the only input port without flag 0x20 in its
  `encoding` is `LP2` (CLIX) / `LP` (NOIX), i.e. the selected one, and both snapshots store 3. Send 0-based to SC.
- **Micro Space `on` button** is stored as 0.5 when on, 0 when off.
- **Reaktor ADSR** (catalog help, kind 272): A, D, R inputs are logarithmic, `t_ms = 10^(x/20)`
  (0 = 1 ms, 20 = 10 ms, 40 = 100 ms, 60 = 1 s, 80 = 10 s); S is linear, a factor of the peak; G > 0 starts
  the envelope with peak = G, G = 0 starts the release. Segment shape: same **GUESS** as k1_stomps (time constant τ,
  segment ≈ 5τ, curve −5; linear attack).
- **Pitch inputs** of Multi 2-Pole / Multi/HP 4-Pole (kinds 303/309): 1 semitone per unit, `Hz = 440·2^((P−69)/12)`.
  Res 0 = minimum, 1 = self-oscillation (typ. 0..0.98).
- **Amp/Mixer Lvl** (kind 156) and **Stereo Amp/Mixer Lvl** (157) are in dB; Pan −1 (L) .. 1 (R).

## 1. Triggering (same preset scheme as K1/STOMPS)

```
Sel. Note Gate (note k_n) ─► Separator(Thld 0) ─Hi─► Value(n) ─┐
                                              └─Lo─► (0) ──────┤ M ─► M ─► presets Out
preset Button n (On value n)                                   ┘
presets Out ─► Order: (1) Snapshot.Snp = v  (2) Snapshot.Recl  (3) Compare(v > 0) ─► 1 / 0 out
```
- Note-on of note k_n or preset button n recalls **snapshot n of bank 1, then sends 1**; note-off / release sends 0.
- **Velocity is discarded** (Value outputs the constant n, Compare gives exactly 1). **MIDI pitch is not used**
  by either voice: notes only select presets.
- **Note map, read from the Sel. Note Gate records** (byte 44 of each record):

| preset n | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CLIX ("MIDI 43-58") | 43 | 44 | 45 | 46 | 47 | 33 | 34 | 50 | 36 | 37 | 38 | 39 | 55 | 41 |
| NOIX ("MIDI 28-41") | 28 | 29 | 30 | 31 | 32 | 33 | 34 | 35 | 36 | 37 | 38 | 39 | 40 | 41 |

  The CLIX map looks like a half-edited copy of NOIX's: notes 33, 34, 36–39 and 41 fire **both** instruments
  (CLIX presets 6, 7, 9–12, 14 and NOIX presets 6, 7, 9–12, 14). Preset buttons: CLIX sids 250–263, NOIX sids 93–106.
- Banks hold only 6 (CLIX) and 5 (NOIX) snapshots for 14 slots: higher slots recall nothing (**GUESS**: no-op)
  and only trigger. In file order the CLIX bank is the 6 "sinebits" blocks at 0x13b391..0x13f2aa, NOIX the 5
  "Snap" blocks at 0x148976..0x14c102 (ids ≤ 108); assumed slot = file order (**GUESS**).
- Each preset recall also changes the delay / reverb settings while earlier tails ring.
- **Voice count**: not in the report. All gate sources are monophonic (Button, Sel. Note Gate), and the
  Audio Voice Combiners are boilerplate. **GUESS: 1 voice**; in CLIX a new hit just adds another impulse into
  the same ringing filter, in NOIX it restarts the ADSR (Reaktor ADSR retriggers on every G > 0 event).

## 2. CLIX (Instrument 1331, macro "Clix" 1335)

### Signal flow (exact)
```
presets Out ─► In 21604 ─┐
Button "$" (sid 6) ──────┴► M 16791 ─► Separator 1349 (Thld = Const 0) ─Hi─┬► Core 1347 "Impulse".Trig
                                                                           └► Core 1346 "Random" (event)
Core 1346 ─► Selector/Scanner 1338 .Pos      In0 = Co (sid 1), In1 = Ca (sid 2)
          └► (1 − x) ─► Meter 1341 (display only)
Selector 1338 ─► Multi/HP 4-Pole 1344 .P ; Fader 1337 "Res" (sid 0) ─► .Res ; Impulse.Out ─► .In
1344 outs HP2 BP2 LP2 HP4 BH4 BP4 ─► Switch 1345 (sid 3, panel lamps) ─► Amp/Mixer 2501 (Lvl = Level sid 5, dB)
─► Clix Out ─► MicroDel 19791 ─► Delay 19768 ─► Micro Space 25078 ─► Stereo Amp/Mixer 19647 (Lvl sid 138 dB, Pan sid 137)
─► Voice Combiners ─► Instrument out L/R ─► master sums (+#1094 / +#1878, input 1)
```
The macro description in the file is "Filtre hp/lp en cascada de 2 pols." (stale, from an earlier version).

### Core cell 1347 "Impulse" (decoded)
An event at Trig latches the constant **1** (the event's value is ignored); at the next audio tick the cell
outputs that 1 and resets the latch to 0. Result: **a one-sample unit impulse, one sample after each trigger**.
No amplitude envelope exists anywhere in CLIX: the sound is the impulse response of the resonant filter.

### Core cell 1346 (event "Random", decoded)
Library LCG: `state = state·1103515245 + 12345` (integer), out = `state · 4.6567e-10` ∈ [−1, 1), then `Abs`.
So each trigger draws **one uniform random r ∈ [0, 1)**. Seed input unconnected.

### Co / Ca
Both are filter **cutoff pitches in semitones** (range 50..120 = 147 Hz .. 8.37 kHz), the two ends of a random
range. Selector/Scanner 1338 mode is not readable from the record; in Selector mode Pos ∈ [0,1) would always pick
Co and Ca would be dead, so it must be in **Scanner mode** (**GUESS**, strong): per hit
`P = Co + r·(Ca − Co)` (linear in semitones, i.e. exponential in Hz). Co and Ca are symmetric, so swapping them
changes nothing. P is held until the next hit.

### Filter and the six modes
Multi/HP 4-Pole (kind 309): resonant 4th-order multimode, unity passband gain, boost at resonance.
Outputs (Switch order, snapshot value 1..6): **HP2, BP2, LP2** (2-pole, 12 dB/oct), **HP4, BH4, BP4** (4-pole).
**GUESS** for the 4-pole taps: two cascaded identical SVF stages at the same P/Res; HP2/BP2/LP2 from stage 1,
HP4 = HP(HP2), BP4 = BP(BP2), BH4 = HP(BP2) ("band-high"). Every stored snapshot uses **LP2 (3)**, so only LP2
matters for the presets.
Res 0.99 turns the impulse into a decaying sine "bit" (≈ Q 50: at 2 kHz t60 ≈ 55 ms, shorter at high pitch);
Res 0 gives a bare low-passed click. Res mapping to Q is a **GUESS**: SVF damping `k = 2·(1 − Res)`, Q = 1/k.

### Panel controls

| label | sid | oid | range, step | panel value (record) | DSP mapping |
|---|---|---|---|---|---|
| Res | 0 | 1337 | 0..0.99, 0.0099 | 0.99 | filter Res |
| Co | 1 | 1339 | 50..120 st, 0.259 | 120 | scanner In0 (P when r = 0) |
| Ca | 2 | 1340 | 50..120 st, 0.259 | 50 | scanner In1 (P when r → 1) |
| L (mode lamps HP2 BP2 LP2 HP4 BH4 BP4) | 3 | 1345 | Switch, 6 inputs | LP2 | output select (1-based in snapshots) |
| Level (Clix section) | 5 | 2502 | −60..0 dB, 0.5 | −9.92 | Amp/Mixer gain after filter |
| $ (trigger button) | 6 | 16792 | Button 0..1 | — | merged with presets → trigger (Hi events only) |
| Pan | 137 | 19648 | −1..1, 0.02 | 0.04 | Stereo Amp pan |
| Level (vertical fader) | 138 | 19649 | −50..−20 dB, 0.6 | (−20 in all snapshots) | Stereo Amp Lvl, after Micro Space |
| MicroDel fine / Feed / LP / P Cutoff / Reson / WET | 178 / 174 / 177 / 175 / 176 / 179 | 19803 / 19797 / 19802 / 19799 / 19800 / 19806 | 0..30 ms / 0..1 / list / 20..120 / 0..0.98 / 0..1 | 9.21 / 0.6 / LP / 86 / 0 / 0.61 | shared MicroDel |
| Delay st / fine / Feed / LP / P Cutoff / Reson / WET | 168 / 172 / 167 / 171 / 169 / 170 / 173 | 19775 / 19787 / 19774 / 19785 / 19782 / 19783 / 19790 | 0..16 steps / 0..75 ms / 0..1 / list / 20..120 / 0..0.98 / 0..1 | 2.875 / 9.45 / 0.9 / LP / 79 / 0.43 / 0.4 | shared Delay (tempo-synced: Tempo Info → 1/x → ×250 → ×Delay st, + fine) |
| Micro Space Size / Mod / Lo / Hi / Position / Mix / on | 264 / 265 / 266 / 267 / 268 / 269 / 270 | 25081..25086, 25092 | 0..100 / 0..1 / ±12 dB / ±12 dB / 0..1 / 0..1 / toggle | 73 / 0 / 0 / −5.25 / 1 / 0.43 / on | shared Micro Space |

### CLIX snapshot bank (floored panel values)

| sid | control | 1 sinebits | 2 sinebits 1 | 3 sinebits 1 | 4 sinebits 2 | 5 sinebits 3 | 6 sinebits 2 |
|---|---|---|---|---|---|---|---|
| 0 | Res | 0.99 | 0 | 0.99 | 0 | 0 | 0 |
| 1 | Co | 96.15 | 120 | 120 | 120 | 120 | 105.48 |
| 2 | Ca | 50 | 120 | 50 | 120 | 120 | 66.33 |
| 3 | filter mode | LP2 | LP2 | LP2 | LP2 | LP2 | LP2 |
| 5 | Clix Level dB | −10 | −10 | −10 | 0 | 0 | 0 |
| 137 | Pan | 0.02 | 0.02 | 0.02 | 0.02 | 0.02 | 0.02 |
| 138 | Out Level dB | −20 | −20 | −20 | −20 | −20 | −20 |
| 178 | MicroDel fine ms | 9.21 | 9.21 | 9.21 | 14.88 | 14.88 | 9.21 |
| 174 | MicroDel Feed | 0.6 | 0.6 | 0.6 | 0.9 | 0.9 | 0.6 |
| 177 | MicroDel mode | LP | LP | LP | LP | LP | LP |
| 175 | MicroDel P Cutoff | 86 | 86 | 86 | 101 | 101 | 86 |
| 176 | MicroDel Reson | 0 | 0 | 0 | 0 | 0 | 0 |
| 179 | MicroDel WET | 0.613 | 0 | 0.613 | 1 | 0 | 0 |
| 168 | Delay st | 2.875 | 2.875 | 2.875 | 2.875 | 1 | 2.875 |
| 172 | Delay fine ms | 9.45 | 9.45 | 9.45 | 9.45 | 9.45 | 9.45 |
| 167 | Delay Feed | 0.9 | 0.9 | 0.9 | 0.9 | 0.9 | 0.9 |
| 171 | Delay mode | LP | LP | LP | LP | LP | LP |
| 169 | Delay P Cutoff | 79 | 79 | 79 | 79 | 101 | 79 |
| 170 | Delay Reson | 0.43 | 0.43 | 0.43 | 0.43 | 0.43 | 0.43 |
| 173 | Delay WET | 0 | 0 | 0.4 | 0 | 0.425 | 0 |
| 264 | Space Size | 73 | 73 | 73 | 73 | 73 | 73 |
| 265 | Space Mod | 0 | 0 | 0 | 0 | 0 | 0 |
| 266 | Space Lo dB | 0 | 0 | 0 | 0 | 0 | 0 |
| 267 | Space Hi dB | −5.25 | −5.25 | −5.25 | −5.25 | −5.25 | −5.25 |
| 268 | Space Position | 1 | 1 | 1 | 1 | 1 | 1 |
| 269 | Space Mix | 0.43 | 0.43 | 0.43 | 0.43 | 0.43 | 0.43 |
| 270 | Space on | on | off | on | on | off | off |

Character: slots 1 and 3 = random-pitched sine blips (147 Hz..2.3 kHz / ..8.4 kHz); slots 2, 4, 5 = fixed bright
clicks (LP2 at 8.4 kHz, no resonance); slot 6 = random-cutoff dull clicks (330 Hz..3.6 kHz, Res 0).

## 3. NOIX (Instrument 2274)

### Signal flow (exact)
```
Button "trigger" (sid 2) ─┐
presets Out ──────────────┴► M 2323 ─┬► ADSR 2279 .G   (A = sid 8, D = sid 6, S unconnected = 0, R = sid 36)
                                     ├► Lamp 2324
                                     └► Filter 2284 .t ─► Filter Env 19888 ADSR .G
ADSR 2279 (audio) ─► Core 2277 .a
Core 2277 out0 (binary noise · a) ─► Selector/Scanner 2282 In"1"
Core 2277 "noise" (white noise · a) ─► Selector/Scanner 2282 In"2"      (In"0" unconnected)
Wave (sid 10, 1..3) ─► Selector 2282 .Pos
Selector ─► Filter 2284: Multi 2-Pole .In ; .P = Cut (sid 52) + Env (sid 57) · A/E(FilterEnv) ; .Res = Reson (sid 12)
            outputs HP BP LP ─► Switch 2290 (sid 13) ─► Filter out
─► MicroDel 2299 ─► Delay 3213 ─► Micro Space 19985 ─► Stereo Amp/Mixer 2325 (Lvl sid 23 dB, Pan sid 21)
─► Voice Combiners ─► Instrument out L/R ─► master sums (+#1094 / +#1878, input 4)
```

### Core cell 2277 (decoded)
- **Digital Noise**: the same LCG clocked by the sample rate, out = `state·4.6567e-10`: uniform white noise in
  [−1, 1). Output `noise` = Digital Noise × a.
- **Binary Noise**: a second LCG per sample; Compare(random > 0) routes to latch +0.5 or −0.5: a random ±0.5
  sign per sample (spectrally white, RMS 0.5 vs 0.577 for the uniform one). Output 0 = Binary Noise × a.
- `a` is the amp ADSR, so the **ADSR is the VCA** (multiplied inside the cell, before the filter).

### Wave
Selector 2282 has 4 ports (Pos + In"0", In"1", In"2"); Wave 1..3. Wave = 1 → binary, Wave ≥ 2 → white
(in either mode: Pos ≥ 2 saturates at the last input). Between 1 and 2: Selector mode = hard switch at 2,
Scanner mode = linear crossfade binary→white (mode not readable, **GUESS**: crossfade). Every snapshot has
**Wave = 3 → pure white noise**; the binary source is unused by the presets.

### Amp envelope
Reaktor ADSR, peak = gate (always 1), **S = 0** (input unconnected), so it is an **AD envelope**; R acts only when
the gate drops before the decay ends. Gate length therefore matters (note/button hold). Times use the log law
(A 0 = 1 ms, D 31 = 35 ms, D 46 = 200 ms, D 80 = 10 s, A 66 = 2 s).

### Filter (macro 2284)
Multi 2-Pole (kind 303), P = Cut + Env·fenv in semitones, Res = Reson, Switch HP/BP/LP (all snapshots LP).
Filter Env 19888 = ADSR with A sid 53 (−20..100 → 0.1 ms .. 100 s), D sid 54 (−20..100), S sid 55 (0..1),
R sid 56 (0..80), gate = the same trigger, then A/E (audio → event, control rate). In snapshots 1–4 the
filter env has **A = 100 (100 s attack)** with Env = +120 st, so it only creeps up (~+1.2 st per second of
hold): effectively static cutoff. Snapshot 5 has Env ≈ 0.

### Panel controls

| label | sid | oid | range, step | panel value (record) | DSP mapping |
|---|---|---|---|---|---|
| trigger | 2 | 2278 | Button 0..1 | — | gate (press 1, release 0) |
| Wave | 10 | 2283 | 1..3, 0.025 | 3 | noise select (§Wave) |
| A | 8 | 2281 | 0..80 log-ms, 1 | 0 | amp attack |
| D | 6 | 2280 | 0..80 log-ms, 1 | 80 | amp decay (to 0) |
| R | 36 | 2349 | 0..80 log-ms, 1 | 0 | amp release |
| LP (filter menu) | 13 | 2290 | Switch HP/BP/LP | LP | filter output |
| Cut | 52 | 19887 | 20..120 st, 1 | 90.9 | filter P base |
| Reson | 12 | 2289 | 0..0.98, 0.01 | 0 | filter Res |
| Env | 57 | 19899 | −120..120 st, 2 | 0.9 | filter env amount (semitones) |
| Filter Env A / D / S / R | 53 / 54 / 55 / 56 | 19890 / 19891 / 19894 / 19895 | −20..100 / −20..100 / 0..1 / 0..80 | −20 / 100 / 1 / 0 | filter ADSR |
| MicroDel fine / Feed / LP / P Cutoff / Reson / WET | 19 / 14 / 18 / 16 / 17 / 20 | 2318 / 2305 / 2316 / 2313 / 2314 / 2321 | 0..30 ms / 0..1 / list / 20..120 / 0..0.98 / 0..1 | 14.9 / 0.37 / LP / 120 / 0 / 0 | shared MicroDel |
| Delay st / fine / Feed / LP / P Cutoff / Reson / WET | 38 / 42 / 37 / 41 / 39 / 40 / 43 | 3220 / 3232 / 3219 / 3230 / 3227 / 3228 / 3235 | 0..16 / **0..30 ms** / 0..1 / list / 20..120 / 0..0.98 / 0..1 | 2 / 0 / 0.72 / HP / 103 / 0 / 0 | shared Delay (note: NOIX Delay fine range 0..30, CLIX 0..75) |
| Micro Space Size / Mod / Lo / Hi / Position / Mix / on | 58 / 59 / 60 / 61 / 62 / 63 / 108 | 19988..19993, 24803 | as CLIX | 73 / 1 / 0 / 0 / 1 / 0.3 / off | shared Micro Space |
| Level | 23 | 2329 | −50..−35 dB, 0.3 | −50 (? snapshots −35) | Stereo Amp Lvl |
| Pan | 21 | 2326 | −1..1, 0.02 | −0.02 | Stereo Amp pan |

### NOIX snapshot bank (floored panel values)

| sid | control | 1 Snap 1 | 2 Snap 2 | 3 Snap 3 | 4 Snap 4 | 5 Snap 2 |
|---|---|---|---|---|---|---|
| 10 | Wave | 3 | 3 | 3 | 3 | 3 |
| 8 | Amp A | 0 | 0 | 0 | 66 | 0 |
| 6 | Amp D | 46 | 31 | 31 | 80 | 80 |
| 36 | Amp R | 45 | 0 | 0 | 0 | 0 |
| 13 | Filter mode | LP | LP | LP | LP | LP |
| 52 | Cut | 94 | 94 | 94 | 79 | 90 |
| 12 | Reson | 0.81 | 0.81 | 0.81 | 0.81 | 0 |
| 57 | Env | 120 | 120 | 120 | 120 | 0 |
| 53 | FEnv A | 100 | 100 | 100 | 100 | −20 |
| 54 | FEnv D | 100 | 100 | 100 | 100 | 100 |
| 55 | FEnv S | 0.82 | 0.82 | 0.82 | 0.82 | 1 |
| 56 | FEnv R | 59 | 59 | 59 | 59 | 0 |
| 19 | MicroDel fine ms | 17.95 | 14.88 | 14.88 | 30 | 14.88 |
| 14 | MicroDel Feed | 0.6 | 0.6 | 0.6 | 0.6 | 0.3 |
| 18 | MicroDel mode | HP | HP | HP | HP | LP |
| 16 | MicroDel P Cutoff | 45 | 45 | 45 | 32 | 120 |
| 17 | MicroDel Reson | 0.3 | 0.3 | 0.3 | 0.3 | 0 |
| 20 | MicroDel WET | 0.7 | 0.7 | 0.7 | 1 | 0 |
| 38 | Delay st | 2 | 2 | 2 | 4 | 2 |
| 42 | Delay fine ms | 0 | 0 | 0 | 0 | 0 |
| 37 | Delay Feed | 0.8 | 0.8 | 0.8 | 0.8 | 0.7 |
| 41 | Delay mode | HP | HP | HP | LP | HP |
| 39 | Delay P Cutoff | 78 | 78 | 78 | 78 | 103 |
| 40 | Delay Reson | 0.13 | 0.13 | 0.13 | 0.13 | 0 |
| 43 | Delay WET | 0 | 0.4 | 0.4 | 0.488 | 0 |
| 58 | Space Size | 73 | 73 | 73 | 73 | 73 |
| 59 | Space Mod | 1 | 1 | 0 | 0 | 1 |
| 60 | Space Lo dB | 0 | 0 | 0 | 0 | 0 |
| 61 | Space Hi dB | 0 | 0 | 8 | 8 | 0 |
| 62 | Space Position | 1 | 1 | 1 | 1 | 1 |
| 63 | Space Mix | 0.29 | 0.29 | 0.29 | 0.29 | 0.29 |
| 108 | Space on | off | off | on | off | off |
| 23 | Out Level dB | −35 | −35 | −35 | −35 | −35 |
| 21 | Pan | −0.02 | −0.02 | −0.02 | −0.02 | −0.02 |

## 4. Effect chain (both instruments)

Strict **series inserts**, mono until the Stereo Amp: `voice → MicroDel → Delay → Micro Space → Stereo Amp(Level dB, Pan) → L/R`.
No sends; each block blends dry/wet internally with its own WET / Mix. From the wires (details in the shared
spec): MicroDel's WET scans `dry → dry + delayed` (sum +#19804 feeds Selector In1); Delay's WET scans
`dry → delayed` (its sum +#19788 has no output wire). Feedback path in both: delayed → Multi 2-Pole
(P Cutoff, Reson) → LP/BP/HP list → ×Feed → back into the delay input. Neither delay has an `on` button here
(pass `on = 1` to the shared helper). Micro Space has `on` (bypass) and Mix.

## 5. Draft SynthDefs

Shared helpers (named here, defined by the shared-macro spec): `~rkTime`, `~bitsFilter.(sig, type /*0 HP,1 BP,2 LP*/, cutSt, reson)`,
`~bitsFilterEnv.(gate, aX, dX, s, rX)`, `~bitsDelay.(sig, on, delaySt, fine, feed, pCutoff, pReson, pType, wet)`,
plus `~bitsMicroDel.(sig, fine, feed, pCutoff, pReson, pType, wet)` and
`~bitsMicroSpace.(sig, on, size, position, mod, lo, hi, mix)` (mono in, mono out). New here:
`~bitsMulti4Pole.(in, cutSt, res)` → `[hp2, bp2, lp2, hp4, bh4, bp4]`. List/Switch values are sent 0-based.

```supercollider
~rkTime = { |x| 10 ** (x / 20) / 1000 };   // Reaktor log time -> seconds
// GUESS: Multi/HP 4-Pole as two cascaded SVF stages (needs sc3-plugins SVF; res 0..1 like Reaktor's)
~bitsMulti4Pole = { |in, cutSt, res|
    var f = cutSt.midicps.clip(10, SampleRate.ir * 0.45);
    var lp2 = SVF.ar(in, f, res, 1, 0, 0), bp2 = SVF.ar(in, f, res, 0, 1, 0), hp2 = SVF.ar(in, f, res, 0, 0, 1);
    [hp2, bp2, lp2, SVF.ar(hp2, f, res, 0, 0, 1), SVF.ar(bp2, f, res, 0, 0, 1), SVF.ar(bp2, f, res, 0, 1, 0)]
};

// CLIX: one persistent synth; hits overlap in the same filter (Reaktor: 1 voice, GUESS)
SynthDef(\bitsClix, { |out = 0,
        res = 0.99, co = 120, ca = 50, mode = 2 /*0 HP2 1 BP2 2 LP2 3 HP4 4 BH4 5 BP4*/, clixLevel = -10,
        mdFine = 9.21, mdFeed = 0.6, mdPCutoff = 86, mdReson = 0, mdType = 2, mdWet = 0.6125,
        delaySt = 2.875, dFine = 9.45, dFeed = 0.9, dPCutoff = 79, dReson = 0.43, dType = 2, dWet = 0.4,
        spOn = 1, spSize = 73, spPosition = 1, spMod = 0, spLo = 0, spHi = -5.25, spMix = 0.43,
        level = -20, pan = 0.02|
    var trig = \trig.ar(0);
    var imp = Trig1.ar(trig, SampleDur.ir);              // Core 1347: 1-sample unit impulse per rising edge
    var r = TRand.ar(0, 1, imp);                         // Core 1346: |LCG| in [0,1), drawn per hit
    var p = co + (r * (ca - co));                        // Scanner 1338 (GUESS: scanner mode)
    var sig = Select.ar(mode, ~bitsMulti4Pole.(imp, p, res));
    sig = sig * clixLevel.dbamp;                         // Amp/Mixer 2501
    sig = ~bitsMicroDel.(sig, mdFine, mdFeed, mdPCutoff, mdReson, mdType, mdWet);
    sig = ~bitsDelay.(sig, 1, delaySt, dFine, dFeed, dPCutoff, dReson, dType, dWet);
    sig = ~bitsMicroSpace.(sig, spOn, spSize, spPosition, spMod, spLo, spHi, spMix);
    Out.ar(out, Pan2.ar(sig, pan, level.dbamp));         // GUESS: Reaktor pan law = equal power
}).add;

// NOIX: AD(SR, S=0) envelope as VCA on noise, then resonant 2-pole with a slow filter ADSR
SynthDef(\bitsNoix, { |out = 0,
        wave = 3, a = 0, d = 80, r = 0,
        filtType = 2, cut = 90, reson = 0, env = 0, fA = -20, fD = 100, fS = 1, fR = 0,
        mdFine = 14.88, mdFeed = 0.3, mdPCutoff = 120, mdReson = 0, mdType = 2, mdWet = 0,
        delaySt = 2, dFine = 0, dFeed = 0.7, dPCutoff = 103, dReson = 0, dType = 0, dWet = 0,
        spOn = 0, spSize = 73, spPosition = 1, spMod = 1, spLo = 0, spHi = 0, spMix = 0.29,
        level = -35, pan = -0.02, gateLen = 0.1|
    var trig = \trig.ar(0);
    var gate = Trig1.ar(trig, gateLen);                  // GUESS: gate length = note/button hold; a new
                                                         // rising edge must retrigger (host drops gate between hits)
    var amp = EnvGen.ar(Env.adsr(~rkTime.(a), 5 * ~rkTime.(d), 0, 5 * ~rkTime.(r), 1, [0, -5, -5]), gate);
    var white = WhiteNoise.ar;                           // Digital Noise, uniform [-1,1)
    var binary = ((WhiteNoise.ar > 0) - 0.5);            // Binary Noise, +-0.5 per sample
    var x = (wave - 1).clip(0, 1);                       // GUESS: scanner crossfade 1..2, white from 2 up
    var sig = ((binary * (1 - x)) + (white * x)) * amp;  // Core 2277: noise * a
    var fenv = ~bitsFilterEnv.(gate, fA, fD, fS, fR);    // A/E: control rate is fine
    sig = ~bitsFilter.(sig, filtType, cut + (env * fenv), reson);
    sig = ~bitsMicroDel.(sig, mdFine, mdFeed, mdPCutoff, mdReson, mdType, mdWet);
    sig = ~bitsDelay.(sig, 1, delaySt, dFine, dFeed, dPCutoff, dReson, dType, dWet);
    sig = ~bitsMicroSpace.(sig, spOn, spSize, spPosition, spMod, spLo, spHi, spMix);
    Out.ar(out, Pan2.ar(sig, pan, level.dbamp));
}).add;
```
Defaults = CLIX snapshot 3 (the panel) and NOIX snapshot 5 (the panel). Persistent synths (one per instrument)
are required so the delay / reverb tails and CLIX's ringing filter survive retriggers.

## 6. Open questions
1. Selector/Scanner mode (1338 CLIX, 2282 NOIX) is not in the module record: CLIX scanner is a strong GUESS,
   NOIX blend between Wave 1..2 is unknowable but unused by the presets.
2. Multi/HP 4-Pole internals (BH4 especially) and the Res→Q law: GUESS; presets only use LP2.
3. Snapshot slot numbering (file order = slot n, 1-based Snp) and what an empty slot / Snp 0 recall does.
4. NOIX Level panel reads −50 while all snapshots store −35 (record parse may be off for that fader).
5. Voice count (assumed 1) and the pan law of Stereo Amp/Mixer.
6. CLIX/NOIX MIDI note collisions (33, 34, 36–39, 41): faithful to the file, probably an editing accident.
