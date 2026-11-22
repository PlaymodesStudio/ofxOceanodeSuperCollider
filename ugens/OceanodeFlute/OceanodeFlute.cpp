//
//  OceanodeFlute.cpp — FluteVoice, the loop of FlutePro
//  (synthdefs/Source/PhysicalModelling/flutepro.scd).
//
//  Flute (flute.scd, the Csound slide flute) closes its loop with
//  LocalIn/LocalOut, which costs one block (64 samples). Its bore delay is the
//  period minus that block, so above about 700 Hz there is nothing left to
//  delay and the note stops rising. Its jet delay is half the bore plus a fixed
//  millisecond, so the jet / bore ratio, which decides the register, drifts with
//  the note. And its cubic x - x^3 has no bound: with high feedback it
//  overflows and the graph stays silent until rebuilt.
//
//  FluteVoice is the same loop computed sample by sample:
//
//    exc + fb1 * bore -> jet delay -> x - x^3 -+-> LPF -> v -> bore delay -> bore
//                                fb2 * bore ---+
//
//  - mode 0 (FlutePro): no block in the loop.
//    * The jet delay is Flute's (jet x period + a fixed 0.215 ms) up to C5, and
//      in proportion to the period above (where Flute no longer reaches).
//      Overblow divides it by 1 + overblow (the fixed part twice, so the jet lands
//      on half a period of the asked mode): 1 asks for the octave, 2 the twelfth,
//      3 two octaves; between whole values the mode of the two with the most loop
//      gain (with some hysteresis), as a real jet hesitates between registers.
//    * The tuning solves the linearised loop LPF . z^-bore . (fb2 + g fb1 z^-jet)
//      (g: the gain of x - x^3 at the jet's measured level and swing) so the mode
//      that sounds turns whole cycles exactly on its harmonic of the note. Which
//      mode sounds is heard (crossings of the output, low-passed above the asked
//      mode); for 0.12 s after a change of register the asked one is tuned
//      instead, which lets the new register take over; if the old one still
//      holds, the loop's level is halved and it tries again (twice at most).
//      Where the model misses (saturation, odd settings), the pitch heard trims
//      the bore slowly (up to 10 %) onto the note.
//    * The loop's LPF is at least track x the sounding mode's frequency, so high
//      notes still speak.
//    * The jet into the cubic is clipped at +-limit, the loop's output bends
//      softly above 4 x limit (both above where Flute is stable), and a loop that goes non-finite is cleared:
//      no feedback setting can break it.
//    Flute's breath-dependent shortening of the bore is kept, minus its slow
//    level (which only detuned).
//  - mode 1 (test): Flute's own arithmetic, block included (the bore is read
//    one block late, DelayC's 2-sample minimum, SC's LPF), to compare with it
//    sample by sample (tools/flutepro/null_test.py: about -100 dB).
//
//  FluteVoice.ar(exc, pitch, fb1, fb2, lpf, jet, jetoffs, overblow, track, limit, mode)
//    exc       the breath, audio: Flute's asum1 (noise x breath + pressure + vibrato)
//    pitch     MIDI note (audio or control rate)
//    fb1, fb2  Flute's ifeedbk1 (through the jet) and ifeedbk2 (around it)
//    lpf       the loop's low-pass cutoff in Hz
//    jet       jet delay / period (Flute: 0.5)
//    jetoffs   the jet's fixed part, ms; < 0: Flute's at this rate and block size (0.215 at 48 kHz, 64)
//    overblow  0 .. 6, see above
//    track     the cutoff is at least track x the sounding frequency (0: off)
//    limit     bound of the jet's swing into the cubic
//    mode      0 FlutePro, 1 test
//

#include "SC_PlugIn.h"
#include <cmath>
#include <cstring>

static InterfaceTable* ft;

namespace {

enum { EXC, PITCH, FB1, FB2, LPF, JET, JETOFFS, OVERBLOW, TRACK, LIMIT, MODE, NUM_IN };

inline double clipd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float cubicinterp(float x, float y0, float y1, float y2, float y3) {
    // SC's 4-point Hermite, as DelayC uses it
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + c0;
}

// DelayC's read, the input of this sample written at w: d >= 1
inline float readC(const float* b, long mask, long w, float d) {
    const long id = (long)d;
    const float frac = d - id;
    const long r1 = w - id;
    return cubicinterp(frac, b[(r1 + 1) & mask], b[r1 & mask], b[(r1 - 1) & mask], b[(r1 - 2) & mask]);
}

inline double kT72(double sr) { return sr / 523.2511306011972; }

inline float zap(float x) { const float a = std::fabs(x); return (a > 1e-15f && a < 1e15f) ? x : 0.f; }

// a soft ceiling: x below k unchanged, above it bends to 2k
inline float soft(float x, float k) {
    const float a = std::fabs(x);
    if (a <= k) return x;
    const float o = a - k;
    const float y = k + o / (1.f + o / k);
    return x < 0 ? -y : y;
}

struct Listen {
    float z, zPrev, prev, env, lp[4];
    bool high;
    long t, start;
    double first, last;     // crossing times, interpolated between samples
    int n, cand, nP;
    double sumP;
    double f0Start;         // the note when the window began (a window across a note change is not trimmed on)
};

struct FluteVoice : public Unit {
    float* bore; float* jet; float* fring;
    long nb, nj, nf, wb, wj, wf;
    double y1, y2;          // LPF state
    double a0, b1, b2, lpfFreq;
    double solved;          // the bore's offset from the period, for the sounding mode
    double mean;            // the breath's slow level
    double xm, xv;          // the jet's level and swing into the cubic, for the tuning
    int mode;               // the loop's mode that sounds (1 = the note, 2 its octave...)
    int asked, heard;       // the mode overblow asks for, the one heard (0: none)
    long insist;            // samples left during which the asked mode is tuned, whatever is heard
    int retries;            // pushes left if the new register does not take
    double trim;            // the bore's correction from the pitch heard, a fraction of the period
    Listen zc;
    double modeCut;
    int block;
};

struct Biquad { double a0, b1, b2; };

Biquad lpfOf(const FluteVoice* unit, double freq) {
    // SC's LPF (FilterUGens.cpp), same mix of float and double
    const double pfreq = freq * unit->mRate->mRadiansPerSample * 0.5;
    const double C = 1.f / std::tan(pfreq);
    const double C2 = C * C;
    const double sqrt2C = C * (float)M_SQRT2;
    Biquad q;
    q.a0 = 1.f / (1.f + sqrt2C + C2);
    q.b1 = -2.f * (1.f - C2) * q.a0;
    q.b2 = -(1.f - sqrt2C + C2) * q.a0;
    return q;
}

void lpfCoefs(FluteVoice* unit, double freq) {
    const Biquad q = lpfOf(unit, freq);
    unit->a0 = q.a0; unit->b1 = q.b1; unit->b2 = q.b2;
    unit->lpfFreq = freq;
}

// the LPF's gain, and its phase delay in samples, at w radians per sample
void lpfResp(const Biquad& q, double w, double& mag, double& lag) {
    // H = a0 (1 + 2z^-1 + z^-2) / (1 - b1 z^-1 - b2 z^-2)
    const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const double nr = 1 + 2 * c1 + c2, ni = -2 * s1 - s2;
    const double dr = 1 - q.b1 * c1 - q.b2 * c2, di = q.b1 * s1 + q.b2 * s2;
    mag = q.a0 * std::hypot(nr, ni) / std::max(1e-12, std::hypot(dr, di));
    double ph = std::atan2(ni, nr) - std::atan2(di, dr);   // negative: a lag
    while (ph > 0) ph -= 2 * M_PI;
    while (ph < -2 * M_PI) ph += 2 * M_PI;
    lag = -ph / w;
}

void FluteVoice_next(FluteVoice* unit, int inNumSamples) {
    float* out = OUT(0);
    const float* exc = IN(EXC);
    const float* pitchIn = IN(PITCH);
    const bool pitchAr = INRATE(PITCH) == calc_FullRate;
    const double sr = SAMPLERATE;
    const float fb1 = IN0(FB1), fb2 = IN0(FB2);
    const bool test = IN0(MODE) > 0.5;
    const long mb = unit->nb - 1, mj = unit->nj - 1, mf = unit->nf - 1;
    const float maxB = (float)(unit->nb - 8), maxJ = (float)(unit->nj - 8);

    if (test) {
        // Flute as it is: delays from asum1 per sample, the bore one block late
        const double B = unit->block, cr = sr / B;
        if (IN0(LPF) != unit->lpfFreq) lpfCoefs(unit, IN0(LPF));
        double y1 = unit->y1, y2 = unit->y2;
        const double a0 = unit->a0, b1 = unit->b1, b2 = unit->b2;
        for (int i = 0; i < inNumSamples; ++i) {
            const float p = pitchAr ? pitchIn[i] : pitchIn[0];
            const float f = sc_midicps(p);
            const float e = exc[i];
            const float afqc = 1.f / f - e / 20000.f - 9.f / (float)sr + f / 12000000.f - (float)(1.0 / cr);
            const float fd = unit->fring[(unit->wf - unit->block) & mf];             // LocalIn
            unit->jet[unit->wj] = e + fd * fb1;
            const float jd = clipd((afqc * 0.5f - e / f / (float)cr + 0.001f) * (float)sr, 2.f, maxJ);
            const float ax = readC(unit->jet, mj, unit->wj, jd);
            unit->wj = (unit->wj + 1) & mj;
            const float s3 = (ax - ax * ax * ax) + fd * fb2;
            const double y0 = s3 + b1 * y1 + b2 * y2;
            float v = a0 * (y0 + 2. * y1 + y2);
            y2 = y1; y1 = y0;
            if (!std::isfinite(v)) v = 0;                                    // Sanitize
            unit->bore[unit->wb] = v;
            const float bd = clipd(afqc * (float)sr, 2.f, maxB);
            float F = readC(unit->bore, mb, unit->wb, bd);
            unit->wb = (unit->wb + 1) & mb;
            if (!std::isfinite(F)) F = 0;                                    // Sanitize before LocalOut
            unit->fring[unit->wf] = F; unit->wf = (unit->wf + 1) & mf;
            out[i] = v;
        }
        unit->y1 = zap(y1); unit->y2 = zap(y2);
        return;
    }

    // FlutePro: no block in the loop
    const float jetR = clipd(IN0(JET), 0.01, 2.0);
    // the jet's fixed part, samples: Flute's 1 ms less half of (a block + 9 samples
    // + its breath term at a breath of 1), at this rate and block size; or as given
    const float jetOffs = IN0(JETOFFS) < 0 ? (float)(0.001 * sr - 0.5 * (unit->block + 9) - 0.5 * sr / 20000.0)
                                           : (float)(clipd(IN0(JETOFFS), 0, 5) * 0.001 * sr);
    const float perBlock = (float)(unit->block / sr);   // Flute's asum1 / ifqc / cr, per unit of period
    // overblow 1 halves the jet (octave), 2 makes it a third (twelfth), 3 a quarter (two octaves)
    const double ob = clipd(IN0(OVERBLOW), 0, 6);
    const float blow = 1.f / (1.f + (float)ob);
    const float meanA = 1 - std::exp(-2 * M_PI * 3 / sr);
    double mean = unit->mean;
    const double track = clipd(IN0(TRACK), 0, 20);
    const float limit = clipd(IN0(LIMIT), 0.2, 10);
    const float p0 = pitchAr ? pitchIn[inNumSamples - 1] : pitchIn[0];
    const double f0 = sc_midicps(clipd(p0, 0, 135));
    // the tuning (see the top): the linearised loop's modes sit near the harmonics
    // of the bore, the jet path pulls them flat; g is the cubic's gain at the jet's
    // level and swing (describing function of x - x^3)
    const double lpfIn = IN0(LPF);
    const double T0 = sr / f0;
    const double offs0 = jetOffs * std::min(1.0, T0 / kT72(sr));
    const double J0 = std::max(1.0, (jetR * T0 + offs0 * blow - unit->mean * T0 * perBlock) * blow);
    const double g = clipd(1 - 3 * unit->xm * unit->xm - 1.5 * unit->xv, -20, 1);
    auto solve = [&](int h, double& corr, double& cut) {
        const double w = std::min(2 * M_PI * h * f0 / sr, M_PI * 0.95);
        cut = clipd(std::max(lpfIn, track * h * f0), 10, sr * 0.45);
        double mag, lagS;
        lpfResp(lpfOf(unit, cut), w, mag, lagS);
        const double re = fb2 + g * fb1 * std::cos(w * J0), im = -g * fb1 * std::sin(w * J0);
        const double ph = (std::fabs(re) + std::fabs(im) > 1e-9) ? std::atan2(im, re) : 0;
        corr = clipd(T0 - lagS + ph / w, 2, maxB) - T0;     // the bore, as an offset from the period
        return mag * std::hypot(re, im);
    };
    // the mode overblow asks for (between two whole values, the one with more gain)
    const int hLo = 1 + (int)std::floor(ob), hHi = 1 + (int)std::ceil(ob);
    {
        double c, k;
        int want = hLo;
        if (hHi != hLo) {
            const double gLo = solve(hLo, c, k), gHi = solve(hHi, c, k);
            const int cur = unit->asked;
            if (cur == hLo || cur == hHi) want = ((cur == hLo ? gHi > gLo * 1.08 : gHi * 1.08 > gLo) ? hHi : hLo);
            else want = gHi > gLo ? hHi : hLo;
        }
        if (want != unit->asked && unit->asked > 0) {
            // a new register: tune the loop for it for a moment, whatever still sounds
            unit->insist = (long)(0.12 * sr);
            unit->heard = 0; unit->zc.cand = -2;
            unit->retries = 2;
        }
        unit->asked = want;
    }
    // the mode that sounds: what the loop is heard to play, until it is not heard
    const int h = (unit->insist <= 0 && unit->heard >= 1 && unit->heard <= 12) ? unit->heard : unit->asked;
    if (unit->insist > 0) unit->insist -= inNumSamples;
    if (h != unit->mode) unit->trim = 0;
    unit->mode = h;
    {
        double c, k;
        solve(h, c, k);
        unit->solved = c; unit->modeCut = k;
    }
    const double cut = unit->modeCut;
    if (cut != unit->lpfFreq) lpfCoefs(unit, cut);
    const float corr = unit->solved;
    const float trim = 1 + (float)unit->trim;
    const float xa = 1 - std::exp(-2 * M_PI * 2 / sr * inNumSamples);
    double xs = 0, xss = 0;
    double y1 = unit->y1, y2 = unit->y2;
    const double a0 = unit->a0, b1 = unit->b1, b2 = unit->b2;
    // Flute where it is stable: the jet swings up to about 1.8 and v to about 6
    // (fb2 0.7); limit's default is 2 and the soft ceiling 4 x limit
    const float ceilV = limit * 4.f;
    float peak = 0;
    Listen& zc = unit->zc;
    const long winLen = (long)std::max(0.015 * sr, 6 * T0);
    const float lpA = 1 - std::exp(-2 * M_PI * std::min(1.5 * unit->asked * f0, 0.4 * sr) / sr);

    for (int i = 0; i < inNumSamples; ++i) {
        const float p = pitchAr ? pitchIn[i] : pitchIn[0];
        const float f = sc_midicps(clipd(p, 0, 135));
        const float e = exc[i];
        const float period = (float)sr / f;
        // Flute's breath-dependent shortening, its jitter only (its level would detune)
        mean += meanA * (e - mean);
        const float bd = clipd(period * trim + corr - (e - (float)mean) * (float)(sr / 20000.0), 2.f, maxB);
        // read before this sample's v is written: d >= 2 touches only earlier samples
        const float fd = readC(unit->bore, mb, unit->wb, bd);
        unit->jet[unit->wj] = e + fd * fb1;
        // Flute's jet: half the period and its fixed part (0.215 ms at 48 kHz, 64-sample blocks),
        // the fixed part shrinking with the period above MIDI 72 (where Flute no longer reaches)
        const float offs = jetOffs * std::min(1.f, period / (float)kT72(sr));
        const float jd = clipd((jetR * period + offs * blow - e * period * perBlock) * blow, 1.f, maxJ);
        float ax = readC(unit->jet, mj, unit->wj, jd);
        unit->wj = (unit->wj + 1) & mj;
        ax = ax > limit ? limit : (ax < -limit ? -limit : ax);
        xs += ax; xss += ax * ax;
        const float s3 = (ax - ax * ax * ax) + fd * fb2;
        const double y0 = s3 + b1 * y1 + b2 * y2;
        float v = soft(a0 * (y0 + 2. * y1 + y2), ceilV);
        y2 = y1; y1 = y0;
        unit->bore[unit->wb] = v;
        unit->wb = (unit->wb + 1) & mb;
        out[i] = v;
        const float a = std::fabs(v);
        if (!(a <= peak)) peak = a;

        // listening: rising crossings of v, low-passed just above the mode overblow
        // asks for, so that a strong upper partial (of a low note) is not taken for
        // the mode (DC removed, Schmitt trigger)
        float l = v;
        for (int k = 0; k < 4; ++k) { zc.lp[k] += lpA * (l - zc.lp[k]); l = zc.lp[k]; }
        zc.zPrev = zc.z;
        zc.z = l - zc.prev + 0.995f * zc.z; zc.prev = l;
        zc.env += 0.002f * (std::fabs(zc.z) - zc.env);
        const float thr = 0.25f * zc.env + 1e-6f;
        if (!zc.high && zc.z > thr) {
            zc.high = true;
            const double tc = zc.t - (zc.z - thr) / std::max(1e-9f, zc.z - zc.zPrev);
            if (zc.n == 0) zc.first = tc;
            else {
                // single periods near the tuned mode's, for the trim (a missed or extra
                // crossing in a noisy breath would bias the count)
                const double per = tc - zc.last, want = T0 / std::max(1, unit->mode);
                if (std::fabs(per / want - 1) < 0.2) { zc.sumP += per; ++zc.nP; }
            }
            zc.last = tc; ++zc.n;
        } else if (zc.high && zc.z < -thr) zc.high = false;
        ++zc.t;
        if (zc.t - zc.start >= winLen) {
            // the harmonic of the note it plays; -1 unclear (keep what was heard), 0 silent
            int hm = zc.n < 2 ? 0 : -1;
            if (zc.n >= 4 && zc.last > zc.first) {
                const double r = (zc.n - 1) * sr / (zc.last - zc.first) / f0;
                const int k = (int)std::lround(r);
                if (k >= 1 && k <= 12 && std::fabs(r - k) < 0.25) {
                    hm = k;
                    // the trim: where the model misses (saturation, odd settings) the
                    // pitch heard pulls the bore slowly onto the note
                    if (k == unit->mode && unit->insist <= 0 && zc.nP >= 4 && std::fabs(f0 / zc.f0Start - 1) < 0.003) {
                        const double ratio = (T0 / k) / (zc.sumP / zc.nP);   // heard / wanted frequency
                        unit->trim = clipd(unit->trim + 0.25 * (ratio - 1), -0.1, 0.1);
                    }
                }
            }
            zc.n = 0; zc.nP = 0; zc.sumP = 0;
            if (hm >= 0 && hm == zc.cand && unit->insist <= 0) {
                unit->heard = hm;
                if (hm > 0 && hm != unit->asked && unit->retries > 0) {
                    // the old register held on: push again, the loop's level halved
                    --unit->retries;
                    unit->insist = (long)(0.12 * sr);
                    unit->heard = 0; hm = -2;
                    for (long k = 0; k < unit->nb; ++k) unit->bore[k] *= 0.5f;
                    for (long k = 0; k < unit->nj; ++k) unit->jet[k] *= 0.5f;
                }
            }
            zc.cand = hm;
            zc.start = zc.t; zc.f0Start = f0;
        }
    }
    if (!(peak < 1e6f) || !std::isfinite(y1) || !std::isfinite(y2)) {
        // not a number: start clean
        std::memset(unit->bore, 0, unit->nb * sizeof(float));
        std::memset(unit->jet, 0, unit->nj * sizeof(float));
        y1 = y2 = 0;
        Clear(inNumSamples, out);
    }
    unit->mean = std::isfinite(mean) ? mean : 0;
    {
        const double m = xs / inNumSamples, var = std::max(0.0, xss / inNumSamples - m * m);
        if (std::isfinite(m) && std::isfinite(var)) { unit->xm += xa * (m - unit->xm); unit->xv += xa * (var - unit->xv); }
    }
    unit->y1 = zap(y1); unit->y2 = zap(y2);
}

void FluteVoice_Ctor(FluteVoice* unit) {
    const double sr = SAMPLERATE;
    unit->block = unit->mWorld->mBufLength;
    // the longest period (MIDI 0) and a jet up to twice it
    long nb = 1, nj = 1;
    while (nb < 0.13 * sr + unit->block + 16) nb <<= 1;
    while (nj < 0.26 * sr + 16) nj <<= 1;
    long nf = 1;
    while (nf < unit->block + 4) nf <<= 1;
    unit->nb = nb; unit->nj = nj; unit->nf = nf;
    float* mem = (float*)RTAlloc(unit->mWorld, (nb + nj + nf) * sizeof(float));
    unit->bore = mem;
    if (!mem) {
        SETCALC(ft->fClearUnitOutputs);
        ClearUnitOutputs(unit, 1);
        if (unit->mWorld->mVerbosity > -2) Print("FluteVoice: no real-time memory\n");
        return;
    }
    std::memset(mem, 0, (nb + nj + nf) * sizeof(float));
    unit->jet = mem + nb; unit->fring = unit->jet + nj;
    unit->wb = unit->wj = unit->wf = 0;
    unit->y1 = unit->y2 = 0;
    unit->mean = IN0(EXC);
    unit->xm = IN0(EXC); unit->xv = 0;
    unit->asked = unit->heard = 0; unit->insist = 0; unit->retries = 0; unit->trim = 0;
    std::memset(&unit->zc, 0, sizeof(Listen));
    unit->mode = 0; unit->modeCut = std::max(10.f, IN0(LPF));
    unit->lpfFreq = -1; unit->solved = 0;
    lpfCoefs(unit, std::max(10.f, IN0(LPF)));
    SETCALC(FluteVoice_next);
    ClearUnitOutputs(unit, 1);
}

void FluteVoice_Dtor(FluteVoice* unit) {
    if (unit->bore) RTFree(unit->mWorld, unit->bore);
}

}

PluginLoad(OceanodeFlute) {
    ft = inTable;
    DefineDtorUnit(FluteVoice);
}
