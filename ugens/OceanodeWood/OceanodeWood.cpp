//
//  OceanodeWood.cpp — WoodPipe, the struck wood / glass voice of MarimbaPro.
//
//  The part of SteamPipeBox's voice (synthdefs/Defaults/SteamPipeBox/steamPipeBox.eel2)
//  that its GlassWoodTemplate snapshot uses, with that snapshot's values as the
//  defaults and the same Primary arithmetic (PrimaryCompat.h):
//
//    noise burst -> 2-pole low-pass -+-> x (1 + push x out) -+
//                                                            v
//      out <- low-pass <- high-pass <- clip <- allpass <- delay <- (-1) x (burst + out x feedback)
//
//  The inverted loop with an allpass inside it rings on a bar's partials
//  (about 1 : 2.8 : 4.1 : 5.9), the uneven clip gives the knock.
//
//  What is different from the SteamPipe voice:
//  - it plays in tune: the delays are solved so that the loop's first resonance
//    is the note (the snapshot sounds about 13 semitones above the key played);
//  - two pipes take the strikes in turn, so the last note rings on at its own
//    pitch, and a pipe that has died away costs nothing;
//  - the gate is a level, taken once it has settled after its edge (an Oceanode
//    value ramps over a block), and it sets the loudness and the brightness;
//  - coefficients are worked out once per block, not per sample.
//
//  WoodPipe.ar(gate, pitch, hardness, strike, decay, cutp, diffusion, ratio,
//              push, symmetry, drive, vibrato, vibfreq,
//              velamp, velbright, keybright, keycut, keydecay, raw)
//  hardness, cutp and decay are what a middle C gets; keybright, keycut and
//  keydecay say how much they follow the note away from it (per semitone:
//  semitones of cutoff, and for the decay 1 = half the ring time an octave up).
//  velamp and velbright: how much the gate's level sets loudness and brightness.
//  gate and pitch are read per sample when they are audio rate (the pitch at
//  the strike); the rest once per block.
//  raw = 1 is for tests: pitch is then the key played on the snapshot and the
//  delays, filters and tracking are the snapshot's own (of the controls only
//  strike, diffusion, push, symmetry, drive and the vibrato are used).
//

#include "SC_PlugIn.h"
#include "../OceanodeSteamPipe/PrimaryCompat.h"
#include <cmath>
#include <cstring>

static InterfaceTable* ft;

namespace {

// GlassWoodTemplate (Instrument - Bank 1, slot 27)
constexpr double kTune = 45.82677125930786 - 0.02362203598022461;
constexpr double kTuneCorr = -1.3385826349258423;
constexpr double kApTune = 27.874016761779785 - 0.5590551197528839;
constexpr double kApCorr = -1.1811023950576782;
constexpr double kNoiseKey = 0.31496062874794006, kNoiseVel = 0.35433071851730347;
constexpr double kHpKey = 1.0866141319274902, kLpKey = 0.8031497001647949;
constexpr double kRt = -20.078741312026978, kRtKey = 0.8188976645469666;
constexpr double kLp = 111.49605989456177, kDiffusion = 0.41732287406921387;
constexpr double kPi = 3.14159265358979323846;

enum { GATE, PITCH, HARD, STRIKE, DECAY, CUTP, DIFF, RATIO, PUSH, SYM, DRIVE, VIB, VIBF,
       VELAMP, VELBRIGHT, KEYBRIGHT, KEYCUT, KEYDECAY, RAW, NUM_IN };

inline double clipd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

// phase lags, in radians, of the parts of the loop at w radians per sample
inline double lagAllpass(double theta, double g) { return theta - 2 * std::atan2(g * std::sin(theta), 1 + g * std::cos(theta)); }
inline double lagLp(double w, double a) { return std::atan2((1 - a) * std::sin(w), 1 - (1 - a) * std::cos(w)); }
inline double lagHp(double w, double a) { return lagLp(w, a) - (kPi - w) * 0.5; }

struct Pipe {
    float* main; float* ap;
    int mw, aw;
    bool active;
    int stage;                      // 0 off, 1 decay, 3 attack
    float env, nbp, nlp, hps, lps;
    float note, vel, level, gain;   // level: the gate's level, evened out along the keyboard
    // per block
    float na, nd, hpa, lpa, fb, d1, d2, d1Prev, d2Prev;
    // the solved tuning and what it was solved for
    double solved, kNote, kCutp, kG, kR;
    int quiet;
};

struct WoodPipe : public Unit {
    Pipe pipe[2];
    int n1, n2, last;
    double rng, vibPhase;
    float prevGate, maxGate;
    bool pending;
    double offs;                    // the semitones the snapshot sounds above the key played
};

double loopFreq(double d1, double d2, double g, double lpa, double hpa) {
    // the first resonance of the inverted loop: where its lag is half a turn
    double lo = 1e-6, hi = kPi / d1;
    for (int i = 0; i < 48; ++i) {
        const double w = 0.5 * (lo + hi);
        const double f = w * d1 + lagAllpass(w * d2, g) + lagLp(w, lpa) + lagHp(w, hpa) - kPi;
        if (f > 0) hi = w; else lo = w;
    }
    return 0.5 * (lo + hi);
}

double solveDelay(double w, double r, double g, double lpa, double hpa) {
    const double target = kPi - lagLp(w, lpa) - lagHp(w, hpa);
    double lo = 0, hi = kPi / w;
    for (int i = 0; i < 40; ++i) {
        const double t = 0.5 * (lo + hi);
        if (w * t + lagAllpass(w * r * t, g) > target) hi = t; else lo = t;
    }
    return 0.5 * (lo + hi);
}

inline double onePole(double pitch, double sr) { return std::min(0.99, double(float(niPitch(pitch) * float(6.2829999923706055 / sr)))); }

void coefficients(WoodPipe* unit, Pipe& p, double vib) {
    const double sr = unit->mRate->mSampleRate;
    const bool raw = IN0(RAW) > 0.5;
    const double k = raw ? p.note : p.note - unit->offs;
    const double g = clipd(IN0(DIFF), -0.98, 0.98);
    // the snapshot's tracking turns about its own middle C, ours about the one that sounds
    const double velBright = raw ? kNoiseVel : clipd(IN0(VELBRIGHT), 0, 1);
    double noisePitch = raw ? 120 + (k - 60) * kNoiseKey
                            : (40 + 100 * clipd(IN0(HARD), 0, 1)) + (p.note - 60) * clipd(IN0(KEYBRIGHT), -2, 2);
    noisePitch *= (1 - velBright) + velBright * p.vel;
    p.na = niFilterF(noisePitch, 0, sr);
    p.nd = niFilterD(noisePitch, 0, sr);
    p.hpa = onePole((k - 60) * kHpKey, sr);
    p.lpa = onePole(raw ? kLp + (k - 60) * kLpKey : clipd(IN0(CUTP), 20, 135) + (p.note - 60) * clipd(IN0(KEYCUT), -2, 2), sr);
    double d1, d2, fbDb;
    if (raw) {
        d1 = niDelay(k + kTune + vib, kTuneCorr, sr);
        d2 = niDelay(k + kApTune, kApCorr, sr) + 1;
        fbDb = -60.0 * (d1 * 1000.0 / sr) * niPitch(kRt + (k - 60) * kRtKey) / 1000.0;
    } else {
        const double r = clipd(IN0(RATIO), 0.1, 16);
        const double w = 2 * kPi * 440.0 * std::pow(2.0, (p.note - 69) / 12.0) / sr;
        if (p.kNote != p.note || p.kCutp != p.lpa || p.kG != g || p.kR != r) {
            p.solved = solveDelay(std::min(w, 3.0), r, g, p.lpa, p.hpa);
            p.kNote = p.note; p.kCutp = p.lpa; p.kG = g; p.kR = r;
        }
        d1 = p.solved * std::pow(2.0, -vib / 12.0);
        d2 = p.solved * r;
        // decay: the seconds the loop takes to lose 60 dB at middle C
        const double trip = kPi / (w * sr);
        fbDb = -60.0 * trip / (clipd(IN0(DECAY), 0.01, 60) * std::pow(2.0, -clipd(IN0(KEYDECAY), -2, 2) * (p.note - 60) / 12.0));
    }
    p.d1 = clipd(d1, raw ? 2 : 1, unit->n1 - 3);
    p.d2 = clipd(d2, raw ? 2 : 1, unit->n2 - 3);
    p.fb = std::pow(10.0, fbDb / 20.0);
}

inline float readL(const float* b, int mask, int w, float d) {
    const int whole = int(d);
    const float frac = d - whole;
    const float y0 = b[(w - whole) & mask], y1 = b[(w - whole - 1) & mask];
    return y0 + (y1 - y0) * frac;
}

void strike(WoodPipe* unit, float level, float pitch) {
    unit->last = 1 - unit->last;
    Pipe& p = unit->pipe[unit->last];
    p.note = clipd(pitch, 0, 127);
    p.vel = clipd(level, 0, 1);
    p.stage = 3;
    p.quiet = 0;
    const bool was = p.active;
    p.active = true;
    coefficients(unit, p, 0);
    // the loop itself is some 2.4 dB quieter per octave going down
    const double velAmp = clipd(IN0(VELAMP), 0, 1);
    p.level = ((1 - velAmp) + velAmp * p.vel) * (IN0(RAW) > 0.5 ? 1.0 : std::pow(10.0, clipd((60 - p.note) * 0.2, -8, 8) / 20.0));
    if (!was) { p.gain = p.level; }
    p.d1Prev = p.d1; p.d2Prev = p.d2;
}

void WoodPipe_next(WoodPipe* unit, int inNumSamples) {
    float* out = OUT(0);
    const float* gateIn = IN(GATE);
    const bool gateAr = INRATE(GATE) == calc_FullRate;
    const float* pitchIn = IN(PITCH);
    const bool pitchAr = INRATE(PITCH) == calc_FullRate;   // then the note is the pitch at the strike's own sample
    const double sr = unit->mRate->mSampleRate;
    const float attack = niTimeRate(-20, sr);
    const float decayMult = std::max(0.0, 1.0 - niTimeRate(20.0 * std::log10(clipd(IN0(STRIKE), 0.05, 500)), sr));
    const float push = clipd(IN0(PUSH), 0, 2);
    const float sym = clipd(IN0(SYM), 0, 1);
    const float hi = 1 + sym, lo = -std::max(0.0001f, 1 - sym);
    const float drive = clipd(IN0(DRIVE), 0.05, 8), undrive = 1 / drive;
    const double vibStep = 2 * kPi * clipd(IN0(VIBF), 0, 40) / sr;
    const double vibAmount = clipd(IN0(VIB), 0, 12);
    const int m1 = unit->n1 - 1, m2 = unit->n2 - 1;
    const float g = clipd(IN0(DIFF), -0.98, 0.98);
    const float slope = 1.f / inNumSamples;

    unit->vibPhase += vibStep * inNumSamples;
    if (unit->vibPhase >= 2 * kPi) unit->vibPhase -= 2 * kPi;
    const double vib = std::sin(unit->vibPhase) * vibAmount;
    float d1Step[2] = { 0, 0 }, d2Step[2] = { 0, 0 }, gainStep[2] = { 0, 0 };
    float peak[2] = { 0, 0 };
    for (int v = 0; v < 2; ++v) {
        Pipe& p = unit->pipe[v];
        if (!p.active) continue;
        coefficients(unit, p, vib);
        d1Step[v] = (p.d1 - p.d1Prev) * slope;
        d2Step[v] = (p.d2 - p.d2Prev) * slope;
        gainStep[v] = (p.level - p.gain) * slope;
    }

    double rng = unit->rng;
    for (int i = 0; i < inNumSamples; ++i) {
        // a note: the gate's level once it stops rising after its edge
        const float gate = gateAr ? gateIn[i] : gateIn[0];
        if (unit->pending) {
            if (gate > unit->maxGate) unit->maxGate = gate;
            else {
                unit->pending = false;
                strike(unit, unit->maxGate, pitchAr ? pitchIn[i] : pitchIn[0]);
                const int v = unit->last;
                d1Step[v] = d2Step[v] = 0;
                gainStep[v] = (unit->pipe[v].level - unit->pipe[v].gain) / (inNumSamples - i);
            }
        } else if (gate > 0 && unit->prevGate <= 0) {
            if (gateAr) { unit->pending = true; unit->maxGate = gate; }
            else { strike(unit, gate, pitchAr ? pitchIn[i] : pitchIn[0]); const int v = unit->last; d1Step[v] = d2Step[v] = 0; gainStep[v] = (unit->pipe[v].level - unit->pipe[v].gain) / (inNumSamples - i); }
        }
        unit->prevGate = gate;

        // Reaktor's noise: a 32-bit shift register, two levels at +/-0.5
        const double bit = std::fmod(std::floor(rng * 0.5), 2.0);
        const double fbBit = std::fmod(std::fmod(rng, 2.0) + std::fmod(std::floor(rng / 32.0), 2.0) + std::fmod(std::floor(rng / 8192.0), 2.0), 2.0);
        rng = std::floor(rng * 0.5) + fbBit * 2147483648.0;
        const float white = float(bit - 0.5);

        float sum = 0;
        for (int v = 0; v < 2; ++v) {
            Pipe& p = unit->pipe[v];
            if (!p.active) continue;
            if (p.stage == 3) { p.env += attack; if (p.env > 1) { p.env = 1; p.stage = 1; } }
            else if (p.stage == 1) { p.env = (p.env + 1e-12f) * decayMult; if (p.env < 1e-9f) { p.env = 0; p.stage = 0; } }
            const float in = white * p.env;
            const float nhp = in - (p.nlp + p.nbp * p.nd);
            p.nbp += p.na * nhp;
            p.nlp += p.na * p.nbp + 1e-12f;
            const float steam = p.nlp * drive;

            p.d1Prev += d1Step[v]; p.d2Prev += d2Step[v];
            const float delayed = readL(p.main, m1, p.mw, p.d1Prev) + 1e-12f;
            const float apDelayed = readL(p.ap, m2, p.aw, p.d2Prev);
            const float apWrite = delayed - g * (apDelayed + 1e-12f);
            float x = apDelayed + g * apWrite;
            p.ap[p.aw] = apWrite; p.aw = (p.aw + 1) & m2;
            x = x > hi ? hi : (x < lo ? lo : x);
            const float hp = x - p.hps;
            p.hps += p.hpa * hp + 1e-12f;
            p.lps += p.lpa * (hp - p.lps) + 1e-12f;
            const float y = p.lps;
            p.main[p.mw] = -(steam * (y * push + 1) + y * p.fb);
            p.mw = (p.mw + 1) & m1;
            p.gain += gainStep[v];
            sum += y * p.gain;
            const float a = std::fabs(y);
            if (a > peak[v]) peak[v] = a;
        }
        out[i] = sum * undrive;
    }
    unit->rng = rng;

    for (int v = 0; v < 2; ++v) {
        Pipe& p = unit->pipe[v];
        if (!p.active) continue;
        p.d1Prev = p.d1; p.d2Prev = p.d2; p.gain = p.level;
        if (!(peak[v] < 1e3f)) {             // blown up or not a number: start clean
            std::memset(p.main, 0, unit->n1 * sizeof(float)); std::memset(p.ap, 0, unit->n2 * sizeof(float));
            p.nbp = p.nlp = p.hps = p.lps = p.env = 0; p.stage = 0; p.active = false;
        } else if (p.stage == 0 && peak[v] < 1e-6f) {
            // died away: stop computing it once a whole loop has stayed silent
            p.quiet += inNumSamples;
            if (p.quiet > unit->n1 + unit->n2) {
                std::memset(p.main, 0, unit->n1 * sizeof(float)); std::memset(p.ap, 0, unit->n2 * sizeof(float));
                p.nbp = p.nlp = p.hps = p.lps = 0; p.active = false;
            }
        } else p.quiet = 0;
    }
    if (!(std::fabs(out[inNumSamples - 1]) < 1e3f)) Clear(inNumSamples, out);
}

void WoodPipe_Ctor(WoodPipe* unit) {
    const double sr = unit->mRate->mSampleRate;
    // (a C1 at the defaults needs 370 and 1200 samples at 48 kHz; longer ones are held at the end)
    int n1 = 1024, n2 = 2048;
    while (n1 < 1024 * sr / 48000.0) n1 *= 2;
    while (n2 < 2048 * sr / 48000.0) n2 *= 2;
    unit->n1 = n1; unit->n2 = n2;
    float* memory = (float*)RTAlloc(unit->mWorld, 2 * (n1 + n2) * sizeof(float));
    std::memset(unit->pipe, 0, sizeof(unit->pipe));
    if (!memory) {
        SETCALC(ft->fClearUnitOutputs);
        ClearUnitOutputs(unit, 1);
        if (unit->mWorld->mVerbosity > -2) Print("WoodPipe: no real-time memory\n");
        unit->pipe[0].main = nullptr;
        return;
    }
    std::memset(memory, 0, 2 * (n1 + n2) * sizeof(float));
    for (int v = 0; v < 2; ++v) {
        unit->pipe[v].main = memory + v * (n1 + n2);
        unit->pipe[v].ap = unit->pipe[v].main + n1;
        unit->pipe[v].kNote = -1;
    }
    unit->last = 1;
    RGen& random = *unit->mParent->mRGen;
    // (in the test mode: the SteamPipe voice's own seed, to compare sample by sample)
    unit->rng = IN0(RAW) > 0.5 ? 22222 : 1 + std::floor(random.frand() * 4294967000.0);
    unit->vibPhase = 0;
    unit->prevGate = 0; unit->maxGate = 0; unit->pending = false;

    // where the snapshot sounds when middle C is played
    const double lpa = onePole(kLp, sr), hpa = onePole(0, sr);
    const double d1 = niDelay(60 + kTune, kTuneCorr, sr), d2 = niDelay(60 + kApTune, kApCorr, sr) + 1;
    const double w = loopFreq(d1, d2, kDiffusion, lpa, hpa);
    const double hz = w * sr / (2 * kPi);
    unit->offs = 12 * std::log2(hz / 261.6255653);

    SETCALC(WoodPipe_next);
    ClearUnitOutputs(unit, 1);
}

void WoodPipe_Dtor(WoodPipe* unit) {
    if (unit->pipe[0].main) RTFree(unit->mWorld, unit->pipe[0].main);
}

}

PluginLoad(OceanodeWood) {
    ft = inTable;
    DefineDtorUnit(WoodPipe);
}
