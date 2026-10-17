//
//  OceanodeAnalog.cpp — AnalogBox's UGens.
//
//  MorphOsc.ar(freq, pm, skew, round, pw, centered, syncFreq)
//    The shared LFOs' morphing shape (scModulation / ModLfo: Width, Skew,
//    Round: triangle .. sine .. square) as a band-limited audio oscillator.
//    The shape is evaluated at 4x the sample rate; every corner (slope change)
//    and step (jump) of the shape gets a polyBLAMP / polyBLEP correction where
//    the phase crosses it (found from the shape itself: the breakpoints of the
//    piecewise warps, their sizes measured on either side), then two halfband
//    FIRs (19 and 63 taps, Kaiser, ~80 dB) decimate. Very steep stretches (Round near
//    1) are measured over half a sample, so they are corrected as steps.
//      freq      Hz (audio rate: exponential / linear FM from the synth)
//      pm        phase modulation, in cycles (audio rate)
//      skew      -1..1, round 0..1, pw 0..1, centered 0 / 1 (as the LFOs)
//      syncFreq  > 0: hard sync to a master at this frequency (exact: the
//                reset lands between samples, with its step corrected)
//    Output -1..1 (asymmetric shapes carry DC: LeakDC after it).
//
//  AnalogFilter.ar(in, freq, res, mode, type, drive, sat, feedback)
//    Zero-delay-feedback (TPT) filters: mode 0 SVF 2-pole, 1 SVF 4-pole
//    (resonance on the second stage), 2 ladder 2-pole, 3 ladder 4-pole;
//    type 0 LP, 1 BP, 2 HP, 3 notch. drive: input gain into the filter's tanh
//    (on its input / in its loop: the resonance squashes and growls), the
//    level made up after; sat 0..1: an asymmetric saturator after the filter;
//    feedback 0..1: the output back into the input through a clipper and a DC
//    block (screams with resonance). Cutoff at audio rate is fine.
//

#include "SC_PlugIn.h"
#include <algorithm>
#include <cmath>

static InterfaceTable* ft;

namespace {

inline float inAt(Unit* unit, int index, int i) {
    return INRATE(index) == calc_FullRate ? IN(index)[i] : IN0(index);
}
inline double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline double frac(double x) { return x - std::floor(x); }
// LinLin then clip (as the SynthDef's LinLin.kr(...).clip(...))
inline double lin(double x, double a, double b, double c, double d) {
    if (b == a) return c;
    return c + (x - a) / (b - a) * (d - c);
}

// ── The LFOs' shape (modulation.scd, ModLfo) ────────────────────────────────
struct ShapeParams {
    double skew = 0, round = 0.5, pw = 0.5;
    bool centered = true;
};

inline double customPow(double v, double amt) {
    const double k1 = 2.0 * amt * 0.99999;
    const double k2 = k1 / ((-amt * 0.999999) + 1.0);
    const double k3 = k2 * std::fabs(v) + 1.0;
    return v * (k2 + 1.0) / std::max(k3, 1e-6);
}

// Width (narrowing / widening) then skew: the phase the shape reads
struct Warp {
    double midpoint, leftMax, rightMin, lo, hi, threshold, pw, skew;
    bool centered;
    void set(const ShapeParams& s) {
        pw = clampd(s.pw, 0.001, 1.0);
        skew = clampd(s.skew, -1.0, 1.0);
        centered = s.centered;
        midpoint = clampd(lin(skew, -1, 1, 1, 0), 0, 1);
        leftMax = clampd(lin(1 - pw, 0, 0.5, 0, midpoint), 0, 1);
        rightMin = clampd(lin(pw, 0.5, 1, midpoint, 1), 0, 1);
        lo = clampd(0.5 - pw, 0, 1);
        hi = clampd(0.5 + pw, 0, 1);
        threshold = clampd(0.5 - skew * 0.5, 0, 1);
    }
    double width(double ph) const {
        if (pw < 0.5) {
            double n = centered ? clampd(lin(ph, lo, hi, 0, 1), 0, 1)
                                : clampd(lin(ph, 0, std::min(std::max(pw * 2, 1e-7), 1.0), 0, 1), 0, 1);
            // after the narrowed window: back to the cycle's start (it rests there
            // until the next one; the LFOs only do this with a negative skew)
            if (n >= 0.99999) n = 0;
            return n;
        }
        if (pw >= 0.99999) return midpoint;
        if (ph < midpoint) return clampd(lin(ph, 0, std::max(leftMax, 1e-7), 0, midpoint), 0, 1);
        return clampd(lin(ph, std::min(rightMin, 1 - 1e-7), 1, midpoint, 1), 0, 1);
    }
    double skewed(double w) const {
        if (w >= threshold) return clampd(lin(w, std::min(threshold, 1 - 1e-7), 1, 0.5, 1), 0.5, 1);
        return clampd(lin(w, 0, std::max(threshold, 1e-7), 0, 0.5), 0, 0.5);
    }
    double operator()(double ph) const { return skewed(width(ph)); }
    // where the shape's window ends (narrowing: the warp drops to 0 after it)
    double windowEnd() const {
        if (pw >= 0.5) return 1.0 - 1e-12;
        // just before the drop (the warp reaches 0.99999 there)
        const double a = centered ? lo : 0.0, b = centered ? hi : std::min(1.0, pw * 2);
        return a + (b - a) * 0.99998;
    }
};

inline double shapeAt(double ph, double round) {
    const double w = ph * 2.0 * M_PI;
    if (round >= 1.0) return (ph >= 0.25 && ph < 0.75) ? 1.0 : 0.0;
    const double cosRaw = std::cos(w + M_PI);
    if (round >= 0.5) return clampd((customPow(cosRaw, (round - 0.5) * 2.0) + 1.0) * 0.5, 0, 1);
    const double tri = std::max(0.0, 1.0 - std::fabs(2.0 * ph - 1.0));
    const double cosMapped = clampd((cosRaw + 1.0) * 0.5, 0, 1);
    return tri + (cosMapped - tri) * (round / 0.5);
}

// ── Halfband decimators (Kaiser, ~80 dB): 4x -> 2x (19 taps, the wide
// transition there), 2x -> 1x (63 taps: pass to 20 kHz, stop from 28 kHz) ──
template<int N>
struct HalfbandTaps {
    static constexpr int TAPS = N, CENTER = N / 2;
    float h[N];
    HalfbandTaps() {
        const int HB_TAPS = N, HB_CENTER = N / 2;
        auto bessel0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 40; k++) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
            return sum;
        };
        const double beta = 7.86;
        double total = 0;
        for (int n = 0; n < HB_TAPS; n++) {
            const double m = n - HB_CENTER;
            const double sinc = m == 0 ? 1.0 : std::sin(M_PI * m / 2.0) / (M_PI * m / 2.0);
            const double r = m / HB_CENTER;
            const double win = bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(beta);
            h[n] = (float)(0.5 * sinc * win);
            total += h[n];
        }
        for (float& x : h) x = (float)(x / total);
    }
};
constexpr int HB_TAPS = 63, HB_CENTER = 31, HB1_TAPS = 19, HB1_CENTER = 9;
constexpr int OVERSAMPLE = 4;
const HalfbandTaps<HB_TAPS>& halfband() {
    static const HalfbandTaps<HB_TAPS> taps;
    return taps;
}
const HalfbandTaps<HB1_TAPS>& halfband1() {
    static const HalfbandTaps<HB1_TAPS> taps;
    return taps;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// MorphOsc
// ════════════════════════════════════════════════════════════════════════════

enum { kFreq, kPm, kSkew, kRound, kPw, kCentered, kSync };

struct MorphOsc : public Unit {
    double phase, master, prevPhi;
    float prevPm;
    double prevY;                  // the last oversampled value (still open to corrections)
    float hist[HB_TAPS * 2];       // 2x -> 1x history (doubled ring: contiguous reads)
    int histPos;
    float hist1[HB1_TAPS * 2];     // 4x -> 2x history
    int hist1Pos;
    ShapeParams shape;
    Warp warp;
    double h;                      // measuring window (phase) of the breakpoints
    // breakpoints: phase, jump, slope change (per unit phase)
    static constexpr int MAX_BP = 12;
    double bp[MAX_BP], bpJump[MAX_BP], bpSlope[MAX_BP];
    int numBp;
    bool primed;
};

static void MorphOsc_next(MorphOsc* unit, int inNumSamples);

static inline double morphValue(const MorphOsc* unit, double phi) {
    return shapeAt(unit->warp(frac(phi)), unit->shape.round) * 2.0 - 1.0;
}

// The smallest phase whose warped value reaches `target` (the warp only
// rises, except the one drop narrowing makes, itself a breakpoint)
static double inverseWarp(const Warp& w, double target) {
    const double end = w.windowEnd();
    if (w(end) < target) return -1;
    double lo = 0, hi = end;
    for (int k = 0; k < 48; k++) {
        const double mid = 0.5 * (lo + hi);
        if (w(mid) >= target) hi = mid; else lo = mid;
    }
    return hi;
}

// Breakpoints of the shape now, with their step and corner sizes
static void MorphOsc_breakpoints(MorphOsc* unit, double dt) {
    Warp& w = unit->warp;
    double cand[24];
    int n = 0;
    cand[n++] = 0.0;
    if (w.pw < 0.5) {
        if (w.centered) { cand[n++] = w.lo; cand[n++] = w.hi; }
        else cand[n++] = std::min(1.0, w.pw * 2);
    } else if (w.pw < 0.99999) {
        cand[n++] = w.leftMax; cand[n++] = w.rightMin; cand[n++] = w.midpoint;
    }
    // the skew threshold, then the shape's own corners / edges (0.25, 0.5, 0.75)
    {
        Warp widthOnly = w;
        // the width stage alone: find where it reaches the threshold
        double lo = 0, hi = w.windowEnd();
        if (w.width(hi) >= w.threshold) {
            for (int k = 0; k < 48; k++) {
                const double mid = 0.5 * (lo + hi);
                if (widthOnly.width(mid) >= w.threshold) hi = mid; else lo = mid;
            }
            cand[n++] = hi;
        }
    }
    for (double target : {0.25, 0.5, 0.75}) {
        const double p = inverseWarp(w, target);
        if (p >= 0) cand[n++] = p;
    }
    std::sort(cand, cand + n);
    const double hh = clampd(std::fabs(dt) * 0.5, 1e-6, 0.02);
    unit->h = hh;
    unit->numBp = 0;
    double last = -1;
    for (int i = 0; i < n && unit->numBp < MorphOsc::MAX_BP; i++) {
        const double b = frac(cand[i]);
        if (last >= 0 && std::fabs(b - last) < 1e-9) continue;
        last = b;
        const double fl = morphValue(unit, b - hh), fl2 = morphValue(unit, b - 2 * hh);
        const double fr = morphValue(unit, b + hh), fr2 = morphValue(unit, b + 2 * hh);
        const double sl = (fl - fl2) / hh, sr = (fr2 - fr) / hh;
        // the step: both sides extrapolated to b (a steep stretch inside the window counts as a step)
        const double jump = (fr - sr * hh) - (fl + sl * hh);
        const double slope = sr - sl;
        if (std::fabs(jump) < 1e-7 && std::fabs(slope) < 1e-5) continue;
        unit->bp[unit->numBp] = b;
        unit->bpJump[unit->numBp] = jump;
        unit->bpSlope[unit->numBp] = slope;
        unit->numBp++;
    }
}

static void MorphOsc_Ctor(MorphOsc* unit) {
    unit->phase = 0;
    unit->master = 0;
    unit->prevPm = 0;
    unit->prevPhi = 0;
    unit->prevY = 0;
    std::fill(unit->hist, unit->hist + HB_TAPS * 2, 0.0f);
    unit->histPos = 0;
    std::fill(unit->hist1, unit->hist1 + HB1_TAPS * 2, 0.0f);
    unit->hist1Pos = 0;
    unit->numBp = 0;
    unit->primed = false;
    halfband();
    halfband1();
    SETCALC(MorphOsc_next);
    MorphOsc_next(unit, 1);
    // the first sample again from the start (the Ctor's call only primed the state)
    unit->phase = 0;
    unit->master = 0;
    unit->prevY = 0;
    std::fill(unit->hist, unit->hist + HB_TAPS * 2, 0.0f);
    unit->histPos = 0;
    std::fill(unit->hist1, unit->hist1 + HB1_TAPS * 2, 0.0f);
    unit->hist1Pos = 0;
}

// Corrections for a phase segment a -> a + d (d may be negative: PM) lasting
// `dur` oversampled samples and ending `after` samples before the current one
static inline void MorphOsc_cross(MorphOsc* unit, double a, double d, double dur, double after, double& yPrev, double& yCur) {
    if (d == 0) return;
    const double ad = std::fabs(d);
    const double dir = d > 0 ? 1.0 : -1.0;
    for (int k = 0; k < unit->numBp; k++) {
        const double b = unit->bp[k];
        const double x = d > 0 ? frac(b - a) : frac(a - b);   // distance to the breakpoint along the motion
        if (x <= 0 || x > ad) continue;
        // time from the crossing to the current sample, in (oversampled) samples
        double tau = (ad - x) / ad * dur + after;
        if (tau >= 1.0) tau = 0.999999;
        const double J = dir * unit->bpJump[k];
        const double K = dir * unit->bpSlope[k] * (d / std::max(dur, 1e-9));   // slope change per sample
        const double u = 1.0 - tau;
        yCur += J * (-0.5 * u * u) + K * (u * u * u / 6.0);
        yPrev += J * (0.5 * tau * tau) + K * (tau * tau * tau / 6.0);
    }
}

void MorphOsc_next(MorphOsc* unit, int inNumSamples) {
    float* out = OUT(0);
    const double sr2 = SAMPLERATE * OVERSAMPLE;   // (the oversampled rate)
    // the shape: control rate (its first sample when audio rate)
    ShapeParams s;
    s.skew = IN0(kSkew);
    s.round = clampd(IN0(kRound), 0, 1);
    s.pw = IN0(kPw);
    s.centered = IN0(kCentered) > 0.5f;
    const double dt0 = IN0(kFreq) / sr2;
    if (!unit->primed || s.skew != unit->shape.skew || s.round != unit->shape.round || s.pw != unit->shape.pw ||
        s.centered != unit->shape.centered || std::fabs(std::fabs(dt0) * 0.5 - unit->h) > unit->h * 0.25) {
        unit->shape = s;
        unit->warp.set(s);
        MorphOsc_breakpoints(unit, dt0);
        unit->primed = true;
    }
    const float* taps = halfband().h;
    const float* taps1 = halfband1().h;

    for (int i = 0; i < inNumSamples; i++) {
        const double dt = inAt(unit, kFreq, i) / sr2;
        const float pmNow = inAt(unit, kPm, i);
        const double mdt = inAt(unit, kSync, i) / sr2;
        for (int k = 0; k < OVERSAMPLE; k++) {
            const double pm = unit->prevPm + (pmNow - unit->prevPm) * (k + 1) / (double)OVERSAMPLE;
            double yPrev = unit->prevY, yCur;
            bool synced = false;
            double syncT = 0;   // samples since the master's wrap
            if (mdt > 0) {
                unit->master += mdt;
                if (unit->master >= 1.0) {
                    unit->master -= std::floor(unit->master);
                    synced = true;
                    syncT = std::min(0.999999, unit->master / mdt);
                }
            }
            const double pOld = unit->phase;
            if (synced) {
                const double atWrap = pOld + dt * (1.0 - syncT);      // the slave's phase when reset
                const double phiWrap = atWrap + pm;
                const double phiStart = 0.0 + pm;
                unit->phase = frac(dt * syncT);
                const double phi = unit->phase + pm;
                yCur = morphValue(unit, phi);
                // before the reset: prevPhi -> phiWrap, ending syncT samples ago
                double d1 = phiWrap - unit->prevPhi;
                d1 -= std::round(d1);
                MorphOsc_cross(unit, unit->prevPhi, d1, 1.0 - syncT, syncT, yPrev, yCur);
                // the reset itself: a step and a corner
                const double hh = unit->h;
                const double vBefore = morphValue(unit, phiWrap), vAfter = morphValue(unit, phiStart);
                const double sBefore = (vBefore - morphValue(unit, phiWrap - hh)) / hh;
                const double sAfter = (morphValue(unit, phiStart + hh) - vAfter) / hh;
                const double J = vAfter - vBefore, K = (sAfter - sBefore) * dt;
                const double tau = syncT, u = 1.0 - tau;
                yCur += J * (-0.5 * u * u) + K * (u * u * u / 6.0);
                yPrev += J * (0.5 * tau * tau) + K * (tau * tau * tau / 6.0);
                // after it: phiStart -> phi
                MorphOsc_cross(unit, phiStart, phi - phiStart, syncT, 0.0, yPrev, yCur);
                unit->prevPhi = phi;
            } else {
                unit->phase = frac(pOld + dt);
                const double phi = unit->phase + pm;
                yCur = morphValue(unit, phi);
                double d = phi - unit->prevPhi;
                d -= std::round(d);
                MorphOsc_cross(unit, unit->prevPhi, d, 1.0, 0.0, yPrev, yCur);
                unit->prevPhi = phi;
            }
            // yPrev is final: into the 4x -> 2x decimator; every second one on to 2x -> 1x
            const int p1 = unit->hist1Pos;
            unit->hist1[p1] = unit->hist1[p1 + HB1_TAPS] = (float)yPrev;
            unit->hist1Pos = (p1 + 1) % HB1_TAPS;
            if (k & 1) {
                const float* x1 = unit->hist1 + unit->hist1Pos;
                float a1 = taps1[HB1_CENTER] * x1[HB1_CENTER];
                for (int j = 0; j < HB1_TAPS; j += 2) a1 += taps1[j] * x1[j];
                const int pos = unit->histPos;
                unit->hist[pos] = unit->hist[pos + HB_TAPS] = a1;
                unit->histPos = (pos + 1) % HB_TAPS;
            }
            unit->prevY = yCur;
        }
        unit->prevPm = pmNow;
        // decimate: the newest HB_TAPS values, oldest first from histPos
        const float* x = unit->hist + unit->histPos;
        float acc = taps[HB_CENTER] * x[HB_CENTER];
        for (int j = 0; j < HB_TAPS; j += 2) acc += taps[j] * x[j];   // odd offsets from the centre (the even ones are 0)
        out[i] = zapgremlins(acc);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// AnalogFilter
// ════════════════════════════════════════════════════════════════════════════

enum { fIn, fFreq, fRes, fMode, fType, fDrive, fSat, fFeedback };

struct AnalogFilter : public Unit {
    double s1, s2, s3, s4;      // ladder stage states / SVF integrators (stage 1: s1 s2, stage 2: s3 s4)
    double lastOut, fbLow;       // the output (feedback) and its DC block
    double lastFc, g;
};

static void AnalogFilter_next(AnalogFilter* unit, int inNumSamples);

static void AnalogFilter_Ctor(AnalogFilter* unit) {
    unit->s1 = unit->s2 = unit->s3 = unit->s4 = 0;
    unit->lastOut = 0;
    unit->fbLow = 0;
    unit->lastFc = -1;
    unit->g = 0;
    SETCALC(AnalogFilter_next);
    AnalogFilter_next(unit, 1);
    unit->s1 = unit->s2 = unit->s3 = unit->s4 = 0;
    unit->lastOut = 0;
}

static inline double softSat(double x, double amount) {
    return amount <= 0 ? x : x + (std::tanh(x) - x) * amount;
}

void AnalogFilter_next(AnalogFilter* unit, int inNumSamples) {
    float* out = OUT(0);
    const double sr = SAMPLERATE;
    const int mode = std::clamp((int)IN0(fMode), 0, 3);
    const int type = std::clamp((int)IN0(fType), 0, 3);
    // drive: the input gain into the filter's own nonlinearity (tanh on its
    // input / in its loop), the level made up after; sat: a saturator after the
    // filter (its harmonics stay); feedback: the output back into the input
    // through a clipper and a DC block
    const double drive = std::max(0.01f, IN0(fDrive));
    // half of the drive (in dB) pushes the filter's input, half a clipper after it (grit that stays)
    const double driveIn = std::sqrt(drive), drivePost = std::sqrt(drive);
    const double makeup = std::pow(driveIn, -0.6), postNorm = drive > 1.0001 ? 1.0 / (std::tanh(drivePost) * std::sqrt(drivePost)) : 1.0;
    const double sat = clampd(IN0(fSat), 0, 1);
    const double satK = 1.0 + 9.0 * sat, satBias = 0.2 * sat, satOff = std::tanh(satBias), satNorm = 1.0 / std::sqrt(satK);
    const double fbAmt = clampd(IN0(fFeedback), 0, 1);
    const double fbCoef = 1.0 - std::exp(-2.0 * M_PI * 120.0 / sr);   // the loop's DC block / low cut
    const double fbMakeup = 1.0 / (1.0 + 2.5 * fbAmt);
    double s1 = unit->s1, s2 = unit->s2, s3 = unit->s3, s4 = unit->s4, lastOut = unit->lastOut, fbLow = unit->fbLow;

    for (int i = 0; i < inNumSamples; i++) {
        const double fc = clampd(inAt(unit, fFreq, i), 10.0, sr * 0.45);
        if (fc != unit->lastFc) { unit->lastFc = fc; unit->g = std::tan(M_PI * fc / sr); }
        const double g = unit->g;
        const double res = clampd(inAt(unit, fRes, i), 0, 1);
        double x = inAt(unit, fIn, i);
        x *= driveIn;
        if (fbAmt > 0) {
            const double f = std::tanh(2.0 * lastOut);
            fbLow += fbCoef * (f - fbLow);
            x += fbAmt * 2.0 * (f - fbLow);
        }
        double y = 0;

        if (mode <= 1) {
            // Cytomic / Simper SVF
            auto svf = [&](double v0, double k, double& ic1, double& ic2, double& lp, double& bp, double& hp) {
                const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
                const double v3 = v0 - ic2;
                const double v1 = a1 * ic1 + a2 * v3;
                const double v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = std::tanh(2 * v1 - ic1);
                ic2 = 2 * v2 - ic2;
                lp = v2; bp = v1; hp = v0 - k * v1 - v2;
            };
            auto pick = [&](double lp, double bp, double hp, double k) {
                switch (type) {
                    case 0: return lp;
                    case 1: return bp * k;   // unity at the peak
                    case 2: return hp;
                    default: return lp + hp;
                }
            };
            double lp, bp, hp;
            x = std::tanh(x);
            if (mode == 0) {
                const double k = std::max(0.005, 2.0 * (1.0 - res));
                svf(x, k, s1, s2, lp, bp, hp);
                y = pick(lp, bp, hp, std::max(k, 0.3));
            } else {
                // 4-pole: Butterworth pair, the resonance on the second stage
                const double k1 = 1.8478, k2 = std::max(0.005, 0.7654 * 2.0 * (1.0 - res) / 1.0);
                svf(x, k1, s1, s2, lp, bp, hp);
                const double first = pick(lp, bp, hp, k1);
                svf(first, k2, s3, s4, lp, bp, hp);
                y = pick(lp, bp, hp, std::max(k2, 0.3));
            }
        } else {
            // Zavalishin TPT ladder
            const double G = g / (1.0 + g), beta = 1.0 / (1.0 + g);
            // the resonance takes the lows down (LP gain 1 / (1 + k)): the LP is
            // made up after (all of it in the 2-pole, half in the 4-pole, Moog-like)
            double u, y1, y2, y3, y4, k;
            if (mode == 3) {
                k = res * 4.2;
                const double S = G * G * G * beta * s1 + G * G * beta * s2 + G * beta * s3 + beta * s4;
                u = (x - k * S) / (1.0 + k * G * G * G * G);
            } else {
                k = res * res * 24.0;
                const double S = G * beta * s1 + beta * s2;
                u = (x - k * S) / (1.0 + k * G * G);
            }
            u = std::tanh(u);
            auto stage = [&](double in, double& s) {
                const double v = (in - s) * G;
                const double o = v + s;
                s = o + v;
                return o;
            };
            y1 = stage(u, s1);
            y2 = stage(y1, s2);
            if (mode == 3) {
                y3 = stage(y2, s3);
                y4 = stage(y3, s4);
                const double hp4 = u - 4.0 * y1 + 6.0 * y2 - 4.0 * y3 + y4;
                switch (type) {
                    case 0: y = y4 * (1.0 + 0.5 * k); break;
                    case 1: y = 4.0 * (y2 - 2.0 * y3 + y4) / (1.0 + 0.6 * k); break;
                    case 2: y = hp4; break;
                    default: y = y4 - hp4; break;   // in phase at the cutoff: the difference notches
                }
            } else {
                switch (type) {
                    case 0: y = y2 * (1.0 + k); break;
                    case 1: y = 2.0 * (y1 - y2) * std::sqrt(1.0 + k * k * 0.25); break;   // unity at the cutoff
                    case 2: y = u - 2.0 * y1 + y2; break;
                    default: y = y2 + (u - 2.0 * y1 + y2); break;   // opposite at the cutoff: the sum notches
                }
            }
        }
        if (!std::isfinite(y)) { y = 0; s1 = s2 = s3 = s4 = 0; fbLow = 0; }
        lastOut = y;
        y *= makeup * fbMakeup;
        if (drive > 1.0001) y = std::tanh(y * drivePost) * postNorm;
        if (sat > 0) y = (std::tanh(y * satK + satBias) - satOff) * satNorm;
        out[i] = (float)y;
    }
    auto flush = [](double v) { return std::fabs(v) < 1e-30 ? 0.0 : v; };
    unit->s1 = flush(s1); unit->s2 = flush(s2);
    unit->s3 = flush(s3); unit->s4 = flush(s4);
    unit->lastOut = lastOut;
    unit->fbLow = flush(fbLow);
}

PluginLoad(OceanodeAnalog) {
    ft = inTable;
    DefineSimpleUnit(MorphOsc);
    DefineSimpleUnit(AnalogFilter);
}
