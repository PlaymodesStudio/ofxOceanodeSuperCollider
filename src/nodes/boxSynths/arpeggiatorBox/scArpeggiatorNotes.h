//
//  scArpeggiatorNotes.h
//  ofxOceanodeSuperCollider
//
//  The ArpeggiatorBox engine as a note writer: chords drawn as notes in, the
//  arpeggio as notes out (the timeline piano roll's Arpeggiator tool,
//  ui/scPianoRollArpeggiator.h).
//
//  The chords are the engine's linked chord source: at every pulse its pool
//  is the notes sounding there, so the chords may change, overlap or leave
//  gaps (no pulses play in a gap). The engine counts in steps (Beat Div 1);
//  the step's length in beats is applied here, so every rate lands exactly.
//  Pulses count from the first chord's start.
//
//  No openFrameworks here (tools/arpeggiatorbox_test builds it alone).
//

#pragma once

#include "scArpeggiatorBoxEngine.h"

namespace arpbox {

struct RollNote {
    double start = 0.0, dur = 0.25;     // beats
    int pitch = 60;
    float vel = 0.8f;
};

enum RollOrder : int { ROLL_UP = 0, ROLL_DOWN, ROLL_UPDOWN, ROLL_DOWNUP, ROLL_RANDOM, ROLL_PLAYED };

struct RollSettings {
    // rhythm
    double stepBeats = 0.25;            // a step's length
    int pulseLen = 8, pulseHits = 8, pulseRotate = 0;   // Euclidean pulses (hits = length: every step)
    float stepChance = 1.0f;
    float swing = 0.0f;                 // steps
    // pattern
    int order = ROLL_UP;
    int octaves = 1;                    // the chord, then again this many octaves up
    bool restart = true;                // each chord starts the pattern again
    int cycle = 0;                      // steps the pattern (and its accents) start again after; 0: never
    // voices
    int polyphony = 1, interval = 1;    // notes per step, chord notes apart
    float noteChance = 1.0f;
    float strum = 0.0f;                 // steps between a step's notes
    float octaveChance = 0.0f; int octaveRange = 1;     // a note plays octaves up
    // notes
    float gate = 1.0f, gateRandom = 0.0f;               // steps
    float velocity = 0.8f, velocityRandom = 0.0f;
    int accentLen = 4, accentHits = 1, accentRotate = 0;
    float accentStrength = 0.0f;
    float humanize = 0.0f;              // steps a note may start late
    // bass: the chord's lowest note, octaves down
    bool bass = false;
    int bassEvery = 4, bassOctave = -1;
    float bassVelocity = 0.75f;
    int seed = 1;
    // the piano roll
    bool mono = false;
    int low = 0, high = 127;            // notes outside are left out
};

// One stretch of the chords with the same notes sounding
struct RollSegment {
    double start = 0.0, end = 0.0;      // steps from the first chord's start
    std::vector<float> pool;
    float root = 60.0f;
};

inline std::vector<RollSegment> rollSegments(const std::vector<RollNote>& chords, double origin, double stepBeats, bool sorted) {
    std::vector<double> edges;
    for(const auto& n : chords) { edges.push_back(n.start); edges.push_back(n.start + std::max(0.0, n.dur)); }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), edges.end());
    // (as played: by start, then pitch)
    std::vector<RollNote> ordered = chords;
    std::stable_sort(ordered.begin(), ordered.end(), [](const RollNote& a, const RollNote& b) {
        return a.start < b.start - 1e-9 || (std::abs(a.start - b.start) <= 1e-9 && a.pitch < b.pitch);
    });
    std::vector<RollSegment> out;
    for(size_t i = 0; i + 1 < edges.size(); i++) {
        const double mid = 0.5 * (edges[i] + edges[i + 1]);
        RollSegment seg;
        seg.start = (edges[i] - origin) / stepBeats;
        seg.end = (edges[i + 1] - origin) / stepBeats;
        std::vector<float> played;
        for(const auto& n : ordered) if(n.start <= mid && n.start + n.dur > mid) played.push_back((float)n.pitch);
        if(played.empty()) continue;
        seg.root = *std::min_element(played.begin(), played.end());
        seg.pool = buildSource(SRC_POOL, {}, played, sorted, true);
        out.push_back(seg);
    }
    return out;
}

// (outside: how many notes fell outside low..high and were left out)
inline std::vector<RollNote> arpeggiate(const std::vector<RollNote>& chords, const RollSettings& s, int* outside = nullptr) {
    std::vector<RollNote> out;
    if(outside) *outside = 0;
    if(chords.empty()) return out;
    const double stepBeats = std::max(1.0 / 64.0, s.stepBeats);
    double from = chords.front().start, to = from;
    for(const auto& n : chords) { from = std::min(from, n.start); to = std::max(to, n.start + n.dur); }
    const auto segments = rollSegments(chords, from, stepBeats, s.order != ROLL_PLAYED);
    if(segments.empty()) return out;
    const double steps = (to - from) / stepBeats;
    auto segmentAt = [&](double step) -> const RollSegment* {
        for(const auto& seg : segments) if(step >= seg.start - 1e-6 && step < seg.end - 1e-6) return &seg;
        return nullptr;
    };
    // How many steps a segment's pattern takes to come round
    const int octaves = std::clamp(s.octaves, 1, 8);
    const bool bounce = s.order == ROLL_UPDOWN || s.order == ROLL_DOWNUP;
    auto traversal = [&](const RollSegment& seg) {
        const int n = std::max(1, (int)seg.pool.size() * octaves);
        return std::clamp(bounce ? 2 * (n - 1) : n, 1, MAX_SEQ);
    };

    Engine e;
    Params& p = e.p;
    p.beatDiv = 1.0f;
    p.pulseMode = s.pulseHits >= s.pulseLen ? PULSE_PERIODIC : PULSE_EUCLID;
    p.eucLen = std::max(1, s.pulseLen); p.eucHits = std::clamp(s.pulseHits, 0, p.eucLen); p.eucOff = s.pulseRotate;
    p.stepChance = std::clamp(s.stepChance, 0.0f, 1.0f);
    p.swing = std::max(0.0f, s.swing);
    switch(s.order) {
        case ROLL_DOWN: p.patternMode = PAT_DESC; break;
        case ROLL_UPDOWN: p.patternMode = PAT_ASCDESC; break;
        case ROLL_DOWNUP: p.patternMode = PAT_DESCASC; break;
        case ROLL_RANDOM: p.patternMode = PAT_RANDOM; break;
        default: p.patternMode = PAT_ASC; break;
    }
    p.patternSeed = 0;                  // drawn again every cycle
    p.polyphony = std::clamp(s.polyphony, 1, MAX_POLY);
    p.polyInterval = std::max(1, s.interval);
    p.noteChance = std::clamp(s.noteChance, 0.0f, 1.0f);
    p.strum = std::max(0.0f, s.strum);
    p.octaveDev = std::clamp(s.octaveChance, 0.0f, 1.0f); p.octaveDevRng = std::max(1, s.octaveRange);
    p.durBase = std::clamp(s.gate, 0.01f, 16.0f); p.durRndm = std::max(0.0f, s.gateRandom);
    p.durStrength = 0.0f;
    p.velBase = std::clamp(s.velocity, 0.0f, 1.0f); p.velRndm = std::max(0.0f, s.velocityRandom);
    p.accMode = EV_EUCLID;
    p.accLen = std::max(1, s.accentLen); p.accHits = std::clamp(s.accentHits, 0, p.accLen); p.accOff = s.accentRotate;
    p.accStrength = s.accentStrength;
    p.positionRndm = std::max(0.0f, s.humanize);
    p.addBass = s.bass;
    p.bassMode = BASS_ALTERNATE; p.bassEvery = std::max(1, s.bassEvery); p.bassOctave = s.bassOctave;
    p.bassVel = std::clamp(s.bassVelocity, 0.0f, 1.0f);
    // The cycle: as set, else as long as fits whole turns of every chord's
    // pattern (so the pattern runs on over it) and, where they fit too, of
    // the pulses, the accents, the bass and the swing
    if(s.cycle > 0) p.seqSize = std::clamp(s.cycle, 1, MAX_SEQ);
    else {
        int64_t turn = 1;
        auto fold = [&](int64_t t) {
            int64_t a = turn, b = t;
            while(b != 0) { const int64_t r = a % b; a = b; b = r; }
            const int64_t both = turn / a * t;
            if(both > MAX_SEQ) return false;
            turn = both;
            return true;
        };
        bool fits = true;
        for(const auto& seg : segments) if(fits) fits = fold(traversal(seg));
        if(fits) {
            if(p.pulseMode == PULSE_EUCLID) fold(p.eucLen);
            if(s.swing > 0.0f) fold(2);
            if(s.accentStrength != 0.0f) fold(p.accLen);
            if(s.bass) fold(p.bassEvery);
        }
        p.seqSize = fits ? (int)(MAX_SEQ / turn * turn) : MAX_SEQ;
    }
    e.seed = (uint64_t)(uint32_t)s.seed + 1;
    e.fresh = H(e.seed, 0xF2E5);
    e.linkedSource = [&](double step) {
        const auto* seg = segmentAt(step);
        return seg ? seg->pool : std::vector<float>{60.0f};
    };
    e.linkedChordStart = [&](double step) {
        const auto* seg = segmentAt(step);
        return seg ? seg->start : 0.0;
    };
    e.linkedRoot = [&](double step) {
        const auto* seg = segmentAt(step);
        return seg ? seg->root : 60.0f;
    };
    e.linkedChange = s.restart ? 1 : 0;
    e.invalidate();

    for(const auto& pl : e.pulsesIn(0.0, steps - 1e-6)) {
        const auto* seg = segmentAt(pl.beat);
        if(!seg) continue;              // a gap between chords
        p.modulo = std::min(traversal(*seg), e.S());
        for(const auto& n : e.notesFor(pl)) {
            const int pitch = (int)std::lround(n.pitch);
            const double start = from + (pl.beat + n.offset) * stepBeats;
            if(start >= to - 1e-6) continue;
            if(pitch < s.low || pitch > s.high) { if(outside) ++*outside; continue; }
            RollNote r;
            r.start = start;
            r.dur = std::max(1.0 / 256.0, std::min(n.dur * stepBeats, to - start));
            r.pitch = pitch;
            r.vel = std::clamp(n.vel, 0.0f, 1.0f);
            out.push_back(r);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const RollNote& a, const RollNote& b) { return a.start < b.start; });
    if(s.mono) {
        // one note at a time: the first of each start, cut where the next begins
        std::vector<RollNote> line;
        for(const auto& n : out) {
            if(!line.empty() && n.start - line.back().start < 1e-6) continue;
            if(!line.empty()) line.back().dur = std::min(line.back().dur, n.start - line.back().start);
            line.push_back(n);
        }
        out = line;
    }
    return out;
}

} // namespace arpbox
