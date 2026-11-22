# AnimaMundi: band pitch spec (BP Index Behavior#2093 pitch path, Body Scales#2736)

Signal path per band (= Reaktor voice `V = 1..B`, `B = Bands#5909`, all event rate):

```
V --Pitch Index#12294--> idx (integer "degree index") --Harmony#10661--> P_h (MIDI note)
  --Pitch Modifiers#5460--> P_band (MIDI note, 69 = 440 Hz) -> Filters / Band Energy / Band Q
Pitch Index also outputs idxNorm = idx / (B*Step) -> LFO BAND ENERGY#6808.idx
```

A reference model of the deterministic part is in `SCRATCH/anima/pitch_ref.py`, which defines functions only.
`python3 -I SCRATCH/anima/pitch/print_ref.py` prints the per-snapshot values given in Section 2.
The helper scripts and extracted tables are in `SCRATCH/anima/pitch/`. `net3.py` is net2 with In/Out port labels added.

## Conventions and decoding notes

These conventions hold everywhere in this spec.

- `rnd(x)` is Reaktor Quantize with step 1. I took it as round half up, `floor(x+0.5)`. INFERRED: the rounding mode is not documented. It only matters for `V/rept` when `rept` is greater than 1, and every snapshot has `rept = 1`.
- `x % y` is the Reaktor Modulo module, which is floor based: `x - y*floor(x/y)`, so the result is in `[0, y)` even when x is negative. `Div` is `floor(x/y)`.
- **Macro port mapping.** The In and Out modules inside a macro carry their port label in the module record, so that is how I matched them to ports. Module order does not give the port order.
- **Send/Receive ids, INFERRED from usage.** Receive selector 9 is `Bands` (Send#5908). Receive selector 15 is the Harmony clock (Send#10702). This applies to Receive 12466/12467/5916 (selector 9) and 5512/10890/12104 (selector 15).
- **Snapshot values.** List and Switch values in the snapshots are entry *ids*, not positions. `controls.txt` lacks the Lists, because the parser's `effective_controls` dropped them, but they are in `snapshots[i].parameters`. I re-extracted every value with `SCRATCH/anima/pitch/snapvals.py`, which writes `SCRATCH/anima/pitch/snapvals.json`.
- **Switches.** Snapshot value p (1-based) selects input p-1. A value of -1 means Off (no input). For the layer category switch the positions are 1=Micro, 2=Overtones, 3=User, 4=Scales, 5=Chords. This is confirmed by the parallel List#10662, whose ids are 1=Chords, 2=Scales, 5=User, 6=Overtone, 7=Microtone, and which agrees with the switch in all 19 snapshots. That a -1 switch is Off is INFERRED.
- **Buttons, INFERRED.** In the snapshot parameters a button stores normalized 0.5 for On; `controls.txt` shows 1. Transpose `step` 10840 is On in every snapshot.
- **Randomizer, INFERRED additive.** `Out = In + U(-Rng, +Rng)`, uniform. It must be additive because Transpose and Inversions feed it In = 0. This sets the #6 index range, the Transpose ranges in #3/#7/#18 and the vibrato rate spread.
- **Unreliable values in `controls.txt`.** Controls that are not stored in snapshots show garbage there (Body faders 2715/3130 read -4/-48). Read the saved panel state instead.

---

## 1. Pitch Index#12294 (per voice)

Controls (snapshot values in all 19 presets unless listed):

| id | label | range | role | snapshots |
|---|---|---|---|---|
| 12297 | Step | 0..12 /0.25 | index step (scale degrees per band) | 1; #6 = 0; #16 = 1.75 |
| 12301 | (cycles) | 0..16 /0.1 | phase multiplier F1, also divides Step | 1 |
| 12307 | (offset) | 0..1 | phase offset F2 | 0 |
| 12311 | rept | 1..6 | adjacent voices sharing one index | 1 |
| 12328 | comb | 0..1 | odd voices mirrored | 0 |
| 12342 | invert (Button) | 0/1 | reverse order | 0 |
| 12351 | Symm | 1..16 | triangle fold count | 1 |
| 12365 | rnd dev | 0..127 | random deviation range (in index units) | 0; #6 = 127 |
| 12376 | rnd dev Switch | Off/On | redraw the random value on each gate | -1 = Off in all |
| 12393 | pow shape | -1..1 (range from raw record) | index curve | 0 (normalized 0.5) |
| 12381 | Q | 0..127 | output quantize step (0 = pass through) | 0 |
| 12414 | skew | ±0.999999 | deterministic curve skew | 1 (=0.999999); #8 mello & #15 arp = 0; #18 beauty = -1 |
| 12419 | round | 0..2 | tri → sine → square | 0 |
| 12454 | (edge) | -0.49..0.49 | clip/expand of the phase | 0 |

The fader range of 12393 is missing from the parsed `control_properties`. The raw record holds -1..1 (consistent with its pow port), and the 0.5 snapshot value is the normalized one, so the shape is 0 (identity).

```
B   = Bands (Receive 12466/12467);  vcs = B + 1
k   = rnd(V / rept)                         // 'if'
ph  = (k/(B+1) * F1 + F2) % 1
d   = detCurve(ph)                          // macro 'deterministic', below
v   = rnd((B+1) * d)
v   = invert ? (1 + (B+1) - v) : v          // macro invert (Relay: button on -> 1+vcs-v)
c   = rnd(| (B+1)*(k % 2)*comb - v |)       // macro Comb: comb=1 mirrors odd voices
st  = Step / F1                             // Divide 12465
x   = c * st
// macro symm (triangle fold, Symm segments over the whole range)
Pd  = st*(B+1)/Symm;  m = x % Pd;  n = floor(x/Pd);  x = |(n%2)*Pd - m| * Symm
// macro rnd dev (Randomizer is additive: Out = In + U(-R,+R), R = fader 12365)
x   = rnd(| x + U(-R,R) |)                  // a new draw for every incoming value, on R change, and
                                            //   on note gate if Switch 12376 is on (it is Off)
// macro pow (shape s = fader 12393)
a = 0.999*s; kk = 2a/(1-a); y = x/(B+1); x = (B+1) * y*(kk+1)/(kk*|y|+1)   // s=0: identity
// macro Q
x   = (Q>0) ? Q*rnd(x/Q) : x                // INFERRED: Quantize with St=0 passes the value through
idx = StepFilter(x, tol 0)                  // emits only on change
idxNorm = idx / (B * Step)                  // Out 15258 -> LFO BAND ENERGY.idx
```

`detCurve(ph)` uses Core cells 12453, 12415 and 12416 with skew `sk` (clipped to ±0.999999), round `r` and edge `a`:

```
x  = (clip(ph, a, 1-a) - a) / (1 - 2a)                    // cell 12453
c  = (sk+1)/2
w  = x<=c ? 0.5*x/c : 0.5 + 0.5*(x-c)/(1-c)               // cell 12415 (skew warp)
g  = 2 - |sk|
s0 = tri(w)/g,  tri(w) = w<0.5 ? 2w : 2-2w                // cell 12416
s1 = (1 - cos(2πw))/2 / g                                 // Sine/Cosine module (input in cycles)
y  = scan(r; s0, s1)                                      // Selector 12418, Pos = round (0..1)
Yexp = clamp(2 - r, 0, 1)                                 // x^y macro: Selector(0,1) with Pos 2-r
out = (sign(2y-1)*|2y-1|^Yexp + 1)/2                      // r<=1: identity; r=2: square
```

Results for the snapshots, with every other control at default:

- `sk = 1`: `d ≈ ph`, so **`idx = V*Step`**. This is the default, and gives voice 1 index 1, voice B index B.
- `sk = -1` (beauty): `d ≈ 1 - ph`, so `idx = (B+1-V)*Step`. The order is reversed.
- `sk = 0` (mello, arp): `d = ph` for `ph < 0.5`, else `1 - ph`. Indices rise, then fall back down: voices V and B+1-V get the same pitch, and the top index is about (B+1)/2.
- **Step = 0** (#6 microtonal landscapes 1): `st = 0`, so x = 0. The symm stage then computes x % 0 (Reaktor's result is UNCERTAIN; the model uses 0), and idxNorm divides by 0. The index is decided only by rnd dev 127: `idx = rnd(|U(-127,127)|)`, uniform over 0..127 per voice.
- **Step = 1.75** (#16 qbrus): `idx = rnd(1.75*V)`.

Scanner and Selector mode (crossfade or switch) is INFERRED for 12418 and 12433. It does not matter here, because `round = 0` in every snapshot.

---

## 2. Harmony#10661

### 2a. Index to MIDI pitch (exact)

Four Stacked Macro layers L = 1..4 are evaluated in parallel for every voice:

```
deg_L   = idx + mode_L + inv                   // +#11187 (mode = fader 11188 etc., 0..7)
inv     = Key + invRand + midiInv              // Inversions#10865 + MIDI IN.inv (Off)
f_L     = category function of deg_L           // below
P_L     = f_L + root_L + 12*oct_L + trans      // +#11194; root 0..12 (Multi Text shows note name), oct 0..8
trans   = transRand + TrSemi + TrFine + midiTrans
P_h     = Scan(layerPos; P_1..P_4) + poly8Oct   // Selector/Scanner 10830 + Poly8#12096
```

Category functions (n = scale size; integer `deg`; floor-mod handles negative degrees):

| category | f(deg) | size |
|---|---|---|
| Chords (Event Table 7×115, row = list value) | `T[row][deg % n] + 12*floor(deg/n)` | n by row: 1→2, 2..14→3, 15..41→4, 42..82→5, 83..108→6, 109..115→7 |
| Scales (Event Table 12×65, row = list value) | same | n by row: 1..18→5, 19..28→6, 29..60→7, 61..63→8, 64..65→12 |
| User (Event Table 12×1) | same, `n` = fader (11183 / 11467 / 11751 / 12035, 1..12) | n |
| Micro | `deg * 12 / N`, N = fader 11238 etc. (0..32): N-EDO with no wrap | N |
| Overtones | `h = {Even: 2deg+2, Odd: 2deg+1, All: deg+1}`, `f = 1 + 12*log2(h)`; Button 11226 on → `rnd(f)` | fader 11216 (display only) |

Notes on the category functions:

- **Overtones** is computed as Expon(P=1)·h → Log. That is `P(8.66 Hz × h)`, which is why the base sits one semitone above.
- **Overtones with h ≤ 0** happens with `Key = -3`: voices 1 and 2 get h = -2 and 0. Reaktor's Log of a value ≤ 0 is UNCERTAIN. Suggest muting those bands or clamping h to at least 1. It affects sweep, sweep2 and sweep3.
- **Table reads.** A row index above the last row (value 65 Chromatic, value 115 dim13) is clipped to the last row. Row 64 is chromatic. Columns beyond n are ignored.
- **Read triggers.** Each category macro samples the degree through Value / Event V.C. All, so the output updates on every index change. Its `G` input is unconnected.
- **Size output.** Event V.C. Max of the size goes to Harmony.size. That port and Harmony.root are **not connected** downstream; only `P` is used.

The external-scale replacement keeps the same formula. With a pitch-class vector `pcs[0..n-1]` relative to the root:

```
P = pcs[deg % n] + 12*floor(deg/n) + root + 12*oct + trans
```

With absolute MIDI notes from a ChordBox, `notes[0..n-1]` ascending:

```
P = notes[deg % n] + 12*floor(deg/n) + trans
```

`deg = idx + Key + mode` as above. With the defaults (Step 1, skew 1) and `Key = -3`, **voice 1 has deg = -2**. It therefore takes `pcs[n-2] - 12`, two degrees below the base octave, and band V has degree V-3.

### Transpose#10832

```
TrSemi = fader 10850 (-24..24 st) = 0;  TrFine = fader 10851 (-1..1 st) = 0
transRand = smooth( Q*rnd( (|x| ? |r| : r) / Q ) ),  r = U(-rndm, +rndm)
```

- `rndm` is fader 10833 (0..12), `Q` is fader 10849 (0..12), `|x|` is button 10855.
- `smooth` is the event smoother 10856 with T = fader 10860 ms, which is 0 in all snapshots.
- A new draw happens on every clock step if `step` (10840) is on. If `step` is off, it happens only when the clock returns to 0. `step` is on in every snapshot.
- The active cases:
  - #3 chinese porcelain: rndm 6, Q 3, |x| off, so values in {-6,-3,0,3,6} every 4 bars.
  - #7 syncrotron: rndm 12, Q 1, so -12..12 st every 1/32 note.
  - #18 beauty: rndm 3, Q 1, so -3..3 every 4 bars.
  - All other snapshots: 0.

### Inversions#10865

```
inv = StepFilter( Key + (|x| ? |q| : q) ),   q = Q*rnd(U(-rndm,+rndm)/Q)
```

- Key is fader 10883 (-24..24, in degrees, not semitones), rndm is 10866 (0..12), Q is 10882 (1..12), |x| is button 10887. The draw is triggered the same way as in Transpose, by button 10873.
- **Key = -3** in every snapshot except #7 syncrotron and #18 beauty, where it is 0. rndm is 0 everywhere, so `inv = Key`.

### MIDI IN#10663

- Note Pitch - 60 (MIDI 60 = C4 in Reaktor's naming, which calls it C3) goes to two switches:
  - `Inv` (10667) adds to the degree. It is Off (-1).
  - `trans` (10668) adds semitones to P after the event smoother 10675 (T = fader 10679 = 0 ms). It is **On (1) in all snapshots**.
- So a played MIDI note transposes the whole bank by (note - 60) semitones.
- Switch `G` (10673) is at 2, which selects Constant 1. MIDI gating is therefore off.
- Instrument#3 is polyphonic. Whether a note reaches every band, as unison would, depends on the instrument's voice-assignment properties, which were not decoded (UNCERTAIN). For AnimaBox, apply the transposition to all bands.
- Reaktor's Note Pitch value before the first note is UNCERTAIN. Treat no note as 0 transposition.

### Layer clock: Global Clock#10698 and the event smoother#12128

```
pos96 = host song position in 96th notes (24 ppq), only while transport runs
n     = floor(pos96 / (96 / beat))           // List 'beat' 10818
s     = n % Len                              // Len = fader 10701 (1..4)
clock = mode(List 10704): '>>' s | '<<' Len-1-s | '><' ping-pong | rndm (cell 10708)
        | Probability (Markov#10718) | manual (MIDI note%12 or OSC) | slide (fader 10829 0..3)
layerPos = eventSmoother(clock, T = smooth fader 12132 ms)
```

- `beat` entry ids: 6 = 1*1 (1 bar), 1 = 1*2 (2 bars), 15 = 1*4 (4 bars), 26 = 1/32.
- `layerPos` is applied as Pos to Selectors 10830, 10918 and 10958.
- **Smoother.** On each new value, the output glides from the previous target to the new one in T ms, updated at 60 Hz. The glide is shaped by a sigmoid: `u = 2r-1; s = (u - k u)/(k - 2k|u| + 1); out = (s+1)/2`, with `k = -0.6` and `r` ramping 0..1.
- **Selector 10830 mode.** It is INFERRED to be Scanner (linear crossfade) from the fader description, "glissando from the previous chord to the actual chord". During a glide each band's pitch is therefore `lerp(P_a, P_b)`.
- **Len = 1** uses layer 1 only. With the transport stopped, `s = 0`, which is also layer 1.
- **Probability mode** is used only in #2 whole_crystals (Len 2). The Markov transition tables live in snapshot compound states 616 and 618-620 and were not decoded. With 2 layers, it alternates randomly between L1 and L2.

### Poly8#12096 (random octave per band)

Three Core `Random [0..1]` LCGs per voice:

```
state = state*1103515245 + 12345 (int32 wrap);  out = (state & 0x7FFFFFFF) * 2^-31
```

They are seeded with V, V+66 and V+123. Each one steps on every poly8 fader event, and on every clock step if `ck` (12112) is on, which it is everywhere.

```
active = poly8 > r1 ;  oct = 12*rnd(rng*r2) ;  sign = 2*rnd(r3) - 1
poly8Oct = |x| ? active*oct : active*oct*sign
```

- poly8 is fader 12100 (0..1), rng is fader 12107 (0..4), |x| is button 12125 (0).
- It is active only in #7 syncrotron: poly8 = 1 and rng = 4, which gives every band a random shift of ±0..48 st, redrawn every 1/32-note step. In all other snapshots poly8 = 0.

### 2b. Layer scales per snapshot

Base = root + 12·oct, as a MIDI note. "unused" means layer index > Len.

| # | snapshot | Bands | Len / mode, step | **L1** | L2 | L3 | L4 |
|---|---|---|---|---|---|---|---|
| 0 | Init | 35 | 2 / >>, 4 bars | **Scales hirajoshi [0,2,3,7,8], root C, base 48** | Scales egyptian [0,2,5,7,10], C, 48 | unused | unused |
| 1 | ether | 35 | 1 | **Scales whole [0,2,4,6,8,10], C, 48** | – | – | – |
| 2 | whole_crystals | 45 | 2 / Probability, 4 bars | **Scales whole [0,2,4,6,8,10], C, 48** | Scales augmented [0,3,4,7,8,11], C, 48 | – | – |
| 3 | chinese porcelain | 45 | 1 | **Scales chinese [0,4,6,7,11], E, 40** | – | – | – |
| 4 | sweep | 119 | 1 | **Overtones Even (h=2,4,6…), E, 28** | – | – | – |
| 5 | metal membranes | 120 | 1 | **Micro 17-EDO (70.59 c), E, 40** | – | – | – |
| 6 | microtonal landscapes 1 | 120 | 1 | **Micro 15-EDO (80 c), E, 40** | – | – | – |
| 7 | syncrotron | 99 | 1, 1/32 | **Scales Chromatic [0..11], E, 28** | – | – | – |
| 8 | mello | 24 | 1 | **Scales iwato [0,1,5,6,10], E, 40** | – | – | – |
| 9 | sweep2 | 64 | 1 | **Overtones Even, E, 28** | – | – | – |
| 10 | sweep3 | 98 | 1 | **Overtones Even, E, 28** | – | – | – |
| 11 | chroma | 98 | 1 | **Scales Chromatic [0..11], E, 28** | – | – | – |
| 12 | mediterranean | 47 | 2 / >>, 4 bars | **Scales minPent [0,3,5,7,10], E, 28** | Scales majPent [0,2,4,7,9], E, 28 | – | – |
| 13 | lucky me | 19 | 4 / >>, 2 bars | **Chords Minor [0,3,7], B, 35** | Chords Major [0,4,7], D, 26, mode 3 | Chords Minor [0,3,7], F#, 30, mode 1 | Chords Major [0,4,7], E, 28, mode 1 |
| 14 | mM | 29 | 2 / >>, 4 bars | **Chords m7 [0,3,7,10], C, 24** | Chords M7 [0,4,7,11], C, 24 | – | – |
| 15 | arp | 32 | 2 / >>, 4 bars | **Chords m7 [0,3,7,10], C, 48** | Chords M7 [0,4,7,11], C, 48 | – | – |
| 16 | qbrus | 120 | 1, 1 bar | **Micro 32-EDO (37.5 c), E, 28** | – | – | – |
| 17 | amuze | 20 | 2 / >>, 2 bars | **Chords 7 [0,4,7,10], G, 31** | Chords Major [0,4,7], C, 24 | – | – |
| 18 | beauty | 16 | 2 / >>, 4 bars | **Chords Major [0,4,7], C, 48** | Chords Minor [0,3,7], C, 48 | – | – |

- `mode` is 0 unless stated. Key is -3 except in #7 and #18, where it is 0.
- Per-snapshot values for the unused layers, with all four layers spelled out, are in `SCRATCH/anima/pitch/snap_md.txt`.
- The Scales list names are SuperCollider `Scale` names and their rows match SC's tables (hirajoshi, egyptian, iwato, chinese…). The full list is in `SCRATCH/anima/pitch/scales_list.txt` (entry id = 132 + value) and the table in `SCRATCH/anima/pitch/scales_table.txt`.
- Chords: the list is in `SCRATCH/anima/pitch/chords_list.txt` (entry id = 17 + value) and the table in `SCRATCH/anima/pitch/chords_table.txt`. Note that list names and rows drift in the 13–14 range: entry 'm9+5' (value 14) is row 14 = [0,2,10].
- User tables: L1 = [0,3,7,8,7,8,10…] with n = 4, so [0,3,7,8]. L2-L4 = [0,3,7,8,10] with n = 5. No snapshot uses User.

**Snapshot-start pitches** for layer 1 with the transport stopped and no random sources, including Pitch Modifiers' octave fader 5490 (`pitch_ref.py`), as MIDI note for voices 1..6, then the min..max over all bands:

| # | snapshot | v1..6 | min..max |
|---|---|---|---|
| 0 | Init | 43 44 48 50 51 55 | 43..123 |
| 1 | ether | 44 46 48 50 52 54 | 44..112 |
| 2 | whole_crystals | 32 34 36 38 40 42 | 32..120 |
| 3 | chinese porcelain | 23 27 28 32 34 35 | 23..130 |
| 4 | sweep | NaN NaN 17 29 36.02 41 | 17..99.4 |
| 5 | metal membranes | 26.59 27.29 28 28.71 29.41 30.12 | 26.6..110.6 |
| 6 | microtonal landscapes 1 | random | 13.6..115 |
| 7 | syncrotron | 17..22 | 17..115 |
| 8 | mello | 58 62 64 65 69 70 | 58..86 |
| 9 | sweep2 | NaN NaN 29 41 48.02 53 | 29..100.5 |
| 10 | sweep3 | NaN NaN 29 41 48.02 53 | 29..108 |
| 11 | chroma | 14..19 | 14..111 |
| 12 | mediterranean | 11 14 16 19 21 23 | 11..122 |
| 13 | lucky me | 26 30 35 38 42 47 | 26..98 |
| 14 | mM | 19 22 24 27 31 34 | 19..103 |
| 15 | arp | 43 46 48 51 55 58 | 43..87 |
| 16 | qbrus | 39.62 40.38 40.75 41.5 42.25 43 | 39.6..117.6 |
| 17 | amuze | 38 41 43 47 50 53 | 38..95 |
| 18 | beauty | 76 72 67 64 60 55 (reversed) | 16..76 |

---

## 3. Pitch Modifiers#5460

```
P_band = P_h + fine + 12*OctPM + SemiPM + detune_V + vib_V
```

| id | label | range | role | snapshots |
|---|---|---|---|---|
| 5463 | fine | -1..1 st | global offset | 0 |
| 5490 | (octave) | -3..3 | ×12 st | 0 0 -1 -1 -2 -1 -2 -1 2 -1 -1 -1 -1 0 0 0 1 1 -3 (#0..18) |
| 5514 | (semi) | -12..12 st | global offset | 0 |
| 5466 | det | 0..1 st | random detune amount | #6 = 1, #16 = 0.23, #18 = 0.07, others 0 |
| 5481 | rnd | 0..24 st | additional random amount | 0 |
| 5493 | Switch | Off/On | redraw detune on every Harmony clock step | Off (-1) |
| 5497 | VibA | 0..1 st | vibrato depth (sine amplitude) | 0.15 0.15 0 0 0 0 0.11 0 0.079 0 0 0 0 0 0.094 0.094 0 0.197 0.173 |
| 5500 | Vibf | 0..8 Hz | vibrato rate | 6.31; #6 and #8 = 0.5; #17 = 5.31; #18 = 4.06 |
| 5508 | poly | 0..8 | per-voice rate spread | #6 = 4.2, #18 = 4.9, others 0 |

### Detune (Iteration#5467 → To V#5471, Core Random 5464)

```
amt = det + rndLatched          // rnd fader value latched on gate / fader change
on trigger: for k = 0..B: voice (x0+k) <- r_k * amt
r_k = successive draws of a bipolar LCG: state = state*1103515245 + 12345, seed 9 (set at init)
      out = int32(state) * 2^-31 in [-1,1)
```

- `detune_V` is a static random offset, uniform ±amt semitones, independent per band.
- Triggers:
  - a note gate > 0;
  - any change of det or rnd;
  - amt ≤ 0, which re-sends zeros;
  - every clock step when Switch 5493 is on (it is Off).
- x0 is the trigger event's value (gate velocity, amt, or 1). The voice numbering offset this causes is UNCERTAIN, but it is immaterial: the effect is one fresh random offset per band.

### Vibrato#5495

```
vib_V = VibA * sin(2π f_V t)                                  // Reaktor LFO.Sin, free running, Snc unconnected
f_V   = scan(poly; Vibf, Vibf * u_V),  u_V = |U(-1,1)|        // u_V is per band, redrawn when Vibf changes
        → poly ≥ 1: f_V = Vibf*u_V (uniform 0..Vibf);  poly = 0: all bands at Vibf
```

- The start phase per voice is UNCERTAIN; free-running from voice start is presumably phase 0.
- Selector 5507 being a Scanner (linear for 0 < poly < 1) is INFERRED.
- Event V.C. Min → Meter is display only.

---

## 4. Body (Instrument#2696): Scales#2736 / Core "Scale Fitter" 2737

```
Pin   = Note Pitch (MIDI)                      // into Scales.0 (Value, retriggered on scale change)
Pout  = Pin + A[12*Sel + ((Pin - Base) imod 12)] + root
Body osc pitch = Pout + 12*Fader2715 + Fader3130
```

- `Base` is unconnected (0), and `root` (In 2745) is unconnected (0).
- `Sel` = In<scale>#2746. It comes from the ensemble Snapshot module#12554, output index 2 (port meaning UNCERTAIN).
- **Array A is empty.** The Init Array property holds 5 zero bytes and size 0, so every offset reads 0 and **the Scale Fitter does nothing: Pout = Pin**. A factory table could exist elsewhere, but none was found in the file (UNCERTAIN).
- Faders 2715 and 3130 are not in the snapshots, and the `controls.txt` values for them (-4 / -48) are garbage. The saved panel state at +0x112 gives octave **-3** and offset **0**, so the Body pitch = note - 36. This is the Body spec's territory; it is recorded here only for the pitch sum.
- The Body pitch is not quantised to the Harmony scale. There is no link between the two.

---

## Implementation summary (AnimaBox)

1. `idx_V = V*Step` (1-based), plus the order options: skew ±1/0 (`pitch_ref.pitch_index`), rept, comb, invert, symm, and rnd dev (additive uniform, abs, round).
2. `deg = idx + Key(-3 default) + mode`, and `P = scale[deg mod n] + 12*floor(deg/n) + base + trans`.
   - With an external scale, `base = root + 12*oct`, or take absolute ChordBox notes.
   - Micro and Overtones are the formulas above.
3. Layer sequencing:
   - **Simple:** L1 only.
   - **Full:** up to 4 scales stepped by transport (every 1/32 note to 4 bars, Len), with a sigmoid glide of T ms crossfading the pitches.
4. Add the per-band random ±octave (Poly8) and random transpose/inversion per clock step, as used by syncrotron, chinese porcelain and beauty.
5. `P_band = P + 12*OctPM + fine/semi + U(-det,det)_V + VibA*sin(2π f_V t)`.

### Notes for the external-scale port

- **The scale vector must accept fractional pitch classes.** Micro N-EDO is exactly the N-note scale `pcs[k] = k*12/N` under the same wrap formula (#5 = 17-EDO, #6 = 15-EDO, #16 = 32-EDO).
- **Overtones need their own mode.** They do not repeat at the octave (#4, #9, #10): use `1 + 12*log2(h(deg))`, not a vector.
- **Fractional idx.** With pow shape ≠ 0 and Q = 0, idx is not an integer and the Chords/Scales/User table read gets a fractional degree. No snapshot does this. The proposed convention is to round deg before the lookup (Reaktor's Event Table read truncation is UNCERTAIN). Micro and Overtones take a fractional deg as is.

### Open items

- Markov "Probability" transition tables (snapshot compound states 616 and 618-620) are not decoded. Only #2 whole_crystals uses this mode, with 2 layers.
- The clock modes rndm (cell 10708), manual and slide are not detailed. No snapshot uses them.
- Selector vs Scanner modes are INFERRED (10830, 12418, 12433, 5507).
- Reaktor's x%0 and Log(≤0) outputs, and Note Pitch's value before the first note.
