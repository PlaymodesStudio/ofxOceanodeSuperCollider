//
//  OceanodeTwanger.cpp — TwangerVoice, one note of the Reaktor ensemble t'wanGer
//  (twangerv4.ens, decoded with tools/ReaktorPatchExtractor; see tools/twanger/).
//
//  One unit is one note. It runs `unison` copies of the Reaktor voice (the ensemble
//  plays 16 voices with 2 per note) whose impulse pitches are spread by spreAd:
//
//    AR(gate x 4) -> Impulse(detuned pitch + spread)
//         -> filTer: hp4 hp1 bh4 bp4 bl4 lp1 lp4 off   (P = cuT + veL x velocity, reS)
//         -> reSonator 1: + delay x Lvl -> Saturator -> Multi 2-Pole LP -> out
//                         ^------------------- Single Delay <-------+
//              delay = Expon(A)(pitch) ms, Lvl = Expon(A)(pitch) x (-daMp + undaMp x (held - 1)) dB,
//              LP P = pitch + cuT, F = DR(velocity; deC, reL) x aMt Hz
//         -> reSonator 2 (oN): the string, tuned to the key itself (not detuned):
//              + 1-pole LP(delay) x Lvl -> Single Delay -> out
//              delay = Expon(A)(41.5 - note/2 - tuNe/2) ms,
//              Lvl = Expon(A)(-30 note/127) x daMp dB, LP P = hiCut + 80 note/127
//
//  "pitch" is the (de)Tuner output: oN ? traNs + scaLe x note : note, plus timBre.
//  Reaktor's Expon (A) is 10^(x/20): fed a pitch, reSonator 1's delay is clamped by
//  its 100 ms buffer and its loop is silent unless timBre/traNs bring the pitch low;
//  that is how the ensemble is wired, kept as it is.
//
//  Exact Primary arithmetic (from ugens/OceanodeSteamPipe, checked there against
//  Reaktor 6.5): Saturator, HP/LP 1-Pole (Euler, 0.99 cap), Multi 2-Pole (11x11
//  coefficient grid), ADSR time constants, linear "Good" delay reads.
//  Reconstructed (no reference in hand): the 4-pole multimode filters (a
//  zero-delay ladder with tapped outputs), Impulse (2-sample impulse, area 8/44100 s,
//  DC removed), the DR envelope's attack (instant), the unison offsets (+-spread/2).
//
//  TwangerVoice.ar(gate, pitch, att, rel, cut, res, vel, spread, filter,
//      r1damp, r1undamp, r1cut, r1res, r1amt, r1dec, r1rel,
//      r2hicut, r2tune, r2damp, r2on, timbre, scale, trans, dton, unison)
//  gate is a level (velocity); gate and pitch may be audio rate, the rest is read
//  once per block. Values are the Reaktor panel's own (dB, semitones, log times).
//

#include "SC_PlugIn.h"
#include "../OceanodeSteamPipe/PrimaryCompat.h"
#include <cmath>
#include <cstring>

static InterfaceTable* ft;

namespace {

enum { GATE, PITCH, ATT, REL, CUT, RES, VEL, SPREAD, FILTER,
       R1DAMP, R1UNDAMP, R1CUT, R1RES, R1AMT, R1DEC, R1REL,
       R2HICUT, R2TUNE, R2DAMP, R2ON, TIMBRE, SCALE, TRANS, DTON, UNISON, NUM_IN };

constexpr int kMaxUnison = 4;
constexpr double kPi = 3.14159265358979323846;

inline double clipd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline double exponA(double db) { return std::pow(10.0, clipd(db, -1500.0, 200.0) / 20.0); }
inline float reaktorSat(float x) { const float z = std::clamp(x, -4.f, 4.f); return z * (1.f - 0.125f * std::fabs(z)); }
// Euler one-pole coefficient of Primary HP/LP 1-Pole
inline float onePoleA(double pitch, double sr) {
    return std::min(0.99f, float(float(niPitch(pitch)) * float(6.2829999923706055 / sr)));
}
// Multi 2-Pole coefficient grid indexed by frequency (P and F already summed, in Hz)
inline void svfCoefs(double hz, double res, double sr, float& a, float& d) {
    float x = float(hz) * float(2. / sr) * 10.f;
    float y = float(clipd(res, 0, 1)) * 10.f;
    int ix = std::clamp(int(x), 0, 9), iy = std::clamp(int(y), 0, 9);
    float fx = std::clamp(x - float(ix), 0.f, 1.f), fy = std::clamp(y - float(iy), 0.f, 1.f);
    auto at = [&](const float* g) {
        const int i = iy * 11 + ix;
        float v = (1.f - fy) * (1.f - fx) * g[i];
        v = v + (1.f - fy) * fx * g[i + 1];
        v = v + fy * (1.f - fx) * g[i + 11];
        return v + fy * fx * g[i + 12];
    };
    a = at(primaryFilterF); d = at(primaryFilterD);
}
inline float readL(const float* buf, int size, int w, double delaySamples) {
    int i0 = w - int(std::floor(delaySamples)); if (i0 < 0) i0 += size;
    int i1 = i0 - 1; if (i1 < 0) i1 += size;
    const float frac = float(delaySamples - std::floor(delaySamples));
    const float y0 = buf[i0], y1 = buf[i1];
    return y0 + (y1 - y0) * frac;
}

// Primary envelopes: linear attack, one-pole decay / release (time constants).
struct Env {
    float env = 0, peak = 0;
    int stage = 0;      // 0 release, 1 decay/hold, 3 attack
};

struct Ladder {         // zero-delay 4-pole ladder with tapped multimode outputs
    double s[4] = {0, 0, 0, 0};
    void reset() { s[0] = s[1] = s[2] = s[3] = 0; }
};

struct SubVoice {
    float* d1; float* d2;
    int w1, w2;
    double phase;
    Env ar, dr;
    Ladder ladder;
    float lp1 = 0;            // HP/LP 1-Pole state of the filTer stage
    float svfBp = 0, svfLp = 0;
    float r2lp = 0;
    double carry = 0;         // impulse share for the next sample
};

} // namespace

struct TwangerVoice : public Unit {
    SubVoice sub[kMaxUnison];
    int n1, n2;               // delay sizes (samples)
    float* memory;
    float prevGate, vel, note;
    int pending;              // samples left in which a ramping gate may still raise the velocity
    bool held;
};

extern "C" {
void TwangerVoice_next(TwangerVoice* unit, int inNumSamples);
void TwangerVoice_Ctor(TwangerVoice* unit);
void TwangerVoice_Dtor(TwangerVoice* unit);
}

namespace {

void startEnvelopes(SubVoice& v, float vel) {
    // AR: peak = Gate (0..4) = 4 x velocity; DR: peak = velocity, instant attack.
    const float arPeak = 4.f * vel;
    v.ar.peak = v.ar.env > arPeak ? v.ar.env : arPeak;
    v.ar.stage = v.ar.env > arPeak ? 1 : 3;
    v.dr.peak = vel;
    v.dr.env = std::max(v.dr.env, vel);
    v.dr.stage = 1;
}

} // namespace

void TwangerVoice_next(TwangerVoice* unit, int inNumSamples) {
    float* out = OUT(0);
    if (!unit->memory) { Clear(inNumSamples, out); return; }
    const double sr = unit->mRate->mSampleRate;
    const bool gateAr = INRATE(GATE) == calc_FullRate;
    const bool pitchAr = INRATE(PITCH) == calc_FullRate;
    const float* gateIn = IN(GATE);
    const float* pitchIn = IN(PITCH);

    const int unison = std::clamp(int(IN0(UNISON) + 0.5f), 1, kMaxUnison);
    const double spread = clipd(IN0(SPREAD), 0, 24);
    const int filter = std::clamp(int(IN0(FILTER) + 0.5f), 0, 7);
    const double res = clipd(IN0(RES), 0, 0.995);
    const double cut = IN0(CUT), velAmt = IN0(VEL);
    const float attackRate = float(niTimeRate(clipd(IN0(ATT), -60, 120), sr));
    const float arRelease = std::max(0.f, float(1 - niTimeRate(clipd(IN0(REL), -60, 120), sr)));
    const float drDecay = std::max(0.f, float(1 - niTimeRate(clipd(IN0(R1DEC), -60, 120), sr)));
    const float drRelease = std::max(0.f, float(1 - niTimeRate(clipd(IN0(R1REL), -60, 120), sr)));
    const double r1damp = IN0(R1DAMP), r1undamp = IN0(R1UNDAMP), r1cut = IN0(R1CUT);
    const double r1res = clipd(IN0(R1RES), 0, 1), r1amt = IN0(R1AMT);
    const double r2hicut = IN0(R2HICUT), r2tune = IN0(R2TUNE), r2damp = IN0(R2DAMP);
    const bool r2on = IN0(R2ON) > 0.5f, dton = IN0(DTON) > 0.5f;
    const double timbre = IN0(TIMBRE), scale = IN0(SCALE), trans = IN0(TRANS);

    // offsets of the unison voices around zero, spreAd apart
    double offs[kMaxUnison];
    for (int u = 0; u < unison; ++u) offs[u] = (u - 0.5 * (unison - 1)) * spread;

    // what follows from the note, its velocity and the controls: once per block and at each gate event
    double d1 = 1, d2 = 1, G = 0, beta = 1, k = 4.0 * res, r1Hz0 = 0;
    float fb1 = 0, fb2 = 0, r2a = 0, fA = 0;
    double inc[kMaxUnison];
    const double h = 8.0 * sr / 44100.0;          // Impulse area 8/44100 s
    auto recompute = [&]() {
        const double note = unit->note, vel = unit->vel;
        // (de)Tuner output: the pitch of the impulse and of reSonator 1
        const double detP = (dton ? trans + scale * note : note) + timbre;
        const double e = exponA(detP);
        d1 = clipd(e * sr * 0.001, 1.0, unit->n1 - 3.0);
        // loop level in dB: Expon(A)(pitch) x (-daMp + undaMp x (Gate - 1)); Gate is 1 while held
        fb1 = float(exponA(e * (-r1damp + r1undamp * ((unit->held ? 1.0 : 0.0) - 1.0))));
        r1Hz0 = niPitch(detP + r1cut);
        // reSonator 2 follows the key itself (its own Note Pitch modules)
        d2 = clipd(exponA(41.5 - 0.5 * note - 0.5 * r2tune) * sr * 0.001, 1.0, unit->n2 - 3.0);
        fb2 = float(exponA(exponA(-30.0 * note / 127.0) * r2damp));
        r2a = onePoleA(r2hicut + 80.0 * note / 127.0, sr);
        // filTer: P = cuT + veL x velocity
        const double filtP = cut + velAmt * vel;
        fA = onePoleA(filtP, sr);
        const double gl = std::tan(kPi * std::min(niPitch(filtP), 0.45 * sr) / sr);
        G = gl / (1 + gl); beta = 1 / (1 + gl);
        for (int u = 0; u < unison; ++u) inc[u] = clipd(niPitch(detP + offs[u]) / sr, 0, 0.5);
    };
    recompute();

    for (int i = 0; i < inNumSamples; ++i) {
        const float g = gateAr ? gateIn[i] : gateIn[0];
        const float p = pitchAr ? pitchIn[i] : pitchIn[0];
        // gate events: a rise from zero starts a note; while the gate still ramps
        // up (an Oceanode value crosses a block) the velocity follows it
        bool changed = false;
        if (g > 0.f && unit->prevGate <= 0.f) {
            unit->held = true; unit->vel = std::min(g, 1.f); unit->note = p;
            unit->pending = unit->mWorld->mBufLength; changed = true;
            for (int u = 0; u < unison; ++u) startEnvelopes(unit->sub[u], unit->vel);
        } else if (g > 0.f && unit->pending > 0 && g > unit->vel) {
            unit->vel = std::min(g, 1.f); unit->note = p; changed = true;
            for (int u = 0; u < unison; ++u) startEnvelopes(unit->sub[u], unit->vel);
        } else if (g <= 0.f && unit->prevGate > 0.f) {
            unit->held = false; changed = true;
            for (int u = 0; u < unison; ++u) { unit->sub[u].ar.stage = 0; unit->sub[u].dr.stage = 0; }
        }
        if (unit->pending > 0) --unit->pending;
        unit->prevGate = g;
        if (changed) recompute();

        float mix = 0.f;
        for (int u = 0; u < unison; ++u) {
            SubVoice& v = unit->sub[u];
            // AR
            if (v.ar.stage == 3) { v.ar.env += attackRate * v.ar.peak; if (v.ar.env >= v.ar.peak) { v.ar.env = v.ar.peak; v.ar.stage = 1; } }
            else if (v.ar.stage == 0) v.ar.env = (v.ar.env + 1e-12f) * arRelease;
            // DR
            if (v.dr.stage == 1) v.dr.env = (v.dr.env + 1e-12f) * drDecay;
            else v.dr.env = (v.dr.env + 1e-12f) * drRelease;

            // Impulse: positive 2-sample impulses of area 8/44100 s, DC removed
            double imp = -h * inc[u];         // the mean of the impulse train
            v.phase += inc[u];
            imp += v.carry; v.carry = 0;      // the part of the last impulse that fell on this sample
            if (v.phase >= 1.0) {
                v.phase -= 1.0;
                const double frac = inc[u] > 0 ? v.phase / inc[u] : 0;   // samples since the crossing, 0..1
                imp += h * (1.0 - frac);
                v.carry = h * frac;
            }
            const float x = float(imp) * v.ar.env;

            // filTer
            float fsig;
            if (filter == 1 || filter == 5) {
                const float hp = x - v.lp1;              // HP before integrating LP (Primary)
                v.lp1 = v.lp1 + (fA * hp + 1e-12f);
                fsig = filter == 1 ? hp : v.lp1;
            } else if (filter == 7) {
                fsig = x;
            } else {
                Ladder& L = v.ladder;
                const double S = G * G * G * beta * L.s[0] + G * G * beta * L.s[1] + G * beta * L.s[2] + beta * L.s[3];
                const double y4e = (G * G * G * G * x + S) / (1 + k * G * G * G * G);
                const double u0 = x - k * y4e;
                double y[5]; y[0] = u0;
                double in = u0;
                for (int s = 0; s < 4; ++s) {
                    const double vv = (in - L.s[s]) * G;
                    const double ys = vv + L.s[s];
                    L.s[s] = ys + vv;
                    y[s + 1] = ys; in = ys;
                }
                switch (filter) {
                    case 0: fsig = float(y[0] - 4 * y[1] + 6 * y[2] - 4 * y[3] + y[4]); break;           // hp4
                    case 2: fsig = float(3.0792 * (y[1] - 3 * y[2] + 3 * y[3] - y[4])); break;           // bh4
                    case 3: fsig = float(4.0 * (y[2] - 2 * y[3] + y[4])); break;                       // bp4
                    case 4: fsig = float(3.0792 * (y[3] - y[4])); break;                               // bl4
                    default: fsig = float(y[4]); break;                                               // lp4
                }
                if (!std::isfinite(fsig)) { L.reset(); fsig = 0; }
            }

            // reSonator 1
            const float delayed1 = readL(v.d1, unit->n1, v.w1, d1);
            const float sat = reaktorSat(fsig + delayed1 * fb1);
            float a, dmp;
            svfCoefs(std::max(0.0, r1Hz0 + v.dr.env * r1amt), r1res, sr, a, dmp);
            const float hp2 = sat - (v.svfLp + v.svfBp * dmp);
            v.svfBp = v.svfBp + a * hp2;
            v.svfLp = v.svfLp + (a * v.svfBp + 1e-12f);
            const float r1 = v.svfLp;
            v.d1[v.w1] = r1;
            if (++v.w1 >= unit->n1) v.w1 = 0;

            // reSonator 2
            float r2 = r1;
            const float delayed2 = readL(v.d2, unit->n2, v.w2, d2);
            {
                const float lpIn = delayed2;
                v.r2lp = v.r2lp + (r2a * (lpIn - v.r2lp) + 1e-12f);
                v.d2[v.w2] = r1 + v.r2lp * fb2;
                if (++v.w2 >= unit->n2) v.w2 = 0;
                if (r2on) r2 = delayed2;
            }
            mix += r2;
        }
        out[i] = mix;
    }
    if (!(std::fabs(out[inNumSamples - 1]) < 1e4f)) {
        // something blew up (it should not): silence the unit's state
        for (int u = 0; u < kMaxUnison; ++u) {
            SubVoice& v = unit->sub[u];
            std::memset(v.d1, 0, unit->n1 * sizeof(float));
            std::memset(v.d2, 0, unit->n2 * sizeof(float));
            v.ladder.reset(); v.lp1 = v.svfBp = v.svfLp = v.r2lp = 0;
        }
        Clear(inNumSamples, out);
    }
}

void TwangerVoice_Ctor(TwangerVoice* unit) {
    const double sr = unit->mRate->mSampleRate;
    unit->n1 = int(0.1 * sr) + 8;     // Single Delay max 100 ms (reSonator 1)
    unit->n2 = int(1.0 * sr) + 8;     // Single Delay max 1000 ms (reSonator 2)
    const size_t perVoice = size_t(unit->n1 + unit->n2);
    unit->memory = (float*)RTAlloc(unit->mWorld, kMaxUnison * perVoice * sizeof(float));
    if (!unit->memory) {
        SETCALC(ft->fClearUnitOutputs);
        ClearUnitOutputs(unit, 1);
        if (unit->mWorld->mVerbosity > -2) Print("TwangerVoice: no real-time memory\n");
        return;
    }
    std::memset(unit->memory, 0, kMaxUnison * perVoice * sizeof(float));
    for (int u = 0; u < kMaxUnison; ++u) {
        SubVoice& v = unit->sub[u];
        new (&v) SubVoice();
        v.d1 = unit->memory + u * perVoice;
        v.d2 = v.d1 + unit->n1;
        v.w1 = v.w2 = 0;
        v.phase = 0;
    }
    unit->prevGate = 0; unit->vel = 0; unit->note = 60; unit->pending = 0; unit->held = false;
    SETCALC(TwangerVoice_next);
    ClearUnitOutputs(unit, 1);
}

void TwangerVoice_Dtor(TwangerVoice* unit) {
    if (unit->memory) RTFree(unit->mWorld, unit->memory);
}

PluginLoad(OceanodeTwanger) {
    ft = inTable;
    DefineDtorUnit(TwangerVoice);
}
