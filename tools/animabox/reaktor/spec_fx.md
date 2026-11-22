# spec_fx — AnimaMundi effect instruments (Metaverb 2.7b, delay2 "Delay 4p")

Sources: am.json via net2.py (Primary) and core_view.py (Core cells 12224, 12711).
Conventions: P2F(p) = 8.17742 * 2^(p/12) Hz (Reaktor Core constant, ~MIDI pitch, 69 -> 440.0);
P2smp(p) = SR / P2F(p) samples. Core "Exp Approx" (order 2/3 polynomial 2^x) is used everywhere —
replicate with exact pow; error is tiny.

---------------------------------------------------------------------------------------------------
## 1. Metaverb (Instrument#12218 -> Macro#12219 -> Core cell 12224 "Metaverb 2.7b", 4673 modules)

### 1.1 Top-level wiring
- Instrument#12218: In L/R -> Metaverb L/R -> Out L/R. Stereo in, stereo out. No other routing.
- Macro#12219 panel faders (all 0..1) -> Core inputs:

| id | label | panel value (saved) | Core input |
|---|---|---|---|
| 12225 | Lo Cut | 0.005 | HP = LoCut |
| 12226 | Hi Cut | 0.595 | LP = HiCut |
| 12227 | Mix | 0.0 | Mix = **Mix²** (X#12231 multiplies the fader by itself) |
| 12228 | Size | 0.865 | Sz = Size |

  (M#12230 + IC Send#12229 just broadcast Size/Lo/Hi/Mix² to an internal connection; ignore.)
- NOTE: saved Mix = 0 -> the Metaverb output is 100 % dry in the saved state.

### 1.2 Signal flow (one sample)
```
inL, inR                                  // reverb inputs (no pre-delay, no input filter)
xL = inL + z1(FB_R)                       // cross-coupled "figure-8" loop, z1 = 1-sample delay
xR = inR + z1(FB_L)
xL,xR -> HighShelf1(HS_F, Hi_dB) -> LowShelf1(LS_F, Lo_dB)       // IN the loop
L chain: a1 = AP(xL, D=nL*0.506394 + M1 + 1, g)                  // allpasses ("Diff Delay")
         a2 = AP(a1, D=nL*0.803851 + M2 + 1, g)
         a3 = AP(a2, D=nL*1.000000 + M3 + 1, g)       -> wetL = a3
         dL = Delay(a3, D=nL*1.498307 + M4)           ("Delay 4P", plain)
         FB_L = dL * AF_L
R chain: same with nR, M1b..M4b, AF_R:      wetR = a3R, FB_R = dR * AF_R
outL = inL*(1-m) + wetL*m ;  outR = inR*(1-m) + wetR*m           // m = smoothed Mix²
```
So one full recirculation = Filter -> L chain (3 AP + delay) -> AF_L -> Filter -> R chain -> AF_R -> back.
The L output tap is the 3rd allpass output of the L chain (before its long delay), same for R.
No saturation anywhere (only Denormal Cancel). Delays/allpasses all share one 16384-sample write
counter (IncWrap 16384); each line has its own 16384-sample array.

### 1.3 Parameter maths ("Parameters" macro; event rate, recomputed on fader change)
```
s      = Size * (0.31 + 0.69*Size)                    // "Par Ctl Shaper", b = -0.69
yDn(s) = (s < 0.2) ? 12  * ((0.2 - s)/0.2)^2 : 0      // "Y dwn BP 0" (bp 0.2, L 12)
yUp(s) = (s < 0.2) ? 1 - 0.4*((0.2 - s)/0.2)^2 : 1    // "Y up BP 1"  (bp 0.2, L 0.4)

SizeL_p = 30 - 16*s + yDn(s)                          // pitch
SizeR_p = SizeL_p - (1 + 2*Size*(2 - Size))           // R is 1..3 semitones LOWER (= longer)
nL = P2smp(SizeL_p) ; nR = P2smp(SizeR_p)             // base lengths in samples (SR-scaled -> fixed ms)
g (Dffs) = -( (max(s,0.0001)/24 + 0.5) * yUp(s) )     // allpass coefficient, NEGATIVE, -0.30 .. -0.5417
RT_p   = -96*s ;  RT = P2smp(RT_p) samples = 0.122288 s * 256^s   (0.122 s .. 31.3 s)
ModAmt = 0.5 + 2*Size*(1.5 - Size)                    // 0.5 .. 1.625 (raw Size, not s)

AF_L = 10^( -3 * DT_L / RT ),  DT_L = nL*1.498307 + M4   // "RT60": dB = max(-60*DT/RT, -144) -> amp
AF_R = 10^( -3 * DT_R / RT ),  DT_R = nR*1.498307 + M4b
```
Only the long delay's length enters RT60, so the real decay is longer (allpasses + both
chains); see the eff. RT60 column in 1.6 (≈ 2.5-2.6 x RT), which ignores the shelves.

Hi Cut / Lo Cut ("FB Filter" macro):
```
x = 1 - HiCut :  HS_F = P2F(120 - 34*x)  (8374 Hz .. 1175 Hz),  Hi_dB = -12 * x*(2-x)  (0 .. -12 dB)
y = LoCut     :  LS_F = P2F(40 + 24*y)   (82.4 Hz .. 329.7 Hz), Lo_dB = -18 * y*(2-y)  (0 .. -18 dB)
```
Saved: HiCut 0.595 -> HS_F 3780 Hz, Hi_dB -7.75 dB ; LoCut 0.005 -> LS_F 83.0 Hz, Lo_dB -0.18 dB.
These are cut amounts applied ONCE PER CHAIN PASS (twice per full figure-8 loop).

Shelves (1st-order bilinear, Core "6dB LP/HP EQ"), A = 10^(dB/20), r = 10^(dB/40) = sqrt(A):
```
HighShelf: y = x + (A-1) * HP1(x, fc = min(HS_F * r, SR))
LowShelf : y = x + (A-1) * LP1(x, fc = min(LS_F / r, SR))
1-pole bilinear: w = tan(pi*fc/SR)  (Core uses series w+w^3/3+2w^5/15), a0=1+w, a1=w-1
   v = (x - a1*v1)/a0 ;  LP = w*(v + v1) ;  HP = v - v1 ;  v1 <- v
```
(Exactly SC-equivalent: HP1 = bilinear 1st-order HPF; can use `HPF`-free formula via FOS:
 LP1: FOS(x, w/a0, w/a0, -a1/a0), HP1: FOS(x, 1/a0, -1/a0, -a1/a0).)

Smoothing (Core "Mnogo Smusas" = linear ramps, clocked at the internal control clock):
- nL, nR (samples) and AF_L, AF_R: linear ramp, T = 0.3 s.
- HS_F, Hi_dB, LS_F, Lo_dB: linear ramp, T = 0.05 s.
- Mix: one-pole exponential at audio rate, coef = 0.01^(1/(N-1)), N = 0.05*SR (1 % after 50 ms) -> SC `Lag.ar(m, 0.075)`.
  ("Odin smusa" verified: ramp accumulates Inc = 1/(CR*T) per control tick, out = Base + ramp*Diff.)
- g (Dffs), RT, ModAmt: not smoothed (step on change).
Control clock: Div = SR/9700, ticks every 4 samples at 44.1/48 kHz (9 at 96 kHz), but all
increments assume 9700 Hz -> ramps really take T*9700/(SR/4) (0.264 s / 0.044 s at 44.1k). Minor.

### 1.4 Modulation ("LFO 4phse")
```
F = 0.66 Hz nominal (effective 0.66*(SR/4)/9700 = 0.750 Hz @44.1k, 0.816 Hz @48k)
a = ModAmt * 0.0005 * SR       // samples (= ModAmt * 0.5 ms); saved Size -> a = 35.25 smp @44.1k
phi in [-0.5,0.5) cycles; Par(x) = x*(8 - 16*|x|) (parabolic sine ≈ sin(2*pi*x)); wrap(x)=x-round(x)
M1  = a*(1 + Par(phi))            M1b = a*(1 + Par(wrap(phi+0.125)))
M2  = a*(1 + Par(wrap(phi+0.25))) M2b = a*(1 - Par(wrap(phi+0.125)))
M3  = a*(1 - Par(phi))            M3b = a*(1 + Par(wrap(phi+0.375)))
M4  = a*(1 - Par(wrap(phi+0.25))) M4b = a*(1 - Par(wrap(phi+0.375)))
```
i.e. 8 quadrature/45° phases, unipolar 0..2a samples added to each line length (lines only
get longer). SC: `LFPar` is the same parabolic shape; or SinOsc (difference inaudible).

### 1.5 Delay lines
- "Diff Delay 4P 16384" (allpass): array 16384, write w = x + g*y, y = d - g*x, where d is the
  4-point Hermite read at ns samples behind the write index, latched one sample (z-1) ->
  effective loop delay D = ns + 1. Transfer (z^-D - g)/(1 - g z^-D) = exactly SC AllpassC's form.
- "Delay 4P 16384": plain delay, Hermite 4-point (= SC cubic interp), D = ns.
- ns clipped to [0, 16382]. Max needed ≈ 4280 + 72 samples @44.1k (Size 1, R) -> 0.1 s.
- Engine adds one more z^-1 on each feedback path (in "z^-1" before the input sum).

### 1.6 Numbers vs Size (SR 44.1k; ns per line in samples, AF include +a)
| Size | s | nL | nR | g | RT (s) | ModAmt | a smp | AF_L | AF_R | eff. RT60* |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 476.7 | 505.0 | -0.300 | 0.122 | 0.5 | 11.0 | 0.395 | 0.374 | ~0.3 s |
| 0.25 | 0.1206 | 955.5 | 1064.8 | -0.473 | 0.239 | 1.125 | 24.8 | 0.385 | 0.345 | ~0.6 s |
| 0.5 | 0.3275 | 1290.3 | 1490.8 | -0.514 | 0.752 | 1.5 | 33.1 | 0.664 | 0.624 | ~1.9 s |
| 0.75 | 0.6206 | 1691.8 | 1997.4 | -0.526 | 3.82 | 1.625 | 35.8 | 0.900 | 0.883 | ~9.8 s |
| **0.865 (saved)** | 0.7844 | 1968.3 | 2335.8 | -0.5327 | 9.47 | 1.5985 | 35.25 | 0.9518 | 0.9432 | ~24 s |
| 1 | 1 | 2402.3 | 2856.8 | -0.542 | 31.3 | 1.5 | 33.1 | 0.982 | 0.979 | ~80 s |
*broadband, ignoring the in-loop shelves (Hi Cut shortens the HF decay a lot).
Saved Size line lengths (samples @44.1k): L 996.7 / 1582.2 / 1968.3 / 2949.1 ; R 1182.8 / 1877.6 / 2335.8 / 3499.8.
Ratios: 2^(-11.78/12)=0.506394, 2^(-3.78/12)=0.803851, 1, 2^(7/12)=1.498307.

## MetaVerb SynthDef design

Controls: `in` (stereo bus), `out`, `size`=0.865, `loCut`=0.005, `hiCut`=0.595, `mix`=0 (panel units 0..1).
All lengths are computed in samples then divided by SR (SampleRate.ir) -> fixed times in seconds.

```supercollider
SynthDef(\MetaVerb, { |in=0, out=0, size=0.865, loCut=0.005, hiCut=0.595, mix=0|
    var sr = SampleRate.ir, p2f = { |p| 8.17742 * (2 ** (p/12)) };
    var dry = In.ar(in, 2), sz, s, yq, pL, pR, nL, nR, g, rt, modAmt, a, phi, par, m;
    var hx, hsF, hiDb, lsF, loDb, fb, xL, xR, chain, wetL, wetR, dL, dR, afL, afR, mixS;
    var lfoRate = 0.66 * (sr/4) / 9700;     // Core control clock quirk (0.750 Hz @44.1k)
    // 1st-order bilinear shelves (Core "HS"/"LS"): y = x + (A-1)*HP1/LP1
    var onePole = { |x, fc, hp|
        var w = tan(pi * fc.clip(1, sr*0.49) / sr), a0 = 1 + w, b1 = (1 - w) / a0;
        if(hp) { FOS.ar(x, 1/a0, -1/a0, b1) } { FOS.ar(x, w/a0, w/a0, b1) } };
    var shelfHS = { |x, f, db| var A = db.dbamp; x + ((A - 1) * onePole.(x, f * A.sqrt, true)) };
    var shelfLS = { |x, f, db| var A = db.dbamp; x + ((A - 1) * onePole.(x, f / A.sqrt, false)) };
    sz = size.clip(0,1);
    s  = sz * (0.31 + (0.69*sz));
    yq = ((0.2 - s)/0.2).max(0).squared;                    // 0 when s >= 0.2
    pL = 30 - (16*s) + (12*yq);
    pR = pL - (1 + (2*sz*(2 - sz)));
    nL = VarLag.kr(sr / p2f.(pL), 0.3, 0);                   // linear 300 ms ramp
    nR = VarLag.kr(sr / p2f.(pR), 0.3, 0);
    g  = ((s.max(0.0001)/24) + 0.5) * (1 - (0.4*yq)) * -1;  // -0.30 .. -0.5417
    rt = sr / p2f.(-96*s);                                   // samples
    modAmt = 0.5 + (2*sz*(1.5 - sz));
    a  = modAmt * 0.0005 * sr;                               // samples
    // 8-phase unipolar LFO, Core parabolic sine Par(x) = x*(8-16|x|), x = wrap(phi+off) in [-0.5,0.5)
    phi = LFSaw.ar(lfoRate) * 0.5;
    par = { |off| var x = (phi + off).wrap(-0.5, 0.5); x * (8 - (16 * x.abs)) };
    // M1..M4 (L) and M1b..M4b (R), offsets in cycles as decoded
    m = [ a*(1+par.(0)), a*(1+par.(0.25)), a*(1-par.(0)),     a*(1-par.(0.25)),
          a*(1+par.(0.125)), a*(1-par.(0.125)), a*(1+par.(0.375)), a*(1-par.(0.375)) ];
    hx = 1 - hiCut.clip(0,1);
    hsF  = VarLag.kr(p2f.(120 - (34*hx)), 0.05, 0);  hiDb = VarLag.kr(-12*hx*(2-hx), 0.05, 0);
    lsF  = VarLag.kr(p2f.(40 + (24*loCut.clip(0,1))), 0.05, 0);
    loDb = VarLag.kr(-18*loCut*(2-loCut), 0.05, 0);
    afL = VarLag.kr(10 ** (-3 * ((nL*1.498307) + a) / rt), 0.3, 0);   // a = mean of M4
    afR = VarLag.kr(10 ** (-3 * ((nR*1.498307) + a) / rt), 0.3, 0);

    fb = LocalIn.ar(2);                                      // [FB_L, FB_R] (already * AF)
    xL = dry[0] + fb[1];  xR = dry[1] + fb[0];               // cross-coupled figure-8
    // in-loop shelves: HS then LS, 1st-order bilinear, implemented with FOS (see 1.3)
    xL = shelfLS.(shelfHS.(xL, hsF, hiDb), lsF, loDb);
    xR = shelfLS.(shelfHS.(xR, hsF, hiDb), lsF, loDb);
    // allpass with fixed coefficient g: AllpassC decay = sign(g)*D*ln(0.001)/ln(|g|)
    chain = { |x, n, mm|
        var y = x;
        [0.506394, 0.803851, 1.0].do { |r, i|
            var d = ((n*r) + mm[i] + 1) / sr;                // +1 = Core z-1 latch
            y = AllpassC.ar(y, 0.2, d, d * (-6.907755) / g.abs.log * g.sign);
        };
        y };
    wetL = chain.(xL, nL, m[0..2]);
    wetR = chain.(xR, nR, m[4..6]);
    // long delay; +1 (engine z^-1) - one block (LocalIn latency)
    dL = DelayC.ar(wetL, 0.25, ((nL*1.498307) + m[3] + 1 - BlockSize.ir) / sr);
    dR = DelayC.ar(wetR, 0.25, ((nR*1.498307) + m[7] + 1 - BlockSize.ir) / sr);
    LocalOut.ar([dL * afL, dR * afR]);
    mixS = Lag.ar(K2A.ar(mix.squared), 0.075);
    Out.ar(out, (dry * (1 - mixS)) + ([wetL, wetR] * mixS));
}).add;
```
Implementation notes:
- Shelves: HS: `x + (A-1)*HP1(x, hsF*sqrt(A))`, LS: `x + (A-1)*LP1(x, lsF/sqrt(A))`, A = dbamp(dB);
  w = tan(pi*fc/sr), a0=1+w, a1=w-1; HP1 = FOS.ar(x, 1/a0, -1/a0, (1-w)/a0);
  LP1 = FOS.ar(x, w/a0, w/a0, (1-w)/a0). (Or `BHiShelf/BLowShelf` 2nd-order as a cheaper approximation — not identical.)
- Block-size compensation in the long delays keeps the figure-8 loop length exact; the minimum
  long delay (714 samples at Size 0) is far above any block size.
- AF uses the mean modulation `a` instead of the instantaneous M4 (Core uses instantaneous, then
  ramps 0.3 s — the difference is < 0.1 dB/pass).
- LFO rate uses the Core quirk (phase step assumes 9700 Hz, clock really ticks every 4 samples at
  44.1/48k); use plain 0.66 Hz if the "nominal" rate is preferred. At 96k the Core ticks every 9 samples
  (0.726 Hz) — the formula above gives 1.63 Hz there; compute period = floor(SR/9700 - 1) + 1 if exactness matters.
- Max delay at 96 kHz: ~8700 samples ≈ 0.09 s — 0.2/0.25 s buffers are ample.

---------------------------------------------------------------------------------------------------
## 2. delay2 (Instrument#12635 -> Macro#12685 -> Core cell 12711 "Delay 4p", 273 modules)

### 2.1 Top-level wiring (Instrument#12635)
In L -> To Voice 1, In R -> To Voice 2, summed (+#12660) -> delay2 (one mono macro, run per voice)
-> From Voice 1 -> Out L, From Voice 2 -> Out R. So it is **dual-mono**: L and R each run an
independent copy of the same delay with the same panel settings (no ping-pong, no crossfeed).

### 2.2 Panel controls (saved values from .ens; snapshots don't store them)
| id | range | saved | role |
|---|---|---|---|
| 12690 | 0..1 | 0.4 | feedback gain (fb = value, linear) |
| 12693 | 0..1 | 0.3 | wet amount: Selector/Scanner Pos between dry (in 0) and dry+wet (in 1) |
| 12695 Mode | HP2,BP2,LP2,BP4,BL4,LP4,PEAK | 7 = PEAK | which filter output is the wet/feedback signal |
| 12696 | 20..120 | 58 (233.1 Hz) | filter pitch P (both Multi/LP 4-Pole and Peak EQ) |
| 12697 | 0..0.98 | 0.57 | Multi/LP 4-Pole Res; Peak EQ boost B = 10*value dB (5.7 dB) |
| 12700 | 1..16 | 2.06 | tempo divider "numerator" |
| 12702 | 1..256 | 3.99 | tempo "denominator" (Event Smoother on it) |
| 12707 | 2-pos switch | 2 | pos1 = effect (Scanner out), **pos2 = dry In (bypass)** |

NOTE: saved 12707 = 2 selects the plain input -> delay2 is BYPASSED in the saved state
(port 0 = Scanner out, port 1 = In#12687; switch positions are 1-based -> pos 2 = port 1).
Caveat: for both switches here the saved int equals the switch's input count (7 / 2); across all 36
switches in the ensemble 21 differ from their input count, so the field is a position, but treat
"Mode = PEAK" and "bypass" as LIKELY rather than certain.

### 2.3 Maths
```
bps   = Tempo Info (host tempo in beats per SECOND; BPM/60)
        Divide#12701: input 0 (X, dividend) = bps, input 1 (Y) = den_s (catalog help confirms)
den_s = EventSmoother(12702)                       // ramp, UNCERTAIN time (~100 ms; props show 100/200)
T_ms  = 1000 / ((bps / den_s) * num) = 1000 * den_s / (bps * num)     // num = 12700
      = (den/num) beats.   @120 BPM, saved: 1000*3.99/(2*2.06) = 968.4 ms
T_ms  -> HP/LP 1-Pole LP at P=10 (14.57 Hz, tau ≈ 10.9 ms) at audio rate  (glide on time changes)
delay (Core "Delay 4p"): T_s = T_ms*0.001 ; D = T_s*SR samples, clipped to [0, 44100-2] samples
      (array fixed 44100 samples -> max 1.0 s @44.1k, 0.919 s @48k; saved 968 ms @48k clips to 918.7 ms)
      4-point Hermite interpolation (= SC DelayC cubic).

loop (per sample):
  dly  = Delay(in + fb*filt, D)                      // fb = 12690
  hp2,bp2,lp2,bp4,bl4,lp4 = MultiLP4(dly, P=12696, Res=12697)
  peak = PeakEQ(dly, P=12696, B=10*12697 dB, Res=0.5)
  filt = select(Mode, [hp2,bp2,lp2,bp4,bl4,lp4,peak])
  scan = (1-pos)*in + pos*(in + filt)  = in + pos*filt      // pos = 12693; Scanner, assumed Linear (UNCERTAIN)
  out  = (switch12707 == 1) ? scan : in
```
Saved PEAK mode: loop gain = 0.4 * (1 at most freqs, 1.93 = +5.7 dB around 233 Hz) -> 0.77 peak, stable.
Peak EQ Res 0.5 ≈ Q 1 (Reaktor Res->Q mapping UNCERTAIN; Q≈1/(2*(1-Res)) is a fair guess).
Multi/LP 4-Pole = Reaktor 4-pole multimode (2 cascaded SVFs); for SC use `BMoog`/`RLPF` cascade or
DFM1; exact Res mapping UNCERTAIN (Res 0.98 ≈ self-oscillation).

SC sketch (per channel):
```supercollider
var t = (1000*den.lag(0.1)/(bps*num)).lag(1/(2pi*14.57)) * 0.001;   // OnePole equivalent
var fbIn = LocalIn.ar(2);
var d = DelayC.ar(inSig + (fbIn*fbAmt), 1.0, (t.min(44098/SampleRate.ir) - ControlDur.ir).max(0));
var f = Select.ar(mode, [hp2, bp2, lp2, bp4, bl4, lp4, MidEQ.ar(d, 233.08, 1, 5.7)]);
LocalOut.ar(f);  out = Select.ar(onOff, [inSig + (f*wet), inSig]);
```
(Subtract one block in the delay because LocalIn adds it; Core reads the loop with no extra delay.)
