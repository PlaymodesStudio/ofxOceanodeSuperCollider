// ArpeggiatorBox engine tests: tools/arpeggiatorbox_test/run.sh
#include "../../src/nodes/scArpeggiatorBoxEngine.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace arpbox;
static int failures = 0;
#define CHECK(cond, ...) do { if(!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while(0)

static Engine make() {
    Engine e;
    e.p.octave = 4;
    e.resetSource(buildSource(SRC_SCALE, {0, 2, 4, 5, 7, 9, 11}, {}, true, false));
    e.invalidate();
    return e;
}
static std::vector<Note> window(Engine& e, double a, double b) {
    std::vector<Note> out;
    for(const auto& pl : e.pulsesIn(a, b)) for(const auto& n : e.notesFor(pl)) out.push_back(n);
    return out;
}
static bool same(const std::vector<Note>& a, const std::vector<Note>& b) {
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); i++)
        if(a[i].n != b[i].n || a[i].pitch != b[i].pitch || a[i].vel != b[i].vel || a[i].dur != b[i].dur || a[i].offset != b[i].offset) return false;
    return true;
}

int main() {
    {   // ascending scale, one note per beat
        Engine e = make();
        e.p.seqSize = 8;
        e.p.velRndm = 0;
        e.invalidate();
        auto ns = window(e, 0, 8);
        CHECK(ns.size() == 8, "8 notes, got %zu", ns.size());
        const float want[] = {48, 50, 52, 53, 55, 57, 59, 60};
        for(size_t i = 0; i < ns.size() && i < 8; i++) CHECK(ns[i].pitch == want[i], "step %zu pitch %.1f want %.1f", i, ns[i].pitch, want[i]);
        CHECK(ns[0].accent && !ns[1].accent && ns[4].accent, "euclid accents E(1,4)");
        CHECK(std::abs(ns[0].vel - 1.0f) < 1e-5 && std::abs(ns[1].vel - 0.8f) < 1e-5, "velocities %.3f %.3f", ns[0].vel, ns[1].vel);
    }
    {   // windows split anywhere give the same notes (chunks)
        Engine e = make();
        e.p.polyphony = 3; e.p.strum = 0.3f; e.p.swing = 0.4f; e.p.positionRndm = 0.2f; e.p.noteChance = 0.7f;
        e.p.octaveDev = 0.3f; e.p.idxDev = 0.3f; e.p.patternMode = PAT_RANDOM; e.p.beatDiv = 4; e.p.stepChance = 0.8f;
        e.p.addBass = true; e.p.bassMode = BASS_EUCLID; e.p.rndShiftChance = 0.5f; e.p.rndShiftRange = 4;
        e.invalidate();
        auto whole = window(e, 0, 32);
        auto a = window(e, 0, 13.37), b = window(e, 13.37, 32);
        a.insert(a.end(), b.begin(), b.end());
        CHECK(same(whole, a), "split windows differ (%zu vs %zu)", whole.size(), a.size());
        Engine f = make(); f.p = e.p; f.invalidate();
        auto again = window(f, 0, 32);
        CHECK(same(whole, again), "not reproducible");
        f.seed = 2; f.invalidate();
        CHECK(!same(whole, window(f, 0, 32)), "seed changes nothing");
    }
    {   // gate advance: pitch steps with played pulses only
        Engine e = make();
        e.p.pulseMode = PULSE_EUCLID; e.p.eucLen = 8; e.p.eucHits = 3; e.p.seqSize = 8; e.p.velRndm = 0;
        e.invalidate();
        auto ns = window(e, 0, 8);
        CHECK(ns.size() == 3, "E(3,8): %zu notes", ns.size());
        if(ns.size() == 3) CHECK(ns[0].pitch == 48 && ns[1].pitch == 50 && ns[2].pitch == 52, "gate advance %.0f %.0f %.0f", ns[0].pitch, ns[1].pitch, ns[2].pitch);
        e.p.gateAdvance = false; e.invalidate();
        ns = window(e, 0, 8);
        if(ns.size() == 3) CHECK(ns[1].pitch == 52 && ns[2].pitch == 57, "step advance: steps 2, 5 -> %.0f %.0f", ns[1].pitch, ns[2].pitch);
    }
    {   // bouncing ball: pulses, their ordinals and beats agree
        Engine e = make();
        e.p.pulseMode = PULSE_BOUNCE; e.p.bounceCurve = 0.7f; e.p.bounceSpeed = 0.5f; e.p.seqSize = 16; e.p.beatDiv = 2;
        e.invalidate();
        auto ps = e.pulsesIn(0, 16);
        CHECK(ps.size() == 16, "bounce: 0.5 x 16 per 8-beat cycle -> 16 in 16 beats, got %zu", ps.size());
        bool ok = true;
        for(size_t i = 1; i < ps.size(); i++) if(ps[i].n != ps[i - 1].n + 1 || ps[i].beat <= ps[i - 1].beat) ok = false;
        CHECK(ok, "bounce ordinals consecutive");
        for(const auto& pl : ps) CHECK(std::abs(e.pulseBeat(pl.n) - pl.beat) < 1e-9, "bounce pulseBeat(%lld) %.6f vs %.6f", (long long)pl.n, e.pulseBeat(pl.n), pl.beat);
    }
    {   // geiger
        Engine e = make();
        e.p.pulseMode = PULSE_GEIGER; e.p.geigerPeriodicity = 0.3f; e.p.geigerDensity = 0.6f;
        e.invalidate();
        auto ps = e.pulsesIn(10, 20);
        CHECK(!ps.empty(), "geiger pulses");
        for(const auto& pl : ps) CHECK(std::abs(e.pulseBeat(pl.n) - pl.beat) < 1e-9, "geiger pulseBeat(%lld)", (long long)pl.n);
        auto a = e.pulsesIn(10, 14.5), b = e.pulsesIn(14.5, 20);
        CHECK(a.size() + b.size() == ps.size() && (a.empty() || a.front().n == ps.front().n), "geiger split");
        std::printf("geiger: %zu pulses in 10 beats\n", ps.size());
    }
    {   // one shot plays one cycle after Reset
        Engine e = make();
        e.p.oneShot = true; e.p.seqSize = 4; e.invalidate();
        CHECK(window(e, 0, 8).empty(), "one shot unarmed");
        e.armed = true; e.origin = 2; e.invalidate();
        auto ns = window(e, 0, 12);
        CHECK(ns.size() == 4 && ns[0].n == 2, "one shot: %zu notes", ns.size());
    }
    {   // seq% mutes about the right share; run gate
        Engine e = make();
        e.p.seqProb = 0.5f; e.p.seqSize = 4; e.invalidate();
        auto ns = window(e, 0, 4000);
        CHECK(ns.size() > 1600 && ns.size() < 2400, "seq%% 0.5: %zu of 4000", ns.size());
        e.p.seqProb = 1; e.p.runBeats = 4; e.p.runChance = 0.25f; e.invalidate();
        ns = window(e, 0, 4000);
        CHECK(ns.size() > 700 && ns.size() < 1300, "run 0.25: %zu of 4000", ns.size());
    }
    {   // trigger mode: ordinals are the counts
        Engine e = make();
        e.p.triggerMode = true; e.p.seqSize = 3; e.invalidate();
        auto a = e.notesFor({0, 0}), b = e.notesFor({4, 0});
        CHECK(a.size() == 1 && b.size() == 1 && a[0].pitch == 48 && b[0].pitch == 50, "trigger mode");
    }
    {   // source change history
        Engine e = make();
        e.p.seqSize = 3;
        e.resetSource(buildSource(SRC_POOL, {}, {60, 64, 67}, true, false));
        e.setSource(3.0, buildSource(SRC_POOL, {}, {62, 65, 69}, true, false));
        e.invalidate();
        auto ns = window(e, 0, 6);
        CHECK(ns.size() == 6 && ns[0].pitch == 108 && ns[3].pitch == 110, "pool history %.0f %.0f", ns.empty() ? 0.f : ns[0].pitch, ns.size() > 3 ? ns[3].pitch : 0.f);
    }
    {   // timing
        Engine e = make();
        e.p.polyphony = 3; e.p.strum = 0.25f; e.p.swing = 0.5f; e.invalidate();
        auto ns = window(e, 0, 2);
        CHECK(ns.size() == 6 && ns[1].offset == 0.25 && ns[2].offset == 0.5 && ns[3].offset == 0.25, "strum/swing offsets");
    }
    {   // a linked ChordBox: C major until beat 4, then D minor; the chord's root for the bass
        Engine e = make();
        e.p.seqSize = 3; e.p.octave = 0; e.p.velRndm = 0; e.p.beatDiv = 2;
        e.linkedSource = [](double b){ return b < 4.0 ? std::vector<float>{60, 64, 67} : std::vector<float>{62, 65, 69}; };
        e.linkedChordStart = [](double b){ return b < 4.0 ? 0.0 : 4.0; };
        e.invalidate();
        auto ns = window(e, 0, 6);
        // keep phase: beat 4 is pulse 8 -> step 8 % 3 = 2 -> the third note of Dm
        CHECK(ns.size() == 12 && ns[7].pitch == 64 && ns[8].pitch == 69, "linked keep phase: %.0f %.0f", ns.size() > 8 ? ns[7].pitch : 0.f, ns.size() > 8 ? ns[8].pitch : 0.f);
        e.linkedChange = 1; e.invalidate();
        ns = window(e, 0, 6);
        CHECK(ns.size() == 12 && ns[8].pitch == 62 && ns[9].pitch == 65, "linked reset pattern: %.0f %.0f", ns.size() > 9 ? ns[8].pitch : 0.f, ns.size() > 9 ? ns[9].pitch : 0.f);
        e.linkedChange = 2; e.invalidate();
        ns = window(e, 0, 6);
        // next cycle: the cycle running at beat 4 (pulses 6-8) keeps C; pulse 9 starts a cycle in Dm
        CHECK(ns.size() == 12 && ns[8].pitch == 67 && ns[9].pitch == 62, "linked next cycle: %.0f %.0f", ns.size() > 9 ? ns[8].pitch : 0.f, ns.size() > 9 ? ns[9].pitch : 0.f);
    }
    {   // through other nodes (ChordBox -> +12 -> the arpeggiator), a frame late: the map learns
        // what they make of each chord, so the chord change plays on its beat
        auto chordAt = [](double b){ return std::fmod(b, 8.0) < 4.0 ? std::vector<float>{60, 64, 67} : std::vector<float>{62, 65, 69}; };
        auto startAt = [](double b){ return std::floor(b / 4.0) * 4.0; };
        ChainMap chain;
        std::vector<float> input = {72, 76, 79}, last;      // what the vector operation gives
        double inputBeat = -1e300;
        const double frame = 1.0 / 30.0;                    // 120 bpm, 60 fps
        for(double now = 0.0; now < 2.0; now += frame){
            // the input shows the chord of one frame before
            auto c = chordAt(std::max(0.0, now - frame)); for(float& x : c) x += 12.0f; input = c;
            if(!ChainMap::same(input, last)){ last = input; inputBeat = now; }
            const double start = startAt(now);
            if(inputBeat < start - 1e-9 && now - start < 0.3) continue;
            chain.learn(chordAt(now), input);
        }
        std::vector<float> v;
        CHECK(chain.offset && chain.delta == 12.0f && chain.predict({62, 65, 69}, v) && v[0] == 74.0f, "chain: offset learned, an unseen chord predicted");
        // just after the change the input is still the old chord: nothing learned wrong
        const size_t before = chain.seen.size();
        { const double now = 4.0 + frame * 0.5; if(!(inputBeat < startAt(now) - 1e-9 && now - startAt(now) < 0.3)) chain.learn(chordAt(now), {72, 76, 79}); }
        CHECK(chain.seen.size() == before, "chain: a stale input right after the change is not learned");
        Engine e = make();
        e.p.seqSize = 3; e.p.octave = 0; e.p.velRndm = 0; e.p.beatDiv = 2;
        e.linkedSource = [&](double b){ std::vector<float> out; return chain.predict(chordAt(b), out) ? out : input; };
        e.linkedChordStart = startAt;
        e.invalidate();
        auto ns = window(e, 0, 6);
        CHECK(ns.size() == 12 && ns[7].pitch == 76 && ns[8].pitch == 81, "chain: the chord changes on its beat: %.0f %.0f", ns.size() > 8 ? ns[7].pitch : 0.f, ns.size() > 8 ? ns[8].pitch : 0.f);
        // not an offset (a different voicing per chord): the values seen per chord
        ChainMap m;
        m.learn({60, 64, 67}, {48, 64, 79});
        CHECK(!m.offset && m.predict({60, 64, 67}, v) && v[0] == 48.0f && !m.predict({62, 65, 69}, v), "chain: a learned voicing, nothing guessed");
        // the nodes in between changed: forget and learn again
        chain.learn({60, 64, 67}, {67, 71, 74});
        CHECK(chain.seen.size() == 1 && chain.delta == 7.0f, "chain: relearn after a change");
    }
    {   // Octave Fold without Fold Poly: the added notes sit their interval above the folded main note
        Engine e = make();
        e.resetSource(buildSource(SRC_SCALE, {72, 76, 79}, {}, true, true));
        e.p.octave = 4; e.p.octaveFold = 2; e.p.foldPoly = false; e.p.pitchExpand = true;
        e.p.polyphony = 3; e.p.polyInterval = 1; e.p.velRndm = 0; e.invalidate();
        bool ok = true;
        for(const auto& pl : e.pulsesIn(0, 8)){
            const auto ns = e.notesFor(pl);
            if(ns.size() != 3) { ok = false; break; }
            const float d1 = ns[1].pitch - ns[0].pitch, d2 = ns[2].pitch - ns[1].pitch;
            if(ns[0].pitch < 48 || ns[0].pitch >= 72 || d1 < 3 || d1 > 5 || d2 < 3 || d2 > 5) { ok = false; std::printf("  %.0f %.0f %.0f\n", ns[0].pitch, ns[1].pitch, ns[2].pitch); break; }
        }
        CHECK(ok, "poly voices relative to the folded main note");
    }
    {   // deviation seeds: a seed repeats per cycle; 0 (no seed) draws anew each cycle and each pass
        auto cyclePitches = [](Engine& e, int cyc){
            std::vector<float> v;
            const int s = e.p.seqSize;
            for(const auto& pl : e.pulsesIn((double)cyc * s / e.bd(), (double)(cyc + 1) * s / e.bd())) for(const auto& n : e.notesFor(pl)) v.push_back(n.pitch);
            return v;
        };
        Engine e = make();
        e.p.seqSize = 8; e.p.octaveDev = 0.5f; e.p.pitchDev = 0.5f; e.p.pitchDevRng = 5; e.p.devSeed = 42; e.invalidate();
        CHECK(cyclePitches(e, 0) == cyclePitches(e, 1), "dev seed: the same deviations every cycle");
        e.p.devCycle = true; e.invalidate();
        CHECK(cyclePitches(e, 0) != cyclePitches(e, 1), "dev seed each cycle: a new set per cycle");
        e.p.devSeed = 0; e.p.devCycle = false; e.invalidate();
        const auto a = cyclePitches(e, 0);
        CHECK(a != cyclePitches(e, 1), "dev seed 0: new draws per cycle");
        e.fresh ^= 0x1234567; e.invalidate();
        CHECK(a != cyclePitches(e, 0), "dev seed 0: new draws on a new pass");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
