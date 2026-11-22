# spec_bank — AnimaMundi filter-bank audio path + Body exciter + Post Filter

Sources: net2.py on 3, 19, 120, 128, 2093, 2119, 2696, 2736, 15262..15592; core_view on cells
12720/12722/2737; raw .ens bytes (instrument headers, Send/Receive labels, smoother times, saved
panel values at offset+0x112). Conventions: P = MIDI pitch (69 = 440 Hz),
`hz(P) = 440*2^((P-69)/12)`, `dbamp(x) = 10^(x/20)` (Reaktor Expon.(A)), SR = sample rate.
Saturator / ADSR / Expon formulas cross-checked against the addon's verified Primary code
(`ugens/OceanodeSteamPipe/PrimaryCompat.h`, `reaktorSat` in `ugens/OceanodeTwanger`).

## 0. Ensemble routing (top level)

```
AudioIn#5743 (L) -> Body#2696 in1 ;  AudioIn#8047 (R) -> Body#2696 in2
Snapshot#12554 out2 "Recl" -> Body#2696 in0 "scale"
Body out L/R -> Soul#3 in L/R -> delay2#12635 -> Metaverb#12218 -> Out#419/#420
```
Soul#3 ports: in `L` (In#5), `R` (In#8033), `snap` (In#12553: wired to nothing inside -> unused).

## 1. Instrument#3 "Soul" (the filter bank)

**Polyphony: 120 voices, fixed** (instrument header field = 0x78; Body/Metaverb/delay2 = 1).
One voice = one band. All 120 voices always run (Reaktor processes every voice).
**Bands fader 5909 [0..120] is NOT the voice count; it is a gain gate.** It goes to `Send#5908`
(label `vcs`); every Receive with link id 9 (int32 at record+0x112) gets it: 5916 (Pitch Modifiers
`Iteration.N` = how many voices get the per-voice detune), 7131/7645 (LFO BAND ENERGY), 7671/7925
(LFO Q), 12466/12467 (Pitch Index), 12777/12819 (LFO `Damp`: `1-(V-1)/vcs` across band index).
(The labels `nvoices`/`vcs` differ but the link id is the same 9.)
Gate, in LFO BAND ENERGY#6808: `energy_v = LFO.A_v * (V_v <= Bands ? 1 : 0)` (Voice Info V vs
Receive#7645, Compare `<=`, X#7648) → Gated#7629 (optional ADSR) → output `LFO` → Filters port 3.
So **bands v > Bands are silent** (energy 0); the other 120-Bands voices still run (CPU).
Bands per snapshot 0..18: 35 35 45 45 119 120 120 99 24 64 98 98 47 19 29 32 120 20 16 (saved 120).
The ens comment warns it can freeze the computer (event loop size in Iteration / LFO).

Inside Soul (mono -> poly: the mono input L/R is broadcast unchanged to every voice):
```
wetL = PostFilter.L( FiltersOutL ) ; wetR = PostFilter.R( FiltersOutR )
outL = Scanner5738(pos=F5739; in0 = dryL(In#5), in1 = wetL)
outR = Scanner5740(pos=F5739; in0 = dryR(In#8033), in1 = wetR)
```
| id | control | range | saved | snapshots | role |
|---|---|---|---|---|---|
| 5739 | (unnamed) dry/wet | 0..1 (step 1e-5) | **1.0** | 1 in all | Pos of both Selector/Scanners: 0 = dry input, 1 = filter bank. Fine-resolution 0..1 fader ⇒ Scanner mode (crossfade) — UNCERTAIN: Linear `(1-p)·dry + p·wet` (best guess) vs Sine. At the saved 1.0 both modes = 100 % wet. |

## 2. Filters#19 inputs (macro port order = In child order; verified on 120 and 15410)

| port | In id | source (BP Index Behavior#2093) | carries | rate |
|---|---|---|---|---|
| 0 (audio L) | 20 | Soul In#5 (Body out L) | exciter L | audio, mono broadcast |
| 1 `P` | 2353 | Out#2348 ← Pitch Modifiers#5460.P | band pitch, semitones (MIDI), per voice | event |
| 2 `W` | 2354 | **unconnected** (= 0) | Gain Correction Width | — |
| 3 `LFO` | 2355 | Out#2350 ← LFO BAND ENERGY#6808.LFO | band energy = LFO amplitude × (V ≤ Bands), via Gated#7629 (linear factor, nominal 0..1 — range per LFO spec) | audio, per voice |
| 4 `Res` | 2356 | Out#2351 ← Q#2119.Res | filter Res = `1 - 10^(-ResL/40) + fine` (fine = `+!` fader 12817: range not decodable, shows [0..0]; snapshot values 0, 1, 1, 0.00787 — if literally 1, Res≈2 and gc becomes a boost: UNCERTAIN, Q spec should settle it; likely a tiny value e.g. n/127) | event |
| 5 `ResL` | 2357 | Out#2352 ← Q#2119.ResL | resonance in dB = `Reso(0..100) + LFOQ_amt·LFO_Q` → CoreCell#15611 → Event Smoother 100 ms (inferred) | event |
| 6 (audio R) | 8032 | Soul In#8033 (Body out R) | exciter R | audio |

(Q#2119: `ResL` = smoothed dB value; `Res = 1 + fine - sqrt(1/dbamp(ResL))`. CoreCell#15611 not
decoded here — Q spec.) Band Energy#2342 and Band Q#3284 are XY displays only (no audio effect).

## 3. Per-band DSP (Filters#19), voice v = 1..120

```
P   = pitch(v)                      // from 2353
fL  = BP4( inL, P + d, Res )        // Multi/LP 4-Pole#23, output BP4;   d = Fader 8049
fR  = BP4( inR, P - d, Res )        // Multi/LP 4-Pole#8030, output BP4
G   = gc(P, Res) * Scale(ResL) * E(v) * K        // X#181 / X#8031 (4-input multipliers), K = Fader 8058
sL  = G * fL ;  sR = G * fR
(L_v, R_v) from Distributor#12718( sL, posL(v) )  +  Distributor#12719( sR, posR(v) )
busL = Σ_v [out0 of 12718 + out0 of 12719]   // +#12731 -> Audio Voice Combiner#118 (sum of voices)
busR = Σ_v [out1 of 12718 + out1 of 12719]   // +#12732 -> Audio Voice Combiner#3301
FiltersOutL = Sat(busL) (Saturator#230) ; FiltersOutR = Sat(busR) (Saturator#3302)
```

### 3a. Multi/LP 4-Pole (Primary, kind 307) — BP4 output
Inputs P (semitones, cutoff/centre = hz(P)), Res (0 = none, 1 = self-oscillation, typ. 0..0.98),
In. BP4 = 4-pole band-pass, 12 dB/oct each side. Implementation already in the addon:
`ReaktorMulti4` (ugens/OceanodeAnalog, type 9: `4·(y1 - 2·y2 + y3)` of a ZDF ladder,
loop gain k = 4·Res) — reconstructed, not bit-exact. Res can exceed 0.98 here (Reso 100 dB →
0.9968, plus `+!` fine) — clamp Res ≤ ~0.995 for stability.

### 3b. Gain Correction#120 (ports P, Res, ResL, Width)
```
gc(P,Res)   = dbamp( -73 * (1 - Res) * hz(P) / SR )                 // Out#121, event rate
                (Invert unipol#128 = 1 - x)
s3(x)       = CtrlShaper3BP: (0→0.40, 0.25→0.57, 0.50→0.71, 0.75→0.71, 1→0.71), piecewise linear
              (x = 0.01*ResL; above 0.5 flat 0.71, so extrapolation beyond 1 is also 0.71)
w(Width,ResL) = Shaper1BP#153( Width * Shaper1BP#160(0.5*(0.01*ResL)^2) )
              #160: (0→0.5/127=0.003937, 0.5→0.1, 1→0) ; #153: (0→0.1585, 0.5→1, 1→1)
              Width is unconnected = 0  ⇒  w ≡ 0.1585  (= -16.0 dB)
Scale(ResL) = AudioSmoother_2.5ms(inferred)( dbamp( -ResL * s3(0.01*ResL) ) * 0.1585 )   // Out#143, audio
```
Note gc uses the band centre P (not P±d). gc ≈ 1 near Res → 1; at Res = 0, 10 kHz, 44.1 k: -16.5 dB.

### 3c. Other per-band constants
| id | control | range | saved | role |
|---|---|---|---|---|
| 8049 | (unnamed) stereo detune | 0..1 st (step .02) | **0.1** | L filter at P+d, R filter at P-d (semitones). Snaps 0..18: .1 .1 .1 .1 .1 .1 .26 .1 0 0 0 0 .1 0 0 0 .08 .08 .14 |
| 8058 | (unnamed) output gain | 1..16 linear | **16** | ×K on every band before the pan. Snaps 0..18: 16 16 16 16 2.54 16 8.2 16 16 8.09 8.09 14.2 14.2 14.2 8.09 8.09 14.2 14.2 13.4 |
| 12726 | (unnamed) stereo spread | 0..1 | **1** (all snapshots 1) | pan randomisation width w, below |

E(v) = Filters input 3 (LFO band energy), multiplied in at audio rate.

### 3d. Stereo placement (Fader 12726, Order#12727, Voice Info#12721, Core 12720/12722, Distributors)
```
U(seed, k)  : Core "Random [0..1]" = LCG, int32 state:
              on Seed event: state = seed ; on each C event: state = state*1103515245 + 12345 (wrap 2^32)
              output = (state & 0x7FFFFFFF) * 4.65661288e-10      // [0,1)
seedL(v) = v            (Voice Info V, 1..120)
seedR(v) = v + Vcs = v + 120
On every event of fader 12726 (value w): Order.1 clocks both cells (C) -> new U; Order.2 then:
posL(v) = w * U_L(v)            // X#12725 -> Distributor#12718.Pos
posR(v) = 1 - w * U_R(v)        // X#12728, 1 - x  -> Distributor#12719.Pos
Distributor (2 outputs, Pos 0..1):  out0 (→ L bus) = a0(pos)*in,  out1 (→ R bus) = a1(pos)*in
```
- w = 0: L filter → hard left, R filter → hard right (plain stereo). w = 1: each band's L-filter
  and R-filter signals land at independent pseudo-random pan positions, both uniform on [0,1)
  (no left/right bias); intermediate w confines the L-filter to [0,w] and the R-filter to [1-w,1].
- First step after seeding (init): `U(s) = ((1103515245*s + 12345) mod 2^31) / 2^31`
  → U_L(1..8) = .514 .028 .542 .056 .569 .083 .597 .111 ; U_R(1..8) = .178 .692 .205 .719 .233 .747 .261 .775.
  Every later event of fader 12726 (incl. each snapshot recall, which re-sends it) advances the
  LCG one step → pans re-randomise. Reaktor's exact pans therefore depend on event history; a port
  should use `U(v)` (one step from seed) or any per-band deterministic random. UNCERTAIN: whether the
  seed event precedes the first clock at init (assumed yes).
- **Pan law UNCERTAIN**: Distributors must be in a panning curve (in "None" mode both filters would
  go to out0 = left), but Linear vs Sine is not decodable (curve property not found in the record;
  flags word 0x6 = same as the obviously-scanning selectors in vibrato/deterministic). Best guess
  **Linear: a0 = 1-pos, a1 = pos**. Sine alternative: a0 = cos(pos·π/2), a1 = sin(pos·π/2).
  The two differ by ~3 dB at centre pan (sine louder) — tune by ear against the original.

### 3e. Saturator (Primary kind 360) ×2, after the voice sum
`sat(x) = z*(1 - |z|/8), z = clip(x, -4, 4)` → unity small-signal gain, ±2 hard ceiling at |x| ≥ 4
(matches catalog "±2 output for inputs beyond ±4"; same formula as the addon's verified `reaktorSat`).

## 4. Whole-bank equation

```
E_v      = LFOenergy_v · [v ≤ Bands]   (K = F8058, d = F8049; saved 16 and 0.1)
P_v      = pitch of band v (BP Index Behavior)           ResL = Q dB,  Res = 1 + fine - 10^(-ResL/40)
g_v      = K · E_v · dbamp(-73·(1-Res)·hz(P_v)/SR) · 0.1585 · dbamp(-ResL·s3(ResL/100))   [Scale part smoothed 2.5 ms]
yL_v     = g_v · BP4(xL, P_v + d, Res)     yR_v = g_v · BP4(xR, P_v - d, Res)
busL     = Σ_{v=1..120} [ a0(posL_v)·yL_v + a0(posR_v)·yR_v ]
busR     = Σ_{v=1..120} [ a1(posL_v)·yL_v + a1(posR_v)·yR_v ]
posL_v   = w·U(v),  posR_v = 1 - w·U(v+120),  w = F12726 (1.0)
bankL/R  = sat(busL/R)
outL/R   = mix5739( dry = xL/xR , wet = PostFilter(bankL/R) )         (5739 = 1 → wet only)
```
xL/xR = Body outputs (with saved switches = the live audio inputs).
(With Linear law and w=0 this reduces to out_L = Σ g_v·BP4(xL, P_v+d), out_R = Σ g_v·BP4(xR, P_v-d).)

## 5. Instrument#2696 "Body" (exciter)

**1 voice (mono)** (instrument header voice field = 1; UNCERTAIN only in that the header layout
is inferred by comparison with Soul = 120). Audio Voice Combiners are therefore pass-through.
Inputs: 0 `scale` (event) → Scales#2736.scale; 1 → In#5742 (live L); 2 → In#8043 (live R).

```
Pn   = NotePitch + ScaleFit(...) + root(=0, In#2745 unconnected)    // Scales#2736
P    = Pn + 12*Oct(F2715) + F3130
PL   = P + F8045 ;  PR = P - F8045                // stereo detune in semitones
env  = ADSR(Gate, A=F2708, D=F2709, S=F2710, R=F2711)   peak = gate value (velocity 0..1)
oscL = Switch2702[ noise(A=env), saw(PL,env), pulse(PL,env,W=0), impulse(PL,env), geigerClk(PL,Rnd=F8053)*env ]
oscR = Switch8037[ same with PR, Geiger#8052 ]
outL = Switch5741[ 1: oscL (synth) , 2: liveL ]   outR = Switch8042[ 1: oscR , 2: liveR ]
```
| id | control | range | saved | meaning |
|---|---|---|---|---|
| 2702 | L osc select | 1 noise, 2 saw, 3 sq, 4 imp, 5 geiger | **2 saw** | |
| 8037 | R osc select | same | **2 saw** | |
| 5741 | L source | 1 synth, 2 IN | **2 IN** | ⇒ by default Body is bypassed: live input goes straight to Soul (as in the original Anima) |
| 8042 | R source | 1 synth, 2 IN | **2 IN** | |
| 2708/2709/2711 | A / D / R | 0..80 log time | **25 / 40 / 15** | T = 10^(x/20) ms: 0=1 ms, 20=10 ms, 40=100 ms, 60=1 s, 80=10 s → A 17.8 ms, D 100 ms, R 5.6 ms |
| 2710 | S | 0..1 | **0.5** | sustain = S × peak |
| 2715 | Octave | -4..4 | **-3** | ×12 semitones (snapshot column shows -4 but those Body values are junk; saved = -3) |
| 3130 | transpose | -48..48 st | **0** | |
| 8045 | stereo detune | 0..1 st (step .02) | **0.24** | L +0.24, R -0.24 semitones |
| 8053 | Geiger Rnd | 0..1 | **0** | 0 = completely random timing, 1 = regular |
(The Body faders are not stored in snapshots: the controls.txt columns for 2708..8053 are raw-int
garbage; use the saved values.)

Primary oscillator maths (catalog + addon-verified Primary code):
- Noise: white, range ±0.5·A (`WhiteNoise.ar * 0.5 * env`).
- Sawtooth: ±A at hz(P). Pulse: ±A, W input unconnected = 0 ⇒ 50 % duty (W: -1=0 %, 0=50 %, 1=100 %).
- Impulse: positive ~2-sample impulses, DC-removed, energy-normalised: peak ≈ 8·A at 44.1 kHz
  (area ≈ 8/44100 s per impulse; scales with SR). Addon reconstruction in OceanodeTwanger.
- Geiger: P = log density (same pitch PL/PR, so density follows the note; UNCERTAIN mapping — assume
  mean rate = hz(P) Hz, e.g. note 60-36 = 24 → ~32.7 clicks/s), Rnd 0 = Poisson. `Clk` = audio
  1-sample click (amplitude 1 assumed) × env.
- ADSR (Primary): linear attack 0→peak in T_A (per-sample increment `1000·10^(-A/20)/SR`), decay
  one-pole toward S·peak with time constant T_D (multiplier `1 - 1000·10^(-D/20)/SR` per sample),
  release one-pole to 0 with T_R. Gate>0 starts/retriggers (peak = gate), gate=0 → release.
  Same as `niTimeRate` in PrimaryCompat.h.
- Note Pitch / Gate: mono, last note; Note Pitch offset property 0.

Scales#2736 = Core "Scale Fitter" (ports P, Base, Sel; out P):
`out = P + table[ ((P - Base) imod 12) + 12·Sel ]`, Base unconnected = 0. Its Init Array is
**empty** (5 zero bytes, size 0) ⇒ the table read returns 0 ⇒ **pass-through** (UNCERTAIN but
likely; there's no scale data in the cell). The Snapshot module output 2 (`Recl`, value 1.0 on
every snapshot recall) drives `scale`: Order → (1) Sel = 1, (2) re-send the held pitch. Net effect:
on each snapshot recall the Body pitch is re-evaluated with scale #1 = no change. Ignore in a port.

## 6. Post Filter#15262 (Reaktor library "Multimode Filter", stereo)

Confirmed: standard Reaktor 5 library multimode (1/2/4-pole Primary filters + Gain Corr macros).
```
P    = EventSmoother_30ms(Cutoff#15585)          // smoother times inferred from record float +0x48          // -52..127 semitones
ResL = max(0, EventSmoother_30ms(Reson#15587))   // 0..48 dB  (E clipper Min, Min = 0)
GC   = EventSmoother_30ms(GainC#15267)           // 0..1, saved/all snaps 0.504
f    = hz(P)
```
Type switches 15500 (L) / 15535 (R), 1-based: 1 LP1, 2 LP2, 3 LP4, 4 BP2, 5 BP4, 6 HP1, 7 HP2,
8 HP4, 9 Off(=input). Saved panel 2 (LP2); snapshots: 1 (LP1) in most, 6 (HP1) in 5,6,7,14,15,18,
3 (LP4) in 8. Cutoff saved 77.7, snapshots mostly 127; Reson 0 except 12.5 dB in snaps 14,15,18.

Resonance dB → Res:
- 2-pole (Multi 2-Pole): `Res2 = 1 - 10^(-ResL/20)`
- 4-pole HP/BP (Multi/HP 4-Pole; BP4 and HP4 both taken from it): `Res4 = 1 - 10^(-ResL/40)`
- 4-pole LP (Multi/LP 4-Pole): `ResLP4 = 1 - 10^(-(ResL - 12·Res4·f/SR)/40)`
- 1-pole: no resonance.

Gain correction (multiplies the filter output; Invert unipol = 1-x; shp1(x; a,b,c) = Shaper 1BP
with levels at 0, 0.5, 1, piecewise linear):
- BP2: `dbamp(6.0206 - GC·ResL) · dbamp(-36·(1-Res2)·f/SR)`
- LP2: `dbamp(-GC·c2)`, `c2 = max(0, ResL + shp1(0.5·Res2; -3, -6, 0))`
- HP2: `dbamp(-GC·c2 - 51.75·(1-Res2)·f/SR)`
- BP4: `dbamp(12.0412 - GC·ResL - 73·(1-Res4)·f/SR)`
- LP4: `dbamp(-GC·c4)`, `c4 = max(0, ResL + shp1(0.5·Res4; -6, -12, 0))`
- HP4: Gain Corr HP#15432 (not decoded, same family; UNCERTAIN)
- HP1: `1 - π·min(f, SR/(2π))/SR`;  LP1: none.

## UNCERTAIN summary
1. Distributor pan law (Linear assumed) and Scanner 5738/5740 curve (irrelevant at saved 1.0).
2. Exact pan values (depend on LCG step count since load); use U(v) one step from seed.
3. Body voice count = 1 (header comparison).
4. Geiger density mapping; Impulse exact shape; Reaktor 4-pole BP4 exact topology.
5. Scale Fitter table empty ⇒ pass-through.
6. `+!` fine-resonance fader 12817 range/scale (feeds Res).
7. Smoother times (2.5 / 30 / 100 ms) read from an inferred record field.
