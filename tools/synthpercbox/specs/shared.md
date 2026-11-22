# Bits (Reaktor 6): shared blocks

Source: `~/Downloads/Bits_r6.ens`, extractor report `r6.json`, panel and snapshot decode
`bits_decoded.json` (scratchpad `bits/`). The tables of snapshot values in real units are in
scratchpad `bits/shared_snaps.txt` (normalised value × range).

Scope: the effect, filter, envelope and output blocks that more than one instrument uses. The
generator voices are covered in the other specs.

Conventions used below
- `a.N -> b.M` = output N of a to input M of b. The dump prints Selector inputs by their *name*
  ("0", "1", …). The Pos input has no name and prints as `.0` too. Rule: a control/List/Button
  always goes to **Pos**, audio goes to the named inputs (checked against the raw
  `target_input_index` in the report).
- Snapshot floats are normalised 0..1 → value = r1 + v·(r2 − r1). Snapshot ints on **List** and
  **Switch** controls are **1-based** entry numbers (List entries decoded from the file: HP = 0,
  BP = 1, LP = 2, so snapshot #1 = HP, #2 = BP, #3 = LP). Buttons are stored 0 / 0.5 → off / on.
- The `saved` field in DECODED is not usable as a default. All instruments except CLIX show the
  range midpoint; CLIX shows raw denormals. Take defaults from snapshots.
- Reaktor time laws (module docs + `ugens/OceanodeSteamPipe/PrimaryCompat.h`):
  `timeSec(x) = 0.001 · 10^(x/20)` (0 = 1 ms, 20 = 10 ms, 40 = 100 ms, 60 = 1 s, 80 = 10 s, 100 = 100 s,
  −20 = 0.1 ms). Pitch: `hz = 440 · 2^((P − 69)/12)` (Reaktor's P2F = 8.17741966·2^(P/12)).
- Instrument ids: CLIX 1331, STOMPS 2051, NOIX 2274, QUAD 3069, CRAX 4215, K1 24044, master 132.

## Where each block appears

| Instrument | Chain after the generator | Notes |
|---|---|---|
| CLIX 1331 | Clix → **MicroDel 19791** → **Delay 19768** → **Micro Space 25078** → Stereo Amp/Mixer 19647 | no filter |
| STOMPS 2051 | osc → Wave Selector → **PASS 16776** → **Filter 16756** → **Delay 24369** (+on) → Stereo Amp/Mixer 2107 | **Pitch 2125** drives the osc `f` in Hz |
| NOIX 2274 | noise/osc Selector → **Filter 2284** → **MicroDel 2299** → **Delay 3213** → **Micro Space 19985** → Stereo Amp/Mixer 2325 | Delay `fine` 0..30 ms |
| QUAD 3069 | voices → Voice Combiners → Amp/Mixer ×2 (L,R) → **Micro Space 25296** (two mono reverbs) → Voice Combiners → Out L/R | no pan stage |
| CRAX 4215 | osc (amp from **Amp EnV 4274**) → Selector → **Filter 4280** → HP/LP 1-Pole (DC block) → Amp/Mixer (Level) → **Delay 4359** → single Out | mono out |
| K1 24044 | **Pitch 24056** → Expon(F) → Ramp Osc → **Shaper 24074** → **Base 24078** → **PASS 24357** → **Filter 24091** → **Delay 24112** (+on) → Stereo Amp/Mixer 24148 | |

---

## 1. Delay (ids 19768 CLIX, 24369 STOMPS, 3213 NOIX, 4359 CRAX, 24112 K1)

All five share one core. The Single Delay output is filtered by a Multi 2-Pole and fed back. They
differ in the wet tap, an input gate and the `fine` range: **three variants**.

### Signal flow (exact)

```
Tempo Info (beats/s) -> 1/x -> X 250 (Constant)        = ms per 16th note = 250 / bps
  -> X 'Delay st'  -> + 'fine'  -> Single Delay.Dly (ms)
Delay-line input  = + ( A , Feed · Sel(mode){HP,BP,LP}( Multi2Pole(P='P Cutoff', Res='Reson', In=Single Delay.out) ) )
     A = In                    (CLIX 19768, NOIX 3213, CRAX 4359)
     A = Relay1,2(Ctl='on', In) = In if on > 0 else 0          (STOMPS 24369, K1 24112)
Single Delay.In = delay-line input
Out = Selector/Scanner(Pos='WET'){ in0 = In (dry), in1 = W }
     W = Single Delay.out           (CLIX, NOIX, STOMPS, K1)
     W = In + Single Delay.out      (CRAX 4359)
```

- The output is tapped **at the Single Delay output, before the filter**. The Multi 2-Pole only
  shapes the recirculation (a filtered-feedback echo or comb).
- CLIX 19768 and NOIX 3213 contain an extra `+` (In + Delay) with no output wire. It is dead, which
  explains 22 children. STOMPS and K1 have 23 (Relay + Button `on`, no dead adder). CRAX has 22,
  with the adder wired to the wet input.
- `on` (STOMPS, K1) gates only the **input** into the delay line. The dry path always reaches the
  WET selector, and the tail keeps recirculating when `on` goes off.
- Filter mode: List (entries HP = 0, BP = 1, LP = 2) → Selector Pos → picks the HP/BP/LP output.
- The feedback `X Feed` is linear, 0..1, with no limiter in the loop. Feed ≈ 0.97 with Reson 0 occurs in
  snapshots, so the loop can ring for a long time.

### Panel controls

| label | CLIX sid | STOMPS | NOIX | CRAX | K1 | range | unit / mapping |
|---|---|---|---|---|---|---|---|
| Feed | 167 | 61 | 37 | 67 | 17 | 0..1 | linear feedback gain |
| Delay st | 168 | 62 | 38 | 68 | 18 | 0..16, step 1/16 | 16th notes × (250 / bps) ms. 16 = one 4/4 bar |
| P Cutoff | 169 | 63 | 39 | 69 | 19 | 20..120 | Multi 2-Pole pitch, Hz = midicps(P) |
| Reson | 170 | 64 | 40 | 70 | 20 | 0..0.98 | Multi 2-Pole Res |
| (mode list, label "LP" is a heuristic) | 171 | 65 | 41 | 71 | 21 | HP/BP/LP | snapshot 1-based |
| fine | 172 | 66 | 42 | 72 | 22 | **0..75 ms** (NOIX **0..30 ms**) | added ms |
| WET | 173 | 67 | 43 | 73 | 23 | 0..1 | Selector/Scanner Pos (see open question 1) |
| on | – | 68 | – | – | 42 | button | gates input into the loop |

Delay time = `Delay st · 250 / bps + fine` ms. Reaktor's Tempo Info follows the host or master clock in bps.

### Snapshot values (real units; banks in file order)

- CLIX (sinebits…): Feed .961, st 2.875 (1.0 in one), P 79.1 (101.9), Res .432, LP, fine 9.45 ms, WET 0/0/.409/0/.425/0
- STOMPS (Snap 2–10): Feed .969/.921, st 0, P 70.4 or 120, Res 0, BP or LP, fine **3.54 ms**, WET .433, on mostly off (on in 2 of 10)
  → a tuned comb resonator (≈282 Hz) when `on` is on.
- NOIX: Feed .835/.724, st 2 or 4, P 78.3/103.5, Res .131/0, HP (LP once), fine 0, WET 0/.409/.409/.496/0
- CRAX: Feed .70 (last .92), st 2 (last 0), P 92.4, Res 0, HP, fine 0 (last 30.1), WET 0 except one snapshot .551
- K1: Feed .67/.50/.97, st 3/0/4, P 88.5/75.1/99.5, Res .648/0, LP/BP, fine 0/18.3/21.3, WET 0 then .433, on off/on

The minimum delay seen in any snapshot is 3.54 ms (170 samples at 48 kHz). `Delay st` = 0 with
`fine` = 0 is reachable from the panel, though.

Loop stability warning: CLIX (Feed .96, Res .43) and K1 (Feed .67, Res .65) put Feed × the filter's resonant
peak near or above 1 with a generic RLPF and Q = 1/(2(1 − res)): about 1.03 and 1.07 at the peak. Reaktor has no limiter in the loop.
Whether the exact Primary grid stays below 1 is not checked. If the RLPF approximation runs away, that is
the approximation's fault. Use the exact filter before tuning anything.

Panel values snap to the control's step grid (floor to step, see `k1_stomps.md` §0). The real-unit
values in this spec are unsnapped.

### Module facts / GUESSes

- Single Delay (kind 340): Dly in ms. Interpolation and max-delay **properties are not decoded**.
  GUESS: linear ("Good"). Max must be ≥ 16·250/bps + 75 ms (4.075 s at 60 bpm). Allocate ~4.2 s,
  or size the buffer from the tempo range the node supports.
- Multi 2-Pole (kind 303): Primary SVF with "passband unity, boost at resonance". The exact model
  already exists in the repo: `ugens/OceanodeSteamPipe/PrimaryCompat.h` `niFilterF/niFilterD`
  (11×11 bilinear grid `PrimaryFilterGrid.h`, verified against Reaktor 6.5) with the recurrence
  `hp = in − lp − d·bp; bp += f·hp; lp += f·bp` (see `steamPipeBox.eel2` lines ~219-224).
  `CompensatedSVF` in OceanodePrimitives is the **Core** SVF (d = 2(1 − res)), not this module.
- Feedback loop: Reaktor Primary breaks a feedback loop with one implicit sample (GUESS: +1
  sample on the loop, not on the output tap).
- `OceanodePrimitives/FilteredFeedbackDelay` (from the Organism MicroDelay port) has the same
  topology as variant A: dry/delayed fade, filtered one-sample feedback. Its differences: a 2 s
  buffer (too short for `Delay st`), the Core SVF, Hermite reads and a parabolic `coreFade`, against
  Primary linear (GUESS). It is usable for MicroDel and for short-delay settings. For an exact port, a sibling UGen
  with the Primary Multi 2-Pole, a larger buffer, an input gate and a wet-mode switch would be the
  clean solution.

### Draft SC (block-compensated LocalIn loop; exact loop length only for delay ≥ 1 block)

```supercollider
// Reaktor Multi 2-Pole, approximation until a native Primary SVF UGen exists (GUESS on Q law).
~multi2pole = { |in, p=70, res=0|
    var hz = p.midicps.clip(10, SampleRate.ir * 0.45);
    var rq = (2 * (1 - res)).max(0.04);              // Core-style damping; GUESS for Primary grid
    [RHPF.ar(in, hz, rq), BPF.ar(in, hz, rq) , RLPF.ar(in, hz, rq)]   // HP, BP, LP (BP gain differs: GUESS)
};

// variant: 0 = wet is delay only (CLIX/NOIX/STOMPS/K1), 1 = wet is In + delay (CRAX)
// on: STOMPS/K1 only, 1 elsewhere. tempoBps = beats per second (BPM / 60).
~bitsDelay = { |sig, feed=0.5, delaySt=4, pCutoff=70, reson=0, mode=2, fine=0, wet=0,
               on=1, variant=0, tempoBps=2, maxDelay=4.2|
    var blk = ControlDur.ir;
    var ms = (delaySt * 250 / tempoBps) + fine;
    var t = (ms * 0.001).clip(blk + SampleDur.ir, maxDelay);   // < 1 block cannot be reproduced here
    var fb = LocalIn.ar(1);                                     // filtered delay, one block late
    var lineIn = (sig * on.clip(0, 1).round) + fb;              // Relay: In if on > 0
    var dly = DelayL.ar(lineIn, maxDelay, t);                   // output tap: full delay
    var early = DelayL.ar(lineIn, maxDelay, t - blk);           // same line, one block earlier
    var w = wet.clip(0, 1);
    LocalOut.ar(Select.ar(mode.round.clip(0, 2), ~multi2pole.(early, pCutoff, reson)) * feed);
    // WET: Selector/Scanner. Linear crossfade is a GUESS (see open question 1).
    (sig * (1 - w)) + (w * Select.ar(variant, [dly, sig + dly]))
};
```

---

## 2. MicroDel (ids 19791 CLIX, 2299 NOIX), 15 children, identical

```
Single Delay.Dly = 'fine' (ms, 0..30)  — no tempo part
Delay-line input = In + Feed · Sel(mode){HP,BP,LP}( Multi2Pole('P Cutoff','Reson', Single Delay.out) )
Out = Selector/Scanner(Pos='WET'){ in0 = In, in1 = In + Single Delay.out }
```

The wet tap is **In + delay**, a comb or flanger sum, as in CRAX's Delay. There is no `on` button.

| label | CLIX sid | NOIX sid | range | mapping |
|---|---|---|---|---|
| Feed | 174 | 14 | 0..1 | linear |
| P Cutoff | 175 | 16 | 20..120 | midicps |
| Reson | 176 | 17 | 0..0.98 | Res |
| mode list | 177 | 18 | HP/BP/LP | 1-based snapshot |
| fine | 178 | 19 | 0..30 ms | delay ms |
| WET | 179 | 20 | 0..1 | Scanner Pos (GUESS linear) |

Snapshots: CLIX Feed .638/.929, P 86.1/101.9, Res 0, LP, fine 9.21/14.88 ms, WET .614/0/.614/1/0/0.
NOIX Feed .638 (.370), P 45.2/32.6/120, Res .309/0, HP (LP once), fine 17.95/14.88/30, WET .70/1/0.

```supercollider
~bitsMicroDel = { |sig, feed=0.5, pCutoff=70, reson=0, mode=2, fine=15, wet=0.5|
    var blk = ControlDur.ir;
    var t = (fine * 0.001).max(blk + SampleDur.ir);       // fine < ~1.4 ms not reproducible this way
    var lineIn = sig + LocalIn.ar(1);
    var dly = DelayL.ar(lineIn, 0.04, t);
    var early = DelayL.ar(lineIn, 0.04, t - blk);
    LocalOut.ar(Select.ar(mode.round.clip(0, 2), ~multi2pole.(early, pCutoff, reson)) * feed);
    (sig * (1 - wet)) + ((sig + dly) * wet)                // linear Scanner: GUESS
};
// Closer native option: FilteredFeedbackDelay.ar(sig, fine, feed, pCutoff.midicps, reson, mode, wet)
// but its wet is the delay only (not In+delay), its SVF is Core-style and its fade is parabolic.
```

---

## 3. Filter + Filter Env (16756 STOMPS, 2284 NOIX, 4280 CRAX, 24091 K1)

```
Filter Env macro (7 children) = ADSR(G = gate In, A, D, S, R) -> Out
A/E (audio→event at control rate, 400 Hz default)  -> X 'Env'  -> + 'Cut' -> Multi 2-Pole.P
Multi 2-Pole(P, Res='Reson'/'Res', In) -> mode select -> Out
```

- P = Cut + Env · ADSR. Env is ±120 semitones at ADSR = 1. The ADSR peak equals the gate value.
  For preset and button triggers that is 1. CRAX also merges a Sel. Note Gate, whose peak is velocity.
- The modulation runs at **control rate** (A/E), so it is stepped at about 400 Hz. Use `kr` or a K2A ramp.
- Mode selection differs:
  - STOMPS 16756 and NOIX 2284 use a panel **Switch** with inputs named HP/BP/LP. Snapshot ints are
    1-based: STOMPS sid38 = 3,3,2,3,3,3,2,1,1,1 and NOIX sid13 = 3 always (LP). DECODED labels them "6" and "None", which is wrong.
  - CRAX 4280 and K1 24091 use a **List + Selector** (HP = 0, BP = 1, LP = 2). CRAX sid66 is LP or HP. K1 sid43 is LP or HP.
- Gate source: STOMPS gets Order#24393 output 1 (first, before Pitch and the amp ADSR). NOIX gets M#2323
  (port `t`). CRAX gets M#4234 (port 1). K1 gets M#24136 (G).
- CRAX's Filter feeds an **HP/LP 1-Pole with P = 1.0** (8.66 Hz), taking the HP output: a DC blocker,
  then the Amp/Mixer Level.

| control | STOMPS | NOIX | CRAX | K1 | range | mapping |
|---|---|---|---|---|---|---|
| Cut | 39 | 52 | 39 | 11 | 20..120 | pitch |
| Reson / Res | 37 | 12 | 40 | 9 | 0..0.98 | Multi 2-Pole Res |
| mode | 38 (Switch) | 13 (Switch) | 66 (List) | 43 (List) | HP/BP/LP | 1-based |
| Env | 44 | 57 | 106 | 16 | −120..120 | semitones × env |
| Attack time | 40 | 53 | 91 | 12 | −20..100 | timeSec log |
| Decay time | 41 | 54 | 92 | 13 | −20..100 | log time constant |
| S | 42 | 55 | 102 | 14 | 0..1 | sustain ratio |
| R | 43 | 56 | 103 | 15 | 0..80 | log time constant |

ADSR (kind 272), as already ported in `EventADSRKernel` (OceanodePrimitives) and SteamPipe "Original ADSR":
linear attack to the peak, then **exponential** decay and release as one-pole *time constants*
(`rate = 1/(timeSec·sr)`). Retrigger starts from the current level. Times are in ms for the UGen:
`EventADSRKernel.ar(trig, gate, timeMs(A), timeMs(D), S, timeMs(R), peak)`.

Snapshot values (STOMPS): Cut 120/81.4/68/20/75.9, Res 0/.72/.76, Env 0.94 / −120 / +120, A 35.8/60.3,
D 7 (≈2.2 ms), S 1, R 36.5. NOIX: Cut 94.8/79.1/90.9, Res .81/0, Env 120 (0.94 once), A 100 (−20), D 100,
S .83/1, R 59.2/0. K1: A 47.1/−20, D 7, S .48, R 69.3/0, Env 0.94..120, Cut 51.5..120, Res 0..0.56.
CRAX: Cut 120/20/88.5, Res .52/0/.31, A −20, D 100 (30 once). S, R and Env (sids 102, 103, 106) appear in only 3 of 16
snapshots (S .22, R 51.7, Env 120), so their value in the other snapshots depends on recall order (open question).

```supercollider
~timeMs = { |x| 10 ** (x / 20) };                       // Reaktor log time -> ms
~bitsFilter = { |sig, gate, cut=70, reson=0, mode=2, env=0,
                 attack=40, decay=40, sustain=0.5, release=40, peak=1|
    var trig = Trig1.ar(K2A.ar(gate) > 0, SampleDur.ir);
    var e = EventADSRKernel.ar(trig, K2A.ar(gate), ~timeMs.(attack), ~timeMs.(decay),
                               sustain, ~timeMs.(release), peak);
    var p = cut + (env * A2K.kr(e));                    // A/E: control-rate modulation
    Select.ar(mode.round.clip(0, 2), ~multi2pole.(sig, p, reson))
};
// CRAX only, after the filter:  HPF 1-pole at midicps(1) = 8.66 Hz:  sig - OnePole.ar(sig, exp(-2pi·8.66/sr))
```

---

## 4. PASS (16776 STOMPS, 24357 K1), identical, 10 children

```
Rectify/Sign(In).|x| -> + (DRIVE · DRIVE) -> Divide: Out = In / (|In| + DRIVE²)
Clipper(In = Divide output, Max 0.5, Min −0.5)  — its output is unwired (dead)
```

- DRIVE range is **reversed**, 2 → 0.1. A small DRIVE gives hard saturation: the output tends to sign(x),
  and small signals get gain 1/DRIVE² (up to 100× at 0.1). A large DRIVE is quiet and nearly linear
  (gain 0.25 at DRIVE 2).
- STOMPS sid45: 0.50/0.88/0.1… (mostly 0.1). K1 sid51: 2/0.22/0.1/1.37/1.16.

```supercollider
~bitsPass = { |sig, drive=1.05| var k = drive.squared; sig / (sig.abs + k) };
```

---

## 5. Pitch

### STOMPS Pitch 2125 (linear Hz into Core Cell 2054 `f`)

```
In (trigger, Order#24393 output 2) -> D#2128(A = 1, D = 'Decay' sid27) -> X 'Penv1' (sid32)
                                   -> D#2135(A = 1, D = 'Decay' sid30) -> X 'Penv2' (sid31)
f_Hz = 'base F' (sid29, 0..250 Hz) + 'Higher F' (sid28, 0..2000 Hz) · (Penv1·D1 + Penv2·D2)
```

There is **no p2f** here: the oscillator takes Hz directly. D (kind 264) is a decay envelope triggered on
a rising edge. It jumps to A and decays with the log time (same scale as the ADSR, 0..80 → 1 ms..10 s;
GUESS: exponential time constant like the ADSR decay).
Snapshots: Decay1 17.6/0/36.5, Decay2 0/30.9/29/63, Higher F 2000/362/0, base F 33/0 Hz, Penv1 mostly on, Penv2 on in 5 of 10.

### K1 Pitch 24056 (semitones → Expon(F) outside the macro)

```
Fader#24059 (0..127, NO snapshot id) -> Core Cell 24072 = p2f: 2^((x−69)/12 + log2 440)
  -> Log (F→P) => ≈ x (round trip, kept only for the Meter display)
P = x + Penv1 · D1(A = 'Amt' sid2, D = 'Decay' sid1) + Penv2 · D2(A = 'Amt' sid4, D = 'Decay' sid3)
macro Out -> Expon. (F) -> Ramp Osc F
D trig = gate G (M#24136)
```

- Amt is 0..68 semitones of sweep at the trigger, decaying to 0.
- The base note fader 24059 has no `snapshot_control_id` in the report, most likely because it is
  **sid 0** (falsy). Snapshot key `0` in the K1 bank = 0.2598 → **33** (A1, 55 Hz) in all 9 snapshots,
  matching `k1_stomps.md`. The 63.5 in the report is the midpoint placeholder.
- Snapshots: Decay1 15.8/7.6/32.1/24.6/28.4/0, Amt1 35.3/68/28.9, Decay2 36.5/7.6/0, Amt2 68, Penv1 on, Penv2 on from snapshot 3.

```supercollider
// Reaktor D: on a rising edge jump to amp (restart, not add), then x = amp·exp(−t/τ), τ = timeSec(dec).
// The time-constant law is a GUESS shared with k1_stomps.md. Silent until the first trigger.
~decayEnv = { |trig, amp=1, dec=40|
    var t = Sweep.ar(trig, 1);
    amp * exp(t.neg / (~timeMs.(dec) * 0.001)) * (PulseCount.ar(trig) > 0) };
~stompsPitchHz = { |trig, decay1=40, higherF=1000, baseF=125, decay2=40, penv2=0, penv1=0|
    baseF + (higherF * ((penv1 * ~decayEnv.(trig, 1, decay1)) + (penv2 * ~decayEnv.(trig, 1, decay2)))) };
~k1PitchHz = { |trig, note=60, decay1=40, amt1=34, decay2=40, amt2=34, penv1=1, penv2=0|
    (note + (penv1 * ~decayEnv.(trig, amt1, decay1)) + (penv2 * ~decayEnv.(trig, amt2, decay2))).midicps };
```

Buttons are stored 0 / 0.5 in snapshots. GUESS: on outputs 1.0 (range max), the same assumption as `k1_stomps.md`.
If Reaktor really outputs 0.5, every Penv contribution halves.

### K1 Shaper 24074 and Base 24078

- Shaper: `Sine` math module, `sin(2π·x)` with input 1.0 = a full period. Ramp Osc (0..1, Snc = gate,
  so the phase resets on each trigger) → a sine oscillator starting at phase 0.
- Base: `Ramp Osc(F = 1/9001 Hz, A = 9001, Snc = G)` gives elapsed **seconds since the trigger**.
  Then `× 'Decay'` → negate → Expon(A) (dB → amplitude) → `× In × 'Vol'`.
  Out = In · Vol · 10^(−Decay·t/20). This is an exponential decay at **Decay dB per second**. The range is
  reversed, 600 → 1 dB/s (sid7: 24.6/161/124/449/336/600). Vol is sid8 (1.0 in all snapshots).

```supercollider
~k1Base = { |sig, trig, decayDbPerSec=300, vol=1|
    var t = Sweep.ar(trig, 1);                       // seconds since gate (Ramp Osc 1/9001 Hz × 9001)
    sig * vol * (10 ** (decayDbPerSec.neg * t / 20)) };
```

---

## 6. Amp EnV 4274 (CRAX)

`ADSR(G = M#4234, A = 'Attack time' sid33, D = 'Decay time' sid34, S sid104, R sid105)` → Core Cell 4217 (MultiWave Osc) `A`.
Same ADSR law as the Filter Env. Snapshots: A −20 (0.1 ms), D 100 (30 once). S .063 and R 30.9 appear in only 3 of 16 snapshots.
The gate peak is the gate value: 1 from presets/button, velocity from the Sel. Note Gate path (M#6145).

---

## 7. Micro Space (25078 CLIX, 19985 NOIX, 25296 QUAD)

### Structure

```
In -> Core 'Space Master Deluxe' (In L) -> L -> Sw.2 port 1A
In -----------------------------------------> Sw.2 port 2A
Sw.2 = Selector(Pos = 'on'){ in0 = 2A (dry), in1 = 1A (processed) } -> Out
Faders -> 'Master Adaptor' (58 children) -> 21 cell parameters
```

- The **cell is mono**. Its only audio input is `In L`. Its outputs are `L` and `LT`, and LT is unwired.
  Inside, one side (Size − Sym, ERSym −2, LtSym +1.5) runs predelay → 3 early diffusers → late
  diffusion (9, 6, 3) → shelf → Delay H tank × RT60 gain → `FB` → z⁻¹ (`root/m26/m17`).
- **The tank feedback appears to be unconnected** (full netlist of `--cell 25093 --net root/m26`, 60 lines).
  The z⁻¹ writes `qbus18` and nothing reads it. The late-input adder `m16` reads `qbus12` (the input) + `qbus19`,
  and nothing writes `qbus19`. The 8Phase outputs #4–#7 (`qbus7`–`qbus10`, the other side's modulation) are
  also written and never read. This is exactly what the stereo cell (L reads R's cross-feed bus) looks
  like with the R side deleted. Read literally, the Bits Micro Space has **no tank recirculation**: its tail
  is only the early and late diffusers' own allpass loops (g = 0.67, damped). Size then changes diffuser
  delays and predelay but **RT60 has no effect**. GUESS until it is confirmed in Reaktor (impulse render at
  Size 100: a short dense tail confirms it; a tail of seconds means the extractor mis-numbered the
  QuickBus). This is open question 2.
- The Master Adaptor constants are identical to `microSpace.scd`'s header: PreDel = Size·0.00022 + 0.002 s,
  ErSz 60 − 0.42·Size, LtSz 48 − 0.72·Size, Dffs 0.67, RT60 10^(Size/20) ms, damping min(0, Hi)·0.5 /
  min(0, Lo)·0.72 at P108/P36, EQ Hi/Lo dB at P108/P36, ModF = ModA = Size·Mod·0.01, E/L = Position, D/W = Mix.
- `on` is a **hard switch** between the dry input and the processed output. When off, the output is
  dry; the cell keeps running and its own D/W mix is bypassed. This differs from boxMicroSpace's `on`, which
  gates the reverb input and lets the tail ring out.
- QUAD 25296 is **two independent mono instances**: In (L) → cell 25311, In 25506 (R) → cell 25446,
  each with its own Master Adaptor and its own Sw.2 (`on` sid60 and sid68), sharing the six faders. It
  is not a stereo pair.

### Control map (all three: Size 0..100, Mod 0..1, Lo/Hi ±12 dB, Position 0..1, Mix 0..1)

| label | CLIX sid | NOIX sid | QUAD sid | OceanodeMicroSpace.ar arg |
|---|---|---|---|---|
| Size (label in DECODED is its tooltip "Room size and reverberation time.") | 264 | 58 | 54 | `size` |
| Mod | 265 | 59 | 55 | `mod` |
| Lo | 266 | 60 | 56 | `lo` |
| Hi | 267 | 61 | 57 | `hi` |
| Position | 268 | 62 | 58 | `position` |
| Mix | 269 | 63 | 59 | `mix` |
| on | 270 | 108 | 60 / 68 | outer `Select` (dry vs processed), not an arg |

Map to the **native** `OceanodeMicroSpace.ar` arguments (identical names and units). Do not use `boxMicroSpace`: its arguments
are the legacy Box adapter (Size 0..60, decay, predelay…).

Snapshots: CLIX Size 73.2, Mod 0, Lo 0.09, Hi −5.0, Pos 1, Mix .433, on on/off. NOIX Size 73.2, Mod 1/0,
Lo 0.09, Hi 0.09/8.03, Pos 1, Mix .30, on mostly off. QUAD Size 65.4/78/66.9, Mod 1, Lo 0.09/−12, Hi 0.09,
Pos 1, Mix .386, both `on` move together.

### Can the existing def be used as is?

**No.** `OceanodeMicroSpace.ar` always builds an L/R pair whose tanks cross-couple, L tank into R and
R tank into L, so its tail is long and governed by RT60. Bits' cell is one side, and per the netlist above
it has no tank return. Called with one channel, `ar` duplicates the input to both sides. Called with
`[sig, 0]`, R still sits inside L's loop. Either way it is much longer and denser than the Bits cell.

The faithful port is a **mono variant** of the same class:
- side 0 only (Size − Sym pitches, modulation `phases` not negated);
- predelay → early diffusers → damping shelf → late diffusers fed by `early` only (`tankIn = 0`; the tank
  delay and RT60 gain are dropped because their output is unread);
- E/L crossfade → EQ shelves → D/W crossfade.

All pieces (predelay, diffusers with their damping, P2T, Smooth, 8Phase #0–#3, parabolic crossfades) are
already in `OceanodeMicroSpace` and can be reused, e.g. `OceanodeMicroSpace.mono(input, size, mod, lo, hi,
position, mix)`. If Reaktor shows a long tail instead (open question 2), the variant is the same with
`late input = early + tankIn[same side]` (self-feedback through the z⁻¹; keep the `+1 − block` compensation).
Until `.mono` exists, `OceanodeMicroSpace.ar([sig, sig], …)[0]` is only a stand-in with a much longer tail.

```supercollider
~bitsMicroSpace = { |sig, size=50, mod=0.5, lo=0, hi=0, position=0.5, mix=0.5, on=1|
    var wet = OceanodeMicroSpace.ar([sig, sig], size, mod, lo, hi, position, mix)[0]; // stand-in for .mono
    Select.ar(on.clip(0, 1).round, [sig, wet])   // Sw.2 hard switch (Selector, button 0/1)
};
// QUAD: two calls, one per channel, same faders, onL = sid60, onR = sid68.
```

GUESS (port order): Sw.2's terminals are named 1A (record 0) and 2A (record 1). In#25088 → selector in1
and In#25089 → in0. Mapping In#25088 = 1A (processed) and In#25089 = 2A (dry) is taken from child order. It agrees
with on = processed, which the snapshot usage supports.

---

## 8. Output stages and master 132

| Instrument | Stage | Level fader | Pan | Into 132 |
|---|---|---|---|---|
| CLIX | Micro Space → Stereo Amp/Mixer 19647 → Voice Combiner per side → Out L/R | `Level` sid138, −50..−20 dB (snap −20) | `Pan` sid137 −1..1 (snap ≈ +0.03) | L → +1094.1, R → +1878.1 |
| STOMPS | Delay → Stereo Amp/Mixer 2107 → AVC → Out | `Level` sid23 −50..−20 (snap −29..−36) | `Pan` sid21 (≈ +0.04) | +1094.3 / +1878.3 |
| NOIX | Micro Space → Stereo Amp/Mixer 2325 → AVC → Out | `Level` sid23 **−50..−35** (snap −35) | `Pan` sid21 (≈ −0.008) | +1094.4 / +1878.4 |
| QUAD | X 3209/3210 → AVC → **Amp/Mixer 3191/3192** (`Level` sid18 −60..0 dB, snap −33.5/−35.9, one fader for both) → Micro Space → AVC → Out L/R | sid18 | none (the stereo comes from the voice, "offset L/R") | +1094.5 / +1878.5 |
| CRAX | Filter → HP 1-pole (P 1) → **Amp/Mixer 4272** (`Level` sid24 −60..−28, snap −40.4/−31.3, then absent) → Delay → **one Out** | sid24 | none | the same mono signal to **both** +1094.2 and +1878.2 |
| K1 | Delay → Stereo Amp/Mixer 24148 → AVC → Out | `Level` sid27 −50..−20 (snap −20..−36) | `Pan` sid26 (no snapshot values) | +1094.0 / +1878.0 |

- Amp/Mixer and Stereo Amp/Mixer `Lvl` are in **dB** (`dbamp`). The Stereo Amp/Mixer pan law is **not
  documented** in the catalog. The Stereo Pan module doc gives centre L = R = In. GUESS: the same
  law, `L = In·min(1, 1 − pan)`, `R = In·min(1, 1 + pan)`, i.e. centre is unity on both sides.
  All snapshot pans are within ±0.04 of centre, so the law barely matters.
- **Master 132** has two adders: `+1094` sums all instruments' output 0 (L) and `+1878` all output 1 (R). Each
  goes through an Audio Voice Combiner to Out#897 (L) and Out#135 (R). The top level has
  Voice Combiners 16744/16747 and Outs 2497/2498. **No master level, limiter or effect.**
- **Voice count: not decoded.** The instrument records in the report carry no voice field, and every
  instrument has the identical raw header. Every trigger path is monophonic (Sel. Note Gate, buttons,
  merges), and the Organism port found one voice in a similar ensemble. GUESS: 1 voice each, so the
  Voice Combiners are pass-throughs.

### Trigger path (presets macro, e.g. CLIX 24804; all instruments the same)

```
MIDI sub-macro: Sel. Note Gate k (k = 1..14, notes e.g. 43-58 for CLIX) -> Separator(Thld 0):
     Hi -> Value(k)  ;  Lo (note-off, 0) -> passes 0          | Buttons '1'..'14' send k / 0
  -> M (merge) -> Order:
       1 -> Snapshot.Snp = k (Bnk = Constant 1)
       2 -> Snapshot.Recl (positive k recalls; 0 on note-off does nothing)
       3 -> Compare(k > 0) -> Out  = 1 on note-on, 0 on note-off
```

- The snapshot is recalled **before** the gate fires. The gate is a held 0/1 with velocity discarded.
- Per instrument, the presets Out goes to:
  - CLIX: the Clix macro input 0.
  - STOMPS: M#2102 → Order#24393, which sends 1 → Filter G, 2 → Pitch trig, 3 → amp ADSR G.
  - NOIX: M#2323 → ADSR, Filter `t`.
  - QUAD: M#3197 → ADSR, Separator, macro 25534.
  - CRAX: M#4234 (also merged with the Trigger button and Sel. Note Gate via M#6145) → Amp EnV G, Filter gate, LFO Rst and others.
  - K1: M#24136 → Pitch G, Ramp Osc Snc, Base G, Filter G.
- Edge-triggered blocks (D envelopes, Ramp Osc sync, Base reset) fire on 0 → 1. The ADSRs hold while the gate is 1
  (note length matters for S and R).

---

## Open questions

1. **WET law** (Delay, MicroDel): Selector/Scanner "Curve" is a property that is not decoded. The WET selector's raw
   module bytes equal the on/off Sw.2 selector's (a hard switch). Fractional WET snapshot values (.41, .43,
   .61, .70) argue for a **linear Scanner crossfade**. If it is a hard Selector, every WET < 1 is fully dry and
   only WET = 1 is heard (CLIX MicroDel and NOIX MicroDel have WET = 1 in one snapshot each). This decides whether the
   delays are audible at all. For STOMPS it also sets the level: with `on` off (8 of 10 snapshots) and
   WET .433, a linear Scanner leaves only 0.567 × dry (−4.9 dB). A hard Selector leaves the full dry.
   Check in Reaktor: Properties of Selector 19789 (Curve None/Linear/Sine).
2. **Micro Space tank feedback**: the netlist shows the tank return bus unread (see §7). Confirm by an impulse
   render in Reaktor before building `OceanodeMicroSpace.mono`.
3. Single Delay interpolation and max-delay properties are not decoded.
4. Stereo Amp/Mixer pan law.
5. CRAX sids 102–106 (Filter Env S/R, Amp EnV S/R, Filter Env amount) are stored in only 3 of 16 snapshots.
   The others keep whatever was last recalled. Decide on a default, e.g. the values above.
6. Voice counts (GUESS 1).
7. Implicit one-sample delay in Primary feedback loops (Delay/MicroDel). The exact Multi 2-Pole needs a native
   UGen (`PrimaryCompat.h` already has the maths). The RHPF/BPF/RLPF draft is an approximation.
8. Button "on" value (stored 0.5): GUESS 1.0.
