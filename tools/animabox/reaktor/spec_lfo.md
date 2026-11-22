# AnimaMundi: spatial LFOs ("LFO BAND ENERGY" #6808, "LFO Q" #7649) + Band Energy / Band Q / Q

Notation: `v` = Reaktor voice = band, 1-based (Voice Info.V). `nv` = **Bands** fader #5909 (0..120), received
through Send#5908 → Receive#7645/#7131/#12777 (LFO Q: #7925/#7671/#12819). Confirmed by elimination:
the other Sends in Instrument#3 carry a Harmony clock router, a Markov merge and MIDI IN.
`frac(x) = x - floor(x)` (Reaktor Modulo with B=1, always ≥ 0).
Snapshot values below are real values. Exception: faders that controls.txt lists as `[0..0]` store
**normalized** values (the extractor had no range). Their real ranges, read from the record at
+0x5c/+0x60, are: pow faders 7609/12788/7889/12830 → **-1..1**, so real = -1 + 2·norm; `+!` #12817 → **0..0.002**.

## 1. Top level of LFO BAND ENERGY #6808

```
inputs : idx   (port 0) <- Pitch Index#12294 out1 'idx'
outputs: G     (port 0) = Gated.G   -> NOT CONNECTED at parent (unused)
         LFO   (port 1) = Gated.OutG -> BP Index Behavior Out 'LFO' -> Filters#19 In 'LFO' (band amplitude); also Band Energy display
         F2    (port 2) = LFO.F2 (LFO frequency Hz) -> NOT CONNECTED (unused)

idx_v  = vIndex(v)                    // macro v-1#7392, §2
A_v    = LFO(idx_v, nv, idxPitch_v)   // macro LFO#7426, §3
x_v    = A_v * (v <= nv ? 1 : 0)      // Compare#7647 + X#7648: voices above Bands are muted
out_v  = Gated(x_v)                   // §5
```
`idx` input (Pitch Index out1) = Out#15258 = PitchIndex_out0 / (Receive#12467 · Step#12297), a normalized
pitch-rank position of the band, about 0..1 (that macro is decoded in the Pitch Index spec). In this
section it is used **only** by Damp (§3.9).

display#7112 (XY#7122) is **visual only**: a bar per band with X1 = 9 + ((v-1)/nv·127 mod 127),
X2 = X1 + 127/nv, Y2 = out_v. v-1#7125 (V-1, Vcs) feeds only this display.

## 2. Band index transform: v-1 #7392 (LFO Q: #7672)

Output port 0 'v-1' (Quantize#7416) → LFO.v-1. The `nv` output is a pass-through of the vcs input (= nv) and is unused.

| id (BE / Q) | control | range | role |
|---|---|---|---|
| 7400 / 7680 | Button | 0/1 | reverse index direction |
| 7402 / 7682 | Fader (N) | 0..121 | index modulo: pattern repeats every N bands (121 = off) |
| 7404 / 7684 | Sym | 0..16 | number of mirror (triangle-fold) segments, 0 = off |
| 7410 / 7690 | Rndm | 0..40 | per-band random index deviation, ±Rndm bands |
| 7417 / 7697 | iQ | 0..32 | index quantization step, 0 = off |

```
u   = Button ? (nv - (v-1)) : v            // Relay#7399: Ctl>0 -> port '1'. Both forms are 1-based.
W   = (Sym <= 0) ? nv : nv / Sym           // Relay#7424 (Compare Sym<=0)
q   = floor(u / W);  m = u - q*W           // Modulo#7403
f   = | (q mod 2)*W - m |                  // triangle fold: even segment -> m, odd segment -> W-m
f   = f mod N                              // Modulo#7401 (N = Fader 0..121)
r   = f + Rndm * U(-1,1)_v                 // Randomizer#7412. U_v is per band, redrawn only when
                                           //   Sym/W, Rndm, Button or N changes (Merge#7413 -> Value#7411)
idx = (iQ > 0) ? iQ * round(r / iQ) : r    // Quantize#7416, St = iQ. Round vs floor UNCERTAIN; St=0 = pass-through (assumed)
```
Snapshot values: Button 0 (BE: 1 in snapshots 4-6 and 9-18; LFO Q: 0 everywhere), N 121, Sym 0, iQ 0, Rndm 0
(BE: 40 in snapshots 2, 3, 7, 18). With these defaults idx = v.
Everything here is **static per band** and recomputed only on parameter change.

## 3. LFO #7426 (LFO Q: #7706). Inputs: v-1 (= idx_v), nv, idx (Pitch Index; BE only)

### 3.1 Controls (BE id / LFO Q id)
| BE | Q | label | range | role |
|---|---|---|---|---|
| 7439 | 7719 | Len (len ctrl) | -1..1 | spatial wavelength: number of LFO cycles across the bank (§3.2) |
| 7465 | 7745 | BPM* | 1..16 | rate multiplier |
| 7467 | 7747 | (Div) | 1..256 | rate divider (through Event Smoother) |
| 7488 | 7768 | Frn | 0..1 | per-band random frequency offset, in Hz |
| 7507 | 7787 | ph+ | -1..1 | global phase offset in cycles (all snapshots -0.00473) |
| 7523 | 7803 | phase-source switch | 1 = Line (one-shot per note), 2 = free ramp | all snapshots: 2 |
| 7474 | 7754 | 'gate sync' (1-input switch) | on/off | note-on resets the ramp. Not in snapshots; saved raw int 2, so likely ON (UNCERTAIN) |
| 7495 | 7775 | 'gate sync' (1-input switch) | on/off | note-on triggers a new random S&H value. Same storage, likely ON (UNCERTAIN) |
| 7571 | 7851 | wave mode | 1 determinisitic, 2 random, 3 brownian, 4 monobrownian | §3.4 |
| 7525 | 7805 | skew | -1..1 | deterministic phase skew |
| 7530 | 7810 | round | 0..2 | 0 tri, 1 sine, 2 square |
| 7561 | 7841 | pwm | -0.49..0.49 | deterministic phase clip / compress |
| 7609 | 7889 | pow | -1..1 (stored norm.) | value tension curve |
| 7452 | 7732 | Q | 0..1, step 1/16 | value quantization step |
| 7454 | 7734 | Scale | -1..1 | amplitude |
| 7456 | 7736 | off | -1..1 | offset |
| 7480 | 7760 | (Smooth ms) | 0.0001..4000 | 1-pole LP smoothing time in ms: fc = 1000/T Hz |
| 7506 | 7786 | G "Midi gated" | 1 = gated, 2 = free | all snapshots: 2 |
| 7595 | 7875 | VibA (vib) | 0..16 | frequency wobble depth in Hz |
| 7598 | 7878 | Vibf (vib) | 0.0001..4 | frequency wobble speed in Hz |
| 12788 | 12830 | Damp pow | -1..1 (stored norm.) | damping curve |
| 12810 | 12852 | Damp Button | 0/1 | damping direction |
| 12815 | 12854 | Damp switch | 1 = damp on, 2 = off (×1) | |

### 3.2 len ctrl #7437 (LFO Q: #7717)
```
len = (Len <= 0) ? (Len + 1)                 // continuous 0..1 cycles across the bank
                 : Q1(15*Len + 1)            // integer 1..16 cycles (Quantize St=1; round assumed, floor possible)
```
Logic NOT: out0 = (Len<=0), 'Not' = (Len>0). Meter#7450 is display only.
Snapshot 0: Len 0.307 gives 15·0.307+1 = 5.6, so len = 6 (5 if floor).

### 3.3 Frequency and phase (per band, time-varying)
```
F_v(t) = (tempoBPS / smooth(Div)) * BPMmul      // Tempo Info = beats per second (BPM/60); Event Smoother ~100 ms (UNCERTAIN: record has 200 and 100)
       + Frn * r_v^2                            // r_v = |U(-1,1)| per band, redrawn whenever the Frn fader sends an event
       + VibA * slowrand_v(t; Vibf)             // §3.7
ramp_v(t): Ramp Osc#7573 (A=1, Ph=0): d(ramp)/dt = F_v, wraps 1 -> 0
   reset to 0 by Merge#7481 events > 0:
     - transport Start/Continue (Start/Stop.G = 1)   -> all bands re-aligned
     - Frn set to exactly 0 (Compare/Equal Frn==0 emits 1, also at init)
     - note-on of that band's voice, if gate sync 7474 is on
ramp is sampled at control rate by A/E#7575 (Reaktor control rate, default 400 Hz: UNCERTAIN for this ensemble)

phSrc = (switch7523 == 2) ? ramp_v(t)
       : Line_v(t)     // Line core #7518: on note-on (Gate>0) ramps 0->1 linearly over 1/F_v s, then holds 1 (one sweep per note)

p_v(t) = frac( ph+ + phSrc + len * idx_v / nv )   // +#7508, +#7434, Modulo#7457
c_v    = p_v - 0.5                                 // -#7461 -> MultiWave core 'Rate' (actually a phase) and deterministic.ph
```
Spatial meaning: with all F_v equal, band v lags by len·idx_v/nv cycles, so `len` whole cycles are spread over the bank.

### 3.4 Waveform w_v ∈ [-1,1] (Switch#7571)
**1 = determinisitic** (macro #7524; cores 7560 pwm, 7526 skew, 7527 tri):
```
x  = c + 0.5 = p
x1 = (clip(x, pwm, 1-pwm) - pwm) / (1 - 2*pwm)          // core 7560. pwm>0: flat 0/1 portions at the cycle ends; pwm<0: range compressed, step at the wrap
s  = (skew + 1)/2
y  = x1 <= s ? 0.5*x1/s : 0.5 + 0.5*(x1 - s)/(1 - s)     // core 7526: piecewise-linear phase warp, s -> 0.5
tri  = y <= 0.5 ? 2y : 2 - 2y                            // core 7527
rcos = 1 - (cos(2π y) + 1)/2                             // Sine/Cosine (input 1 = one cycle). 0 -> 1 -> 0, same phase as tri
L  = scan(round; tri, rcos)        // Selector/Scanner#7529, Pos = round: lerp(tri, rcos, clamp(round,0,1))
e  = scan(2 - round; 0, 1)         // inside xˆy#7538: e = clamp(2 - round, 0, 1)
w  = sign(2L-1) * |2L-1|^e         // xˆy output (z+1)/2, then X2 -1 (#7565/#7567)
```
round 0 gives a triangle, 1 a raised sine, 2 a square (e = 0). Values in between morph.
UNCERTAIN: the Selector Curve property (linear Scanner vs hard Selector) is not decoded. Linear is assumed
because round is a fine 0..2 fader; a hard selector would make round > 1 an instant square.
w = -1 at p = 0, +1 at p = 0.5 (with skew = 0, pwm = 0).

**2 = random** (MultiWave LFO core #7460 output 'Rnd'): sample-and-hold, per band.
- New value on each trg event. trg = Step Filter (changes only) of `p > 0.5`, so **two new values per cycle** (at p = 0.5 and at the wrap).
- Also a new value on note-on (Separator Gate>0 → switch 7495) if that gate sync is on.
- Value = LCG(seed): `state = seed*1103515245 + 12345` (int32 wrap); `w = state * 4.6567e-10`.
- seed = round((p>0.5) + U(-10,10)), rewritten **every control tick** (Randomizer#7471, Rng 10; the seed pin is int).
- So w is one of 22 fixed levels (seed -10..11): .861 -.625 -.111 .403 .917 -.569 -.056 .458 .972 -.514 0 .514 -.972 -.458 .056 .569 -.917 -.403 .111 .625 -.861 -.348.
- The 22-level result assumes Compare re-emits on every control tick (its input is a control-rate event
  stream) and that Randomizer is uniform ±Rng.
- Port: `w = uniform(-1,1)` drawn at p = 0 and p = 0.5 is a faithful-enough equivalent.

**3 = brownian** (core output 'BRWN2'): smooth random, per band.
- On each upward wrap of p (once per cycle), draw a new point `y_n = LCG(int(seedFloat*32768))*4.6567e-10`, effectively uniform ±1.
- Keep the last 4 points. Output = 4-point Hermite (Catmull-Rom) interpolation from y_{n-2} at p = 0 to y_{n-1} at p → 1:
  `out = t0 + f(c1 + f(f·c3 - (c1 + t0 - t1 + c3)))`, with c1 = ½(t1 - t_-1), c3 = ½(t2 - t_-1) + 1.5(t0 - t1) and f = 1 - p.
  Taps from the decode: t_-1 = y_n (newest), t0 = y_{n-1}, t1 = y_{n-2}, t2 = y_{n-3}.
  So at p = 0 (f = 1) out = y_{n-2}, and at p → 1 (f → 0) out = y_{n-1}: continuous at the wrap.
- Overshoot of the cubic is clipped later (§3.5).
- Port: a cubic-interpolated random sequence advancing one point per LFO cycle (≈ LFNoise3 locked to the phase), one cycle of lag.

**4 = monobrownian** (transverse #7580): every band plays **band 1's** brownian signal, delayed.
```
w_v(t) = brown_1(t - D_v),  D_v[ms] = len * (1000 / F_v) * idx_v / nv      // From Voice(1) -> Single Delay (ms)
```
With equal F this is brown_1 evaluated len·idx_v/nv cycles earlier: one random curve travels across the bank.
Port idea: one shared random table indexed by band 1's unwrapped cycle count Φ1. Then
`w_v = HermiteTable(Φ1 - len*idx_v/nv)`, with no delay buffer.
UNCERTAIN: the maximum delay of the Single Delay module (long delays at slow rates may be truncated).

### 3.5 Value chain (per band, control-rate events)
```
w  = clip(w, -1, 1)                          // core 7569
u  = (w + 1) / 2                             // 0..1
u  = tension(u, pow)                         // pow#7606/7610
u  = (Q > 0) ? Q*round(u/Q) : u              // Quantize#7451 (round assumed)
u  = clip(u*Scale + off, 0, 1)               // X#7453, +#7455, core 7570
s  = OnePoleLP(u, fc = 1000/Tms Hz)          // Log -> HP/LP 1-Pole#7459 (audio-rate module). T = 0.0001 ms: no smoothing
g  = (G == 1) ? s * (gate_v > 0) : s         // X#7502 / switch G#7506
A_v = g * Damp_v                             // X#12770
F2  = F_v                                    // unused
tension(x, p): k = 0.999*p;  a = 2k/(1-k);  return x*(1+a) / (1 + a*|x|)
               // p=0: identity. p>0: log-like (fast rise). p<0: exp-like (stays low, then jumps up near x=1).
```
Snapshot pow (BE 7609, real = -1+2·norm): snapshot 0 = -0.874, snapshot 8 = 0, snapshot 9 = -0.134, snapshot 16 = -0.984.
Most BE snapshots are strongly exp-like: narrow, sparse peaks of band energy.
1-pole LP: `y += (1 - exp(-2π fc / SR)) * (x - y)`. T = 94.5 ms → 10.6 Hz, 31.5 ms → 31.7 Hz.

### 3.6 Midi-gated G
`gate_v` is the Instrument#3 voice gate of band v (Gate module: the note-on velocity, 0 on note-off).
How notes reach band voices depends on the ensemble's voice allocation (UNCERTAIN). All snapshots use G = 2 (ungated).

### 3.7 vib #7593 (LFO Q #7873)
`vib_v = VibA * SlowRandom_v(F = Vibf, A = 1)`. This is a Reaktor Slow Random: random values at control rate
through a 1-pole LP at Vibf Hz, so its absolute amplitude is small and ill-defined (UNCERTAIN).
Port: `LFNoise2.kr(Vibf) * VibA`.
The ' LFO'#7594 and its sync Merge are dead: their output is unconnected. vib is added to F in Hz, so it is a
random **rate** wobble per band. VibA is non-zero only in BE snapshots 4 (5.29) and 9 (3.91).

### 3.8 Snapshot-0 (Init) values, BE / LFO Q
BE:
- Len .307 (len 6), Q 0, Scale 1, off 0, BPM* 1.06, Div 1, Smooth 94.5 ms, Frn 0, mode 4 (monobrownian).
- skew 0, round 1, pwm 0, VibA 0, pow -0.874.
- Damp: pow -0.874, button 1, switch 2 (off).
- Gated: switch 2 (bypass).

LFO Q: as BE except Len .213 (len 4), BPM* 1, Div 3.01, pow +0.087, Damp pow -0.874 / button 0 / switch 2, pwm 0.123, Vibf .281.

Rates per snapshot: F = BPM/60 · BPM* / Div (e.g. at 120 BPM: snapshot 0 BE 2.12 Hz; snapshot 5 BE 0.126 Hz).

### 3.9 Damp #12776 (LFO Q #12818), described as "Still not working properly"
```
BE:     d_in = idxPitch_v (Pitch Index 'idx', ~0..1)       // In#12857
LFO Q:  d_in = 1 - (v-1)/nv                                 // linear band position, 1 at band 1
        (the BE macro computes this same expression but leaves it unconnected)
if d_in > 0: dp = tension(d_in, DampPow)                    // Separator Thld 0: values <= 0 are dropped, previous value held
Damp_v = (DampSwitch == 2) ? 1 : (DampButton ? dp : 1 - dp) // Relay#12809: Ctl>0 -> pow, else 1-pow
```
Static per band (it changes only when idx, nv or the faders change).

## 4. LFO Q #7649 vs LFO BAND ENERGY: differences only
Structure is identical. Checked: every Primary sub-macro netlist diffs clean once ids and labels are
normalized (the LFO Q faders are just unlabeled). Every Core cell pair (7460/7740, 7518/7798, 7526/7806,
7527/7807, 7560/7840, 7569/7849, 7570/7850) gives identical `--net` output on every structure path
(hashes differ only by layout). The only structural difference:
- No `idx` input, and Damp uses `1 - (v-1)/nv` instead of the Pitch Index idx (§3.9).

Wiring difference: output 'LFO' (Gated.OutG) goes to **Q#2119.Res input** and the Band Q display.
G and F2 are unused here too.

Control ids by role are in the §3.1 table. Notable snapshot values:
- Scale #7734 = 0 in snapshots 4-6, 9-12 and 18, so the LFO Q output is 0 there (no Q modulation).
- Mode #7851: snapshot 0 = 4, snapshot 1 = 3, snapshots 2-7 and 9-12 = 2 (random), snapshot 8 = 1, snapshots 13-18 = 4.
- G #7786 = 2 everywhere. Gated #7921 = 2 (bypass) everywhere.

## 5. Gated #7629 (LFO Q #7909): LFO-threshold envelope, NOT a MIDI envelope
LFO Q ids: Thr 7914, A 7917, D 7918, S 7919, R 7920, switch 7921.
```
trig_v = (x_v > Thr)                       // Fader#7634 0..1, 0.07 in all snapshots; A/E -> Step Filter: edges only
env_v  = ADSR(gate = trig_v (peak 1), A=#7637, D=#7638, S=#7639, R=#7640)
         times: t_ms = 10^(x/20)  (0=1 ms, 20=10 ms, 40=100 ms, 60=1 s, 80=10 s); S linear 0..1
         Snapshot values: A 0 (1 ms), D 52 (398 ms), S 0, R 26 (20 ms); snapshot 8: D 31 (35 ms), R 0
out_v  = (Switch#7641 == 1) ? env_v : x_v
G out  = trig_v (unused)
```
BE uses ADSR mode (switch = 1) in snapshots 8 (mello), 15 (arp) and 18 (beauty): each time a band's LFO value
rises above 0.07 the band "plucks" with a 1 ms / 398 ms decay. Otherwise the LFO value passes straight through.
ADSR segment shapes are Reaktor's ADSR (assume linear attack, exponential decay/release: UNCERTAIN).

## 6. How the outputs are used (outside the macros)

**Band Energy #2342 and Band Q #3284** are display only. XY 'P / A': X1 = band pitch P, X2 = P + 0.4,
Y2 = LFO value. Their outputs are not wired, so they have no effect on the sound.

**Amplitude.** BP Index Behavior Out 'LFO' → Filters#19 In#2355 → X#181 (L) and X#8031 (R):
`band_out_v = BP4_v(in) * GainCorr_v * GainCorr.Scale_v * LFO_BE_v`, a linear gain in 0..1. Then:
- ×Fader#8058 (1..16),
- the panner,
- the voice combiner.

**Resonance.** Q #2119 (input = LFO Q out):
```
R_dB  = clip(LFOQ_v * LFOQamt + Reso, 0, 100)   // LFO Q#15609 0..100 (100 everywhere); Reso#15607 0..100; core 15611 clip 0..100
R_s   = EventSmoother(R_dB)                      // time ~100 ms UNCERTAIN
ResL  = R_s                                       // Out port 1 -> Filters 'ResL' -> Gain Correction (dB)
Res   = 1 - sqrt(1 / 10^(R_s/20)) + plus          // = 1 - 10^(-R_s/40) + "+!"
                                                  //   0 dB -> 0, 40 dB -> 0.9, 100 dB -> 0.99684
Res   -> Filters 'Res' -> Multi/LP 4-Pole#23 / #8030 Res (both channels) and Gain Correction
```
"+!" #12817 is 0..0.002 (real = 0.002·norm; snapshots 1-2 give 0.002). Res can exceed the module's typical
0.98 range. Reaktor clamps it internally (UNCERTAIN), so clamp to ≈0.999 in a port.

## 7. Rates and what to compute where

| part | changes | rate |
|---|---|---|
| idx_v (Sym/N/Button/Rndm/iQ), Frn r_v, Damp_v, mute mask v ≤ nv, len·idx/nv offset | on parameter change (and the Rndm/Frn redraw) | static per band: **C++ arrays** |
| F_v base = tempo·BPM*/Div | tempo / fader change (Div smoothed ~100 ms) | slow control |
| ramp/phase p_v | continuous (F up to ≈ 32 Hz + vib) | control (Reaktor ~400 Hz events) |
| waveform, tension, Q, Scale/off/clip | per control tick | control |
| random draws | at p = 0 and p = 0.5 (random), p = 0 (brownian) | per-cycle events |
| 1-pole LP (fc = 1000/T) | continuous | audio in Reaktor; kr is enough |
| resync: transport start, Frn→0, note-on (gate sync), Line, G gating | discrete events | per event |
| Gated ADSR | LFO crossing 0.07 | per band event + envelope |
| Q path: dB clip → smoother → 1-10^(-R/40) | per band | control |

Recommendation:
- Run the time-varying LFO per band in SC at kr: Phasor plus per-band phase offset array, the deterministic
  shape maths, TRand/Latch at the phase edges, a Hermite random table for brownian/monobrownian, OnePole.
- Compute the static per-band arrays (idx offsets, r_v, Damp_v, mask) in C++ and send them on change.
- A C++-only version needs ≥ 100-200 Hz array updates for fast rates (F up to ≈ 32 Hz plus square/random
  edges when Smooth ≈ 0). That is heavy for 2 × 120 bands over OSC.
- value(v,t) has no closed form when Frn ≠ 0 or VibA ≠ 0: each band then has its own phase accumulator.
  Otherwise `p_v = frac(ph+ + F·(t - t_sync) + len·idx_v/nv)`.
