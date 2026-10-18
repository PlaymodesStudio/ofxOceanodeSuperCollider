//
//  scArpeggiatorBoxEngine.h
//  ofxOceanodeSuperCollider
//
//  ArpeggiatorBox's note engine: every musical decision of the Polyphonic
//  Arpeggiator GUI (ofxSantiNodes), as a pure function of the pulse:
//
//    pulse n (its ordinal since the start) at beat b
//      -> step = (n - origin) * (1 + Skip) mod Seq Size, its cycle
//      -> played? (One Shot, Seq% mute blocks, run gate, pulse pattern, Step%)
//      -> its chord (pattern offset, polyphony, deviations, velocity, duration,
//         strum / swing / position offsets) and its bass notes
//
//  Nothing depends on what was generated before, so the node writes notes
//  ahead (the player plays them sample accurately), a seek or a loop replays
//  them, and the window shows exactly what will play.
//
//  The GUI node's random draws come in two kinds, kept apart here:
//  • its pattern streams (Pattern, Accent, Duration, Velocity, Strum,
//    Position, Bass octave, LFO seeds) were already seeded per step: the same
//    here; "each cycle" draws that stream's seed from the node Seed and the
//    cycle's index.
//  • the rest used a free running generator (Step% / Note%, deviations,
//    Seq%, run gate, Geiger, step-seq pulses, bass chance and velocity,
//    random durations, random shift): seeded here by the node Seed and the
//    pulse (seed 0: the node draws a fresh seed for each pass).
//
//  Differences from the GUI node (all deliberate):
//  • a cycle starts whenever the step wraps (the GUI: only when it lands on
//    step 0, which Skip can avoid for several cycles);
//  • Seq% mutes blocks of SeqCycles x Seq Size pulses aligned to the start
//    (the same muted share as the GUI's chained blocks);
//  • same-pitch notes merge within a pulse (the GUI also merged across
//    pulses, by its output slots; the player's voices handle overlaps).
//
//  No openFrameworks here (tools/arpeggiatorbox_test builds it alone).
//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace arpbox {

constexpr int MAX_SEQ = 128;
constexpr int MAX_POLY = 16;
constexpr int MAX_BASS = 4;
constexpr int CURVE_RES = 64;
constexpr int GEIGER_TPB = 96;             // the GUI's Geiger / bouncing ball micro ticks per beat

enum PulseMode : int { PULSE_PERIODIC = 0, PULSE_EUCLID, PULSE_GEIGER, PULSE_STEPSEQ, PULSE_BOUNCE };
enum EventMode : int { EV_ALTERNATE = 0, EV_EUCLID, EV_RANDOM_STEP, EV_RANDOM_NOTE, EV_STEPSEQ };
enum BassMode : int { BASS_ALTERNATE = 0, BASS_EUCLID, BASS_RANDOM, BASS_VEL_ACCENT, BASS_DUR_ACCENT, BASS_START };
enum LfoShape : int { LFO_SIN = 0, LFO_SAW, LFO_INVSAW, LFO_PULSE, LFO_SLOWRANDOM };
enum SourceMode : int { SRC_SCALE = 0, SRC_POOL };
enum PatternMode : int { PAT_ASC = 0, PAT_DESC, PAT_RANDOM, PAT_USER, PAT_ASCDESC, PAT_DESCASC };
enum StrumDir : int { STRUM_UP = 0, STRUM_DOWN, STRUM_RANDOM };

// ── Hashing ─────────────────────────────────────────────────────────────────
inline uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
inline uint64_t H(uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0) {
    uint64_t h = mix64(a ^ 0x41525042u);
    h = mix64(h ^ b);
    h = mix64(h ^ (c * 0x9e3779b97f4a7c15ull));
    h = mix64(h ^ (d * 0xc2b2ae3d27d4eb4full));
    return mix64(h ^ (e * 0x165667b19e3779f9ull));
}
inline double unit(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
inline uint64_t U64(int64_t v) { return (uint64_t)v; }
inline int64_t floorDiv(int64_t a, int64_t b) { int64_t q = a / b; if((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
inline int64_t ceilDiv(int64_t a, int64_t b) { return -floorDiv(-a, b); }
inline int64_t wrap64(int64_t v, int64_t n) { if(n <= 0) return 0; int64_t m = v % n; return m < 0 ? m + n : m; }
inline int wrapI(int64_t v, int n) { return (int)wrap64(v, n); }

// Smooth 1D value noise in 0..1 (the velocity LFO's SlowRandom)
inline float valueNoise(double x, uint64_t seed) {
    const double f = std::floor(x);
    const double t = x - f;
    const double s = t * t * (3.0 - 2.0 * t);
    const double a = unit(H(seed, U64((int64_t)f), 0x515)), b = unit(H(seed, U64((int64_t)f + 1), 0x515));
    return (float)(a + (b - a) * s);
}

// Salts (the streams' tags)
enum Tag : uint64_t {
    T_STEPCH = 11, T_NOTECH, T_DEV1, T_DEV2, T_DEV3, T_DEV4, T_DEV5, T_DEV6, T_MUTE, T_RUN, T_STEPSEQ,
    T_GEIGER, T_GEIGER_POS, T_GEIGER_STEP, T_BASS_RND, T_BASS_PROB, T_BASS_VEL, T_DUR_RND, T_RSHIFT1, T_RSHIFT2,
    T_PATTERN = 101, T_ACC_STEP, T_ACC_NOTE, T_DUR_STEP, T_DUR_NOTE, T_VEL, T_VELSTEP, T_STRUM, T_STRUMDIR,
    T_POS, T_BASSOCT1, T_BASSOCT2,
    // the streams' "each cycle" seeds
    S_PATTERN = 201, S_ACC, S_DUR, S_VEL, S_VELSTEP, S_STRUM, S_STRUMDIR, S_POS, S_BASSOCT
};

// ── Parameters (the GUI node's, with its defaults) ──────────────────────────
struct Params {
    // clock / pulses
    bool triggerMode = false;            // pulses are external triggers (no beats)
    float beatDiv = 1.0f;                // steps per beat
    int pulseMode = PULSE_PERIODIC;
    std::vector<int> pulseStepPattern = std::vector<int>(16, 100);
    float geigerSpeed = 1.0f, geigerDensity = 0.45f, geigerPeriodicity = 0.75f;
    float bounceSpeed = 1.0f, bounceCurve = 0.0f;
    bool bounceSymmetric = false;
    int eucLen = 8, eucHits = 8, eucOff = 0;
    bool oneShot = false;
    int skipSteps = 0;
    // pattern
    int seqSize = 16, patternMode = PAT_ASC, patternSeed = 0;
    bool patternRndCycle = false;
    std::vector<int> idxPattern = {0, 1, 2, 3};
    int modulo = 0, sourceStart = 0, sourceStride = 1;
    bool gateAdvance = true;
    int stepShift = 0;
    float rndShiftChance = 0.0f;
    int rndShiftRange = 0, rndShiftQuant = 1;
    // pitch
    int octave = 0, octaveFold = 0;
    bool foldPoly = true;
    float root = 0.0f;
    bool pitchExpand = true;
    int expandStep = 12, transpose = 0;
    float octaveDev = 0.0f; int octaveDevRng = 1;
    float idxDev = 0.0f;    int idxDevRng = 2;
    float pitchDev = 0.0f;  int pitchDevRng = 2;
    bool byGate = true;                  // accents / durations step with played pulses (else every pulse)
    // polyphony / bass
    int polyphony = 1, polyInterval = 2, polyAccent = 0;
    bool addBass = false;
    int bassMode = BASS_ALTERNATE;
    bool bassInvert = false;
    int bassEvery = 2, bassShift = 0, bassEucLen = 8, bassEucHits = 2, bassEucOff = 0, bassOctave = -2;
    float bassProb = 1.0f;
    int bassOctRnd = 0;
    float bassOctRndChance = 0.0f;
    int bassOctRndSeed = 0;
    bool bassOctRndCycle = false;
    int bassPoly = 1;
    float bassVel = 0.75f, bassVelRndm = 0.0f;
    bool bassPitchMod = true;
    // timing
    float strum = 0.0f, strumRndm = 0.0f;
    int strumRndSeed = 0; bool strumRndCycle = false;
    int strumDir = STRUM_UP, strumDirSeed = 0; bool strumDirCycle = false;
    float swing = 0.0f;
    float positionRndm = 0.0f; int positionRndSeed = 0; bool positionRndCycle = false;
    // chances
    float seqProb = 1.0f, seqProbCycles = 1.0f;
    float runBeats = 16.0f, runChance = 1.0f, runPhase = 0.0f;
    float stepChance = 1.0f, noteChance = 1.0f;
    // velocity
    float velBase = 0.8f, velRndm = 0.1f;
    int velRndSeed = 0; bool velRndCycle = false;
    float velStepRndm = 0.0f; int velStepRndSeed = 0; bool velStepRndCycle = false;
    std::vector<float> velocityCurve = std::vector<float>(CURVE_RES, 0.0f);
    float lfoDepth = 0.0f, lfoSpeed = 0.25f, lfoPhase = 0.0f, lfoWidth = 0.5f;
    int lfoSeed = 0, lfoShape = LFO_SLOWRANDOM;
    // accents
    int accMode = EV_EUCLID, accEvery = 2, accShift = 0;
    float accChance = 0.5f; int accSeed = 0; bool accRndCycle = false;
    std::vector<int> accSteps = std::vector<int>(16, 0);
    int accLen = 4, accHits = 1, accOff = 0;
    float accStrength = 0.2f;
    // durations
    float durBase = 1.0f, durRndm = 0.0f;
    bool durRndPerStep = true;
    int durMode = EV_EUCLID, durEvery = 2, durShift = 0;
    float durChance = 0.5f; int durSeed = 0; bool durRndCycle = false;
    std::vector<int> durSteps = std::vector<int>(16, 0);
    int durLen = 4, durHits = 4, durOff = 0;
    float durStrength = 0.0f;
};

struct Pulse { int64_t n = 0; double beat = 0.0; };

struct Note {
    int64_t n = 0;          // its pulse (absolute ordinal)
    double pulseBeat = 0.0; // the pulse's beat (trigger mode: n / Beat Div)
    double offset = 0.0;    // steps after the pulse (strum, swing, position)
    double dur = 1.0;       // steps
    float pitch = 60.0f, vel = 0.8f;
    bool accent = false, longNote = false, bass = false;
    int tone = 0;           // chord tone (bass: its bass voice)
    int chord = 1;          // the pulse's chord size (bass: bass voices)
    int step = 0;           // the sequence step
};

// Source values from the GUI node's inputs (Scale mode: the scale sorted and
// deduplicated; Chord Pool: the pool clamped, sorted and deduplicated as set)
inline std::vector<float> buildSource(int mode, const std::vector<float>& scale, const std::vector<float>& pool,
                                      bool sortPool, bool removeDuplicates) {
    auto dedup = [](std::vector<float>& v) {
        std::vector<float> out;
        for(float x : v) {
            bool found = false;
            for(float y : out) if(std::abs(x - y) < 0.001f) { found = true; break; }
            if(!found) out.push_back(x);
        }
        v = out;
    };
    std::vector<float> s;
    if(mode == SRC_SCALE) {
        if(scale.empty()) s.push_back(0.0f);
        else { s = scale; std::sort(s.begin(), s.end()); dedup(s); }
    } else {
        s = pool;
        for(float& v : s) v = std::clamp(v, 0.0f, 127.0f);
        if(sortPool) std::stable_sort(s.begin(), s.end());
        if(removeDuplicates) dedup(s);
        if(s.empty()) s.push_back(60.0f);
    }
    return s;
}

class Engine {
public:
    Params p;
    uint64_t seed = 1;          // the node Seed in effect
    int64_t origin = 0;         // the ordinal of step 0 (Reset)
    bool armed = false;         // One Shot: a Reset happened

    // ── Source history: position (beat, or ordinal in trigger mode) -> values
    void setSource(double from, const std::vector<float>& values) {
        sources.erase(sources.lower_bound(from), sources.end());
        sources[from] = values;
        while(sources.size() > 32) sources.erase(sources.begin());
        cycles.clear();
    }
    void resetSource(const std::vector<float>& values) { sources.clear(); sources[-1e300] = values; cycles.clear(); }
    const std::vector<float>& sourceAt(double pos) const {
        static const std::vector<float> fallback = {60.0f};
        if(sources.empty()) return fallback;
        auto it = sources.upper_bound(pos);
        if(it == sources.begin()) return it->second;
        return std::prev(it)->second;
    }
    size_t sourceCount() const { return sources.size(); }
    const std::vector<float>& latestSource() const { return sources.empty() ? sourceAt(0) : sources.rbegin()->second; }
    // Drop entries older than `pos` (keeping the one in effect there)
    void pruneSources(double pos) {
        auto it = sources.upper_bound(pos);
        if(it == sources.begin()) return;
        --it;
        if(it != sources.begin()) sources.erase(sources.begin(), it);
    }

    // Parameters changed: everything derived is rebuilt on demand
    void invalidate() {
        cycles.clear();
        eucGate = euclidPattern(p.eucLen, p.eucHits, p.eucOff);
        eucAcc = euclidPattern(p.accLen, p.accHits, p.accOff);
        eucDur = euclidPattern(p.durLen, p.durHits, p.durOff);
        eucBass = euclidPattern(p.bassEucLen, p.bassEucHits, p.bassEucOff);
        rebuildBounce();
        const uint64_t sig = H(seed, U64((int64_t)std::lround(p.geigerSpeed * 1e5)), U64((int64_t)std::lround(p.geigerDensity * 1e5)),
                               U64((int64_t)std::lround(p.geigerPeriodicity * 1e5)));
        if(sig != geigerSig) { geigerSig = sig; geigerCum.assign(1, 0); }
    }

    // ── Sequence geometry ───────────────────────────────────────────────────
    int S() const { return std::clamp(p.seqSize, 1, MAX_SEQ); }
    int adv() const { return 1 + std::max(0, p.skipSteps); }
    double bd() const { return std::max(0.001, (double)p.beatDiv); }
    double stepBeats() const { return 1.0 / bd(); }
    int stepOf(int64_t r) const { return wrapI(r * adv(), S()); }
    int64_t cycleOf(int64_t r) const { return floorDiv(r * adv(), S()); }
    int64_t cycleStart(int64_t c) const { return ceilDiv(c * S(), adv()); }
    bool offGrid() const {
        return !p.triggerMode && ((p.pulseMode == PULSE_GEIGER && p.geigerPeriodicity < 0.999f) || p.pulseMode == PULSE_BOUNCE);
    }
    // The longest a note can start after its pulse (steps)
    double maxOffsetSteps() const {
        const int poly = std::min(MAX_POLY, std::max(1, p.polyphony + p.polyAccent));
        const int voices = std::max(poly, std::max(1, p.bassPoly));
        return (voices - 1) * (p.strum + p.strumRndm) + 0.5 * p.swing + p.positionRndm + 0.01;
    }
    double maxDurSteps() const { return 16.0; }

    // ── Pulses ──────────────────────────────────────────────────────────────
    // Every pulse with its beat in [from, to) (clocked modes)
    std::vector<Pulse> pulsesIn(double from, double to) {
        std::vector<Pulse> out;
        if(!(to > from)) return out;
        if(!offGrid()) {
            const double b = bd();
            for(int64_t k = (int64_t)std::ceil(from * b - 1e-9); (double)k / b < to; k++) out.push_back({k, (double)k / b});
        } else if(p.pulseMode == PULSE_BOUNCE) {
            const int64_t m0 = (int64_t)std::floor(from * GEIGER_TPB), m1 = (int64_t)std::ceil(to * GEIGER_TPB);
            for(int64_t m = m0; m < m1; m++) {
                const int64_t cs = bounceIndex((double)m / GEIGER_TPB), ce = bounceIndex((double)(m + 1) / GEIGER_TPB);
                const int64_t cnt = ce - cs;
                for(int64_t i = 0; i < cnt; i++) {
                    const double beat = ((double)m + ((double)i + 0.5) / (double)cnt) / GEIGER_TPB;
                    if(beat >= from && beat < to) out.push_back({cs + i, beat});
                }
            }
        } else {
            const int64_t m0 = std::max<int64_t>(0, (int64_t)std::floor(from * GEIGER_TPB));
            const int64_t m1 = (int64_t)std::ceil(to * GEIGER_TPB);
            if(m0 >= m1) return out;
            int64_t n = geigerBefore(m0);
            for(int64_t m = m0; m < m1; m++) {
                if(!geigerAt(m)) continue;
                const double beat = geigerBeat(m);
                if(beat >= from && beat < to) out.push_back({n, beat});
                n++;
            }
        }
        return out;
    }
    // A pulse's beat
    double pulseBeat(int64_t n) {
        if(!offGrid()) return (double)n / bd();
        if(p.pulseMode == PULSE_BOUNCE) {
            // the micro tick holding pulse n: bounceIndex is monotonic
            int64_t lo = (int64_t)std::floor((double)n / std::max(0.125, bouncePerCycle()) * cycleBeats() * GEIGER_TPB) - 4 * GEIGER_TPB;
            int64_t hi = lo + 8 * GEIGER_TPB;
            while(bounceIndex((double)lo / GEIGER_TPB) > n) lo -= 8 * GEIGER_TPB;
            while(bounceIndex((double)hi / GEIGER_TPB) <= n) hi += 8 * GEIGER_TPB;
            while(hi - lo > 1) {   // largest m with bounceIndex(m) <= n
                const int64_t mid = lo + (hi - lo) / 2;
                if(bounceIndex((double)mid / GEIGER_TPB) <= n) lo = mid; else hi = mid;
            }
            const int64_t cs = bounceIndex((double)lo / GEIGER_TPB), ce = bounceIndex((double)(lo + 1) / GEIGER_TPB);
            const int64_t cnt = std::max<int64_t>(1, ce - cs);
            return ((double)lo + ((double)(n - cs) + 0.5) / (double)cnt) / GEIGER_TPB;
        }
        if(n < 0) return -1.0 - (double)(-n);   // no Geiger pulses before the start
        // the beat block holding it, then its micro tick
        int64_t b = 0;
        while(geigerBlock(b + 1) <= n) {
            if(b > 1000000) return 1e9;
            int64_t step = 1;
            while(geigerBlock(b + step * 2) <= n && step < 4096) step *= 2;
            b += step;
        }
        int64_t count = geigerBlock(b);
        for(int64_t m = b * GEIGER_TPB; m < (b + 1) * GEIGER_TPB; m++) {
            if(!geigerAt(m)) continue;
            if(count == n) return geigerBeat(m);
            count++;
        }
        return (double)(b + 1);
    }
    // The first pulse at or after a beat
    Pulse firstPulseFrom(double beat) {
        const double span = std::max(1.0, 64.0 * stepBeats());
        for(int k = 0; k < 64; k++) {
            auto ps = pulsesIn(beat + k * span, beat + (k + 1) * span);
            if(!ps.empty()) return ps.front();
        }
        const int64_t n = (int64_t)std::ceil(beat * bd() - 1e-9);
        return {n, (double)n / bd()};
    }
    // The beat the next cycle starts at (or the current one, if a pulse at `beat` starts it)
    double nextCycleBeat(double beat) {
        const Pulse pl = firstPulseFrom(beat);
        const int64_t r = pl.n - origin;
        const int64_t c = cycleOf(r);
        const int64_t rc = cycleStart(c);
        if(rc == r) return pl.beat;
        return pulseBeat(origin + cycleStart(c + 1));
    }
    int64_t nextCycleOrdinal(int64_t n) {
        const int64_t r = n - origin;
        const int64_t c = cycleOf(r);
        if(cycleStart(c) == r) return n;
        return origin + cycleStart(c + 1);
    }

    // ── A cycle's decisions ─────────────────────────────────────────────────
    struct Cycle {
        int64_t c = 0, r0 = 0;
        int shift = 0;                     // Step Shift + this cycle's random shift
        std::vector<char> played;          // per pulse of the cycle
        std::vector<int> gates;            // played pulses before it in the cycle
        std::vector<double> beats;
    };
    const Cycle& cycle(int64_t c) {
        auto it = cycles.find(c);
        if(it != cycles.end()) return it->second;
        if(cycles.size() > 512) cycles.clear();
        Cycle cy;
        cy.c = c;
        cy.r0 = cycleStart(c);
        const int64_t r1 = cycleStart(c + 1);
        cy.shift = p.stepShift + randomShift(c);
        int g = 0;
        for(int64_t r = cy.r0; r < r1; r++) {
            const double beat = pulseBeat(origin + r);
            const bool on = played(r, beat, cy.shift);
            cy.played.push_back(on ? 1 : 0);
            cy.gates.push_back(g);
            cy.beats.push_back(beat);
            if(on) g++;
        }
        return cycles.emplace(c, std::move(cy)).first->second;
    }
    int randomShift(int64_t c) const {
        const float chance = std::clamp(p.rndShiftChance, 0.0f, 1.0f);
        const int range = std::max(0, p.rndShiftRange), quant = std::max(1, p.rndShiftQuant);
        if(chance <= 0.0f || range <= 0) return 0;
        if(unit(H(seed, U64(c), T_RSHIFT1)) > chance) return 0;
        const int maxQ = range / quant;
        if(maxQ <= 0) return 0;
        const int k = -maxQ + std::min(2 * maxQ, (int)(unit(H(seed, U64(c), T_RSHIFT2)) * (2 * maxQ + 1)));
        return k * quant;
    }
    bool played(int64_t r, double beat, int shift) {
        const int s = S();
        if(p.oneShot && (!armed || r < 0 || r >= ceilDiv(s, adv()))) return false;
        if(p.seqProb < 1.0f) {
            const int64_t block = std::max<int64_t>(1, std::llround(std::max(0.25f, p.seqProbCycles) * s));
            if(unit(H(seed, U64(floorDiv(r, block)), T_MUTE)) < 1.0 - std::clamp(p.seqProb, 0.0f, 1.0f)) return false;
        }
        if(!runGateOpen(p.triggerMode ? (double)r / bd() : beat)) return false;
        if(!pulseActive(stepOf(r) - shift, r)) return false;
        if(p.stepChance < 1.0f && unit(H(seed, U64(r), T_STEPCH)) > p.stepChance) return false;
        return true;
    }
    bool runGateOpen(double beat) const {
        const double B = std::max(0.001, (double)p.runBeats);
        const int64_t w = (int64_t)std::floor((beat + p.runPhase * B) / B);
        if(p.runChance <= 0.0f) return false;
        if(p.runChance >= 1.0f) return true;
        return unit(H(seed, U64(w), T_RUN)) < p.runChance;
    }
    int64_t runWindow(double beat) const {
        const double B = std::max(0.001, (double)p.runBeats);
        return (int64_t)std::floor((beat + p.runPhase * B) / B);
    }
    // The pulse pattern (grid pulses; off grid pulses always play)
    bool pulseActive(int64_t shiftedStep, int64_t r) {
        if(offGrid()) return true;
        switch(p.pulseMode) {
            case PULSE_EUCLID: return eucGate.empty() || eucGate[wrapI(shiftedStep, (int)eucGate.size())];
            case PULSE_GEIGER: {
                if(p.geigerPeriodicity >= 0.999f) return geigerGridLocked(shiftedStep);
                return unit(H(seed, U64(r), T_GEIGER_STEP)) <= geigerProbability((double)shiftedStep / bd());
            }
            case PULSE_STEPSEQ: {
                const auto& pat = p.pulseStepPattern;
                if(pat.empty()) return true;
                const float prob = std::clamp(pat[wrapI(shiftedStep, (int)pat.size())] / 100.0f, 0.0f, 1.0f);
                if(prob >= 1.0f) return true;
                if(prob <= 0.0f) return false;
                return unit(H(seed, U64(r), T_STEPSEQ)) <= prob;
            }
            case PULSE_BOUNCE: {
                const double a = (double)shiftedStep / bd(), b = (double)(shiftedStep + 1) / bd();
                return bounceIndex(b) - bounceIndex(a) > 0;
            }
            default: return true;
        }
    }

    // ── Notes ───────────────────────────────────────────────────────────────
    std::vector<Note> notesFor(const Pulse& pl) {
        std::vector<Note> out;
        const int64_t r = pl.n - origin;
        const int64_t c = cycleOf(r);
        const Cycle& cy = cycle(c);
        const int64_t idx = r - cy.r0;
        if(idx < 0 || idx >= (int64_t)cy.played.size() || !cy.played[idx]) return out;
        const int g = cy.gates[idx];
        const int shift = cy.shift;
        const int s = S();
        const int step = stepOf(r);
        const double beat = p.triggerMode ? (double)pl.n / bd() : pl.beat;
        const auto& src = sourceAt(p.triggerMode ? (double)pl.n : pl.beat);

        const int64_t accentIndex = p.byGate ? g : r;
        const int64_t shiftedAcc = accentIndex - shift;
        const bool stepAccented = eventStepActive(true, shiftedAcc, c, shift);
        const int poly = std::min(std::max(1, p.polyphony) + (stepAccented ? std::max(0, p.polyAccent) : 0), MAX_POLY);
        const int interval = std::max(1, p.polyInterval);
        const int patOff = patternOffset(p.gateAdvance ? g : step, c, shift);
        const int baseRel = patOff * std::max(0, p.sourceStride);
        const int baseSrc = p.sourceStart + baseRel;
        const bool stepDurAccent = eventStepActive(false, shiftedAcc, c, shift);
        const float sharedDur = p.durRndm > 0.0f ? p.durRndm * (float)unit(H(seed, U64(r), 0, T_DUR_RND)) : 0.0f;
        const float stepVel = p.velStepRndm > 0.0f
            ? std::clamp(p.velStepRndm * (float)unit(H(streamSeed(p.velStepRndSeed, p.velStepRndCycle, S_VELSTEP, c), U64(wrapI(step, s)), 0, T_VELSTEP, salt(shift))), 0.0f, 1.0f)
            : 0.0f;
        const float lfo = lfoOffset(beat);
        const float curve = curveAt(step);

        double prevPitch = std::numeric_limits<double>::quiet_NaN();
        for(int v = 0; v < poly; v++) {
            if(p.noteChance < 1.0f && unit(H(seed, U64(r), U64(v), T_NOTECH)) > p.noteChance) continue;
            const int si = baseSrc + v * interval;
            const bool fold = v == 0 || p.foldPoly;
            const int attempts = std::max(1, (int)src.size());
            float pitch = 60.0f;
            for(int a = 0; a < attempts; a++) {
                const int ci = si + a;
                const float dev = deviation(src, ci, r, v * 64 + a);
                pitch = std::clamp(mapPitch(sourceValue(src, ci), fold) + dev, 0.0f, 127.0f);
                if(std::isnan(prevPitch) || std::abs(pitch - prevPitch) > 0.001) break;
            }
            prevPitch = pitch;
            Note nt;
            nt.n = pl.n;
            nt.pulseBeat = pl.beat;
            nt.step = step;
            nt.tone = v;
            nt.chord = poly;
            nt.pitch = pitch;
            nt.accent = eventNoteActive(true, shiftedAcc, v, stepAccented, c, shift);
            nt.longNote = eventNoteActive(false, shiftedAcc, v, stepDurAccent, c, shift);
            const float durOff = p.durRndPerStep ? sharedDur
                               : (p.durRndm > 0.0f ? p.durRndm * (float)unit(H(seed, U64(r), U64(v + 1), T_DUR_RND)) : 0.0f);
            nt.dur = noteDuration(nt.longNote, durOff);
            float vel = p.velBase;
            if(p.velRndm > 0.0f)
                vel += p.velRndm * (float)unit(H(streamSeed(p.velRndSeed, p.velRndCycle, S_VEL, c), U64(wrapI(step, s)), U64(v), T_VEL, salt(shift)));
            vel += stepVel + curve + lfo;
            if(nt.accent) vel += p.accStrength;
            nt.vel = std::clamp(vel, 0.0f, 1.0f);
            nt.offset = positionOffset(step, v, poly, c);
            // the same pitch twice in one pulse: one note, as long as both
            bool merged = false;
            for(auto& o : out) {
                if(std::abs(o.pitch - nt.pitch) > 0.001f) continue;
                const double start = std::min(o.offset, nt.offset), end = std::max(o.offset + o.dur, nt.offset + nt.dur);
                if(nt.offset < o.offset) o.vel = nt.vel;
                o.offset = start;
                o.dur = end - start;
                merged = true;
                break;
            }
            if(!merged) out.push_back(nt);
        }

        if(p.addBass && bassStepActive(step - shift, stepAccented, stepDurAccent, r)) {
            bool add = true;
            if(p.bassMode != BASS_RANDOM) add = p.bassProb >= 1.0f || unit(H(seed, U64(r), T_BASS_PROB)) <= p.bassProb;
            if(add) {
                const float bassDurOff = p.durRndPerStep ? sharedDur
                                       : (p.durRndm > 0.0f ? p.durRndm * (float)unit(H(seed, U64(r), 100, T_DUR_RND)) : 0.0f);
                const double dur = noteDuration(stepDurAccent, bassDurOff);
                const int voices = std::clamp(p.bassPoly, 1, MAX_BASS);
                const int octOff = bassOctaveOffset(step - shift, c, shift);
                for(int bv = 0; bv < voices; bv++) {
                    Note nt;
                    nt.n = pl.n;
                    nt.pulseBeat = pl.beat;
                    nt.step = step;
                    nt.bass = true;
                    nt.tone = bv;
                    nt.chord = voices;
                    nt.accent = stepAccented;
                    nt.longNote = stepDurAccent;
                    nt.dur = dur;
                    float vel = p.bassVel;
                    if(p.bassVelRndm > 0.0f) vel += p.bassVelRndm * (float)unit(H(seed, U64(r), U64(bv), T_BASS_VEL));
                    nt.vel = std::clamp(vel, 0.0f, 1.0f);
                    float bp = p.root + (float)p.transpose;
                    if(p.bassPitchMod) bp += (float)(p.octave * 12);
                    bp += (float)((p.bassOctave + octOff + bv) * 12);
                    nt.pitch = std::clamp(bp, 0.0f, 127.0f);
                    nt.offset = positionOffset(step, bv, voices, c);
                    out.push_back(nt);
                }
            }
        }
        return out;
    }

    // ── The cycle's lanes (EucGateOut / EucAccOut / EucDurOut, the window) ─
    // Per step of the cycle holding pulse ordinal n
    void lanes(int64_t n, std::vector<int>& gate, std::vector<int>& acc, std::vector<int>& dur) {
        const int s = S();
        gate.assign(s, 0); acc.assign(s, 0); dur.assign(s, 0);
        const int64_t r = n - origin;
        const int64_t c = cycleOf(r);
        const int shift = p.stepShift + randomShift(c);
        int g = 0;
        for(int i = 0; i < s; i++) {
            const bool on = pulseActive(i - shift, cycleStart(c) + i);
            gate[i] = on ? 1 : 0;
            const int64_t ai = (p.byGate ? g : i) - shift;
            acc[i] = laneActive(true, ai, c, shift) ? 1 : 0;
            dur[i] = laneActive(false, ai, c, shift) ? 1 : 0;
            if(on) g++;
        }
    }

    // ── Pieces (public for the window's pictures) ───────────────────────────
    float sourceValue(const std::vector<float>& src, int64_t index) const {
        if(src.empty()) return 60.0f;
        const int n = (int)src.size();
        const float v = src[wrapI(index, n)];
        if(!p.pitchExpand) return v;
        return v + (float)(floorDiv(index, n) * p.expandStep);
    }
    float mapPitch(float pitch, bool fold) const {
        float m = pitch + (float)(p.transpose + p.octave * 12);
        if(fold && p.octaveFold > 0) {
            const float base = (float)(p.octave * 12), range = (float)(p.octaveFold * 12);
            float local = std::fmod(m - base, range);
            if(local < 0.0f) local += range;
            m = base + local;
        }
        return std::clamp(m, 0.0f, 127.0f);
    }
    float curveAt(int step) const {
        const auto& cv = p.velocityCurve;
        if(cv.empty()) return 0.0f;
        const int s = S();
        const float pos = s <= 1 ? 0.0f : (float)wrapI(step, s) / (float)(s - 1);
        if(cv.size() == 1) return std::clamp(cv[0], 0.0f, 1.0f);
        const float x = std::clamp(pos, 0.0f, 1.0f) * (float)(cv.size() - 1);
        const int i = std::clamp((int)std::floor(x), 0, (int)cv.size() - 1);
        const int j = std::min(i + 1, (int)cv.size() - 1);
        return std::clamp(cv[i] + (cv[j] - cv[i]) * (x - (float)i), 0.0f, 1.0f);
    }
    float lfoWave(double beat) const {
        const double off = (double)p.lfoPhase + (double)p.lfoSeed * 0.00091;
        const double phase = beat * std::max(0.0, (double)p.lfoSpeed) + off;
        const double x = phase - std::floor(phase);
        switch(p.lfoShape) {
            case LFO_SIN: return (float)(0.5 + 0.5 * std::sin(2.0 * M_PI * x));
            case LFO_SAW: return (float)x;
            case LFO_INVSAW: return (float)(1.0 - x);
            case LFO_PULSE: return x < (double)p.lfoWidth ? 1.0f : 0.0f;
            default: return valueNoise(phase, H((uint64_t)(uint32_t)p.lfoSeed, 0x51f));
        }
    }
    float lfoOffset(double beat) const {
        if(p.lfoDepth <= 0.0f) return 0.0f;
        return std::clamp(lfoWave(beat) * p.lfoDepth, 0.0f, 1.0f);
    }
    double noteDuration(bool longNote, float offset) const {
        double u = p.durBase + (longNote ? p.durStrength : 0.0f) + offset;
        return std::clamp(u, 0.01, 16.0);
    }
    double geigerProbability(double beat) const {
        const float density = std::clamp(p.geigerDensity, 0.0f, 1.0f);
        const float speed = std::max(0.001f, p.geigerSpeed);
        const float periodicity = std::clamp(p.geigerPeriodicity, 0.0f, 1.0f);
        if(density <= 0.0f) return 0.0;
        const double speedPhase = std::abs(beat) * std::pow((double)speed, 1.18);
        const double cycleLen = lerp(2.4, 0.09, density);
        const double phase = std::fmod(speedPhase, cycleLen) / std::max(0.0001, cycleLen);
        float periodic = 1.0f - std::abs((float)phase * 2.0f - 1.0f);
        periodic = std::pow(std::clamp(periodic, 0.0f, 1.0f), (float)lerp(4.2, 0.75, density));
        const float diffuse = (float)lerp(0.82, 0.16, periodicity);
        const float structured = (float)lerp(0.12, 1.0, periodicity) * periodic;
        const float activity = density * (float)lerp(diffuse, structured + diffuse * 0.15f, periodicity);
        const float rate = (0.35f + 26.0f * density) * std::pow(speed, 1.22f) * std::max(0.06f, activity);
        return std::clamp(1.0 - std::exp(-rate / (double)GEIGER_TPB), 0.0, 1.0);
    }
    bool geigerGridLocked(int64_t step) const {
        const float density = std::clamp(p.geigerDensity, 0.0f, 1.0f);
        if(density <= 0.0f) return false;
        const double rateBeat = std::max(0.001, (double)density * std::pow((double)std::max(0.001f, p.geigerSpeed), 1.18));
        const double ratePerStep = rateBeat / bd();
        if(ratePerStep >= 1.0) return true;
        const int interval = std::max(1, (int)std::lround(1.0 / std::max(0.0001, ratePerStep)));
        return wrapI(step, interval) == 0;
    }
    // Bouncing ball: pulses per cycle, distributed by the density curve
    double bounceDensity(double phase) const {
        phase = std::clamp(phase, 0.0, 1.0);
        const double curve = std::clamp((double)p.bounceCurve, -1.0, 1.0);
        const double mag = std::abs(curve);
        if(mag < 0.0001) return 1.0;
        const double e = lerp(1.0, 4.5, mag);
        const double edge = std::pow(std::abs(phase * 2.0 - 1.0), e);
        const double center = std::pow(std::max(0.0, 1.0 - std::abs(phase * 2.0 - 1.0)), e);
        if(p.bounceSymmetric) return 0.08 + (curve >= 0.0 ? edge : center);
        return curve > 0.0 ? 0.08 + std::pow(phase, e) : 0.08 + std::pow(1.0 - phase, e);
    }
    double cycleBeats() const { return (double)S() / bd(); }
    double bouncePerCycle() const { return std::max(0.125, (double)p.bounceSpeed * S()); }
    // Pulses (fractional) since beat 0
    double bounceCount(double beat) const {
        const double len = std::max(0.0001, cycleBeats());
        const double pos = beat / len;
        double ci = std::floor(pos);
        double local = pos - ci;
        if(local < 0.0) { local += 1.0; ci -= 1.0; }
        const double x = local * BOUNCE_SAMPLES;
        const int i = std::clamp((int)std::floor(x), 0, BOUNCE_SAMPLES - 1);
        const double partial = bounceCum[i] + bounceDens[i] * (x - i);
        const double prog = bounceTotal > 0.0 ? partial / bounceTotal : local;
        return ci * bouncePerCycle() + prog * bouncePerCycle();
    }
    int64_t bounceIndex(double beat) const { return (int64_t)std::floor(bounceCount(beat) + 1e-9); }

    std::map<int64_t, Cycle> cycles;

private:
    std::map<double, std::vector<float>> sources;
    std::vector<char> eucGate, eucAcc, eucDur, eucBass;
    static constexpr int BOUNCE_SAMPLES = 48;
    double bounceDens[BOUNCE_SAMPLES] = {}, bounceCum[BOUNCE_SAMPLES + 1] = {}, bounceTotal = 0.0;
    uint64_t geigerSig = 0;
    std::vector<int64_t> geigerCum = {0};   // pulses before each beat block (from beat 0)

    static double lerp(double a, double b, double t) { return a + (b - a) * t; }
    static uint64_t salt(int shift) { return U64((int64_t)shift + 0x1000); }

    static std::vector<char> euclidPattern(int len, int hits, int off) {
        std::vector<char> pat(std::max(0, len), 0);
        if(len <= 0 || hits <= 0) return pat;
        hits = std::min(hits, len);
        for(int j = 0; j < hits; j++) pat[wrapI((int64_t)j * len / hits + off, len)] = 1;
        return pat;
    }
    void rebuildBounce() {
        bounceTotal = 0.0;
        bounceCum[0] = 0.0;
        for(int i = 0; i < BOUNCE_SAMPLES; i++) {
            bounceDens[i] = bounceDensity(((double)i + 0.5) / BOUNCE_SAMPLES);
            bounceTotal += bounceDens[i];
            bounceCum[i + 1] = bounceTotal;
        }
    }
    bool geigerAt(int64_t m) const {
        if(m < 0) return false;
        return unit(H(seed, U64(m), T_GEIGER)) <= geigerProbability((double)m / GEIGER_TPB);
    }
    double geigerBeat(int64_t m) const { return ((double)m + unit(H(seed, U64(m), T_GEIGER_POS))) / GEIGER_TPB; }
    int64_t geigerBlock(int64_t b) {   // pulses before beat b
        if(b <= 0) return 0;
        while((int64_t)geigerCum.size() <= b) {
            const int64_t k = (int64_t)geigerCum.size() - 1;
            int64_t count = 0;
            for(int64_t m = k * GEIGER_TPB; m < (k + 1) * GEIGER_TPB; m++) if(geigerAt(m)) count++;
            geigerCum.push_back(geigerCum.back() + count);
        }
        return geigerCum[b];
    }
    int64_t geigerBefore(int64_t m) {   // pulses before micro tick m
        if(m <= 0) return 0;
        const int64_t b = m / GEIGER_TPB;
        int64_t n = geigerBlock(b);
        for(int64_t k = b * GEIGER_TPB; k < m; k++) if(geigerAt(k)) n++;
        return n;
    }

    // A stream's seed for cycle c ("each cycle": drawn from the node Seed)
    uint64_t streamSeed(int base, bool eachCycle, uint64_t stream, int64_t c) const {
        if(eachCycle) return H(seed, stream, U64(c), 0xC1C1E);
        return H((uint64_t)(uint32_t)base, stream, 0x5EED);
    }
    double patternUnit(int base, bool eachCycle, uint64_t stream, int64_t c, int64_t index, int voice, uint64_t tag, int shift) const {
        return unit(H(streamSeed(base, eachCycle, stream, c), U64(index), U64(voice), tag, salt(shift) * 1000003u + (uint64_t)S()));
    }
    int traversalSize() const {
        const int s = S();
        return p.modulo <= 0 ? s : std::max(1, std::min(s, p.modulo));
    }
    int bidirectional(int64_t stepIndex, bool asc) const {
        const int size = traversalSize();
        if(size <= 1) return 0;
        const int w = wrapI(stepIndex, size), mid = size / 2;
        if(asc) return w <= mid ? w : size - w;
        return w <= mid ? mid - w : w - mid;
    }
    int patternOffset(int64_t progressionIndex, int64_t c, int shift) const {
        const int size = traversalSize();
        const int64_t sh = progressionIndex - shift;
        switch(p.patternMode) {
            case PAT_DESC: return size - 1 - wrapI(sh, size);
            case PAT_RANDOM: return std::min(size - 1, (int)(patternUnit(p.patternSeed, p.patternRndCycle, S_PATTERN, c, sh, 0, T_PATTERN, shift) * size));
            case PAT_USER: {
                const auto& pat = p.idxPattern;
                if(pat.empty()) return 0;
                return wrapI(pat[wrapI(sh, (int)pat.size())], size);
            }
            case PAT_ASCDESC: return bidirectional(sh, true);
            case PAT_DESCASC: return bidirectional(sh, false);
            default: return wrapI(sh, size);
        }
    }
    // accents (acc) or long notes (!acc) on a (shifted) index
    bool eventStepActive(bool acc, int64_t idx, int64_t c, int shift) const {
        const int mode = acc ? p.accMode : p.durMode;
        switch(mode) {
            case EV_ALTERNATE: {
                const int every = std::max(1, acc ? p.accEvery : p.durEvery);
                return wrapI(idx - (acc ? p.accShift : p.durShift), every) == 0;
            }
            case EV_EUCLID: {
                const auto& pat = acc ? eucAcc : eucDur;
                return !pat.empty() && pat[wrapI(idx, (int)pat.size())];
            }
            case EV_RANDOM_STEP: {
                const float chance = acc ? p.accChance : p.durChance;
                return chance > 0.0f && patternUnit(acc ? p.accSeed : p.durSeed, acc ? p.accRndCycle : p.durRndCycle,
                                                    acc ? S_ACC : S_DUR, c, idx, 0, acc ? T_ACC_STEP : T_DUR_STEP, shift) <= chance;
            }
            case EV_STEPSEQ: {
                const auto& pat = acc ? p.accSteps : p.durSteps;
                return !pat.empty() && pat[wrapI(idx, (int)pat.size())] > 0;
            }
            default: return false;
        }
    }
    bool eventNoteActive(bool acc, int64_t idx, int voice, bool stepActive, int64_t c, int shift) const {
        if((acc ? p.accMode : p.durMode) != EV_RANDOM_NOTE) return stepActive;
        const float chance = acc ? p.accChance : p.durChance;
        return chance > 0.0f && patternUnit(acc ? p.accSeed : p.durSeed, acc ? p.accRndCycle : p.durRndCycle,
                                            acc ? S_ACC : S_DUR, c, idx, voice, acc ? T_ACC_NOTE : T_DUR_NOTE, shift) <= chance;
    }
    bool laneActive(bool acc, int64_t idx, int64_t c, int shift) const {
        if((acc ? p.accMode : p.durMode) != EV_RANDOM_NOTE) return eventStepActive(acc, idx, c, shift);
        for(int v = 0; v < std::max(1, p.polyphony); v++) if(eventNoteActive(acc, idx, v, false, c, shift)) return true;
        return false;
    }
    bool bassStepActive(int64_t idx, bool accented, bool durAccented, int64_t r) const {
        switch(p.bassMode) {
            case BASS_ALTERNATE: return wrapI(idx - p.bassShift, std::max(1, p.bassEvery)) == 0;
            case BASS_EUCLID: return !eucBass.empty() && eucBass[wrapI(idx, (int)eucBass.size())];
            case BASS_RANDOM: return p.bassProb > 0.0f && unit(H(seed, U64(r), T_BASS_RND)) <= p.bassProb;
            case BASS_VEL_ACCENT: return p.bassInvert ? !accented : accented;
            case BASS_DUR_ACCENT: return p.bassInvert ? !durAccented : durAccented;
            case BASS_START: return wrapI(idx, S()) == 0;
            default: return false;
        }
    }
    int bassOctaveOffset(int64_t idx, int64_t c, int shift) const {
        const int range = std::max(0, p.bassOctRnd);
        if(range <= 0 || p.bassOctRndChance <= 0.0f) return 0;
        if(patternUnit(p.bassOctRndSeed, p.bassOctRndCycle, S_BASSOCT, c, idx, 0, T_BASSOCT1, shift) > p.bassOctRndChance) return 0;
        const int count = range * 2 + 1;
        const double u = patternUnit(p.bassOctRndSeed, p.bassOctRndCycle, S_BASSOCT, c, idx, 0, T_BASSOCT2, shift);
        return std::clamp((int)std::floor(u * count), 0, count - 1) - range;
    }
    float deviation(const std::vector<float>& src, int64_t si, int64_t r, int k) const {
        float dev = 0.0f;
        auto u = [&](uint64_t tag){ return unit(H(seed, U64(r), U64(k), tag)); };
        if(p.octaveDev > 0.0f && u(T_DEV1) < p.octaveDev)
            dev += 12.0f * (float)(1 + std::min(std::max(1, p.octaveDevRng) - 1, (int)(u(T_DEV2) * std::max(1, p.octaveDevRng))));
        if(p.idxDev > 0.0f && u(T_DEV3) < p.idxDev) {
            const int off = 1 + std::min(std::max(1, p.idxDevRng) - 1, (int)(u(T_DEV4) * std::max(1, p.idxDevRng)));
            dev += sourceValue(src, si + off) - sourceValue(src, si);
        }
        if(p.pitchDev > 0.0f && u(T_DEV5) < p.pitchDev)
            dev += (float)(1 + std::min(std::max(1, p.pitchDevRng) - 1, (int)(u(T_DEV6) * std::max(1, p.pitchDevRng))));
        return dev;
    }
    double positionOffset(int step, int voice, int total, int64_t c) const {
        const int s = S();
        double off = strumOffset(step, voice, total, c);
        if(p.swing > 0.0f && wrapI(step, 2) == 1) off += p.swing * 0.5;
        if(p.positionRndm > 0.0f)
            off += unit(H(streamSeed(p.positionRndSeed, p.positionRndCycle, S_POS, c), U64(wrapI(step, s)), U64(voice), T_POS)) * p.positionRndm;
        return std::max(0.0, off);
    }
    double strumOffset(int step, int voice, int total, int64_t c) const {
        if(total <= 1 || p.strum <= 0.0f) return 0.0;
        const int s = S();
        double base = p.strum;
        if(p.strumRndm > 0.0f) {
            const double u = unit(H(streamSeed(p.strumRndSeed, p.strumRndCycle, S_STRUM, c), U64(wrapI(step, s)), U64(voice), T_STRUM));
            base = std::max(0.0, base + (u * 2.0 - 1.0) * p.strumRndm);
        }
        if(p.strumDir == STRUM_UP) return voice * base;
        if(p.strumDir == STRUM_DOWN) return (total - 1 - voice) * base;
        const double u = unit(H(streamSeed(p.strumDirSeed, p.strumDirCycle, S_STRUMDIR, c), U64(wrapI(step, s)), U64(voice), T_STRUMDIR));
        return u * (total - 1) * base;
    }
};

} // namespace arpbox
