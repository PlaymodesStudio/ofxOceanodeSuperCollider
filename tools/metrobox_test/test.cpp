// Tests for MetroBox's rhythm engine (src/nodes/scMetroBoxEngine.*).
// Build and run:  tools/metrobox_test/run.sh

#include "../../src/nodes/scMetroBoxEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>

using namespace metrobox;

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if(!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while(0)

static std::vector<int> ticksOf(const std::vector<Onset>& v) {
    std::vector<int> t;
    for(const auto& o : v) t.push_back(o.tick);
    return t;
}
static bool sameCycle(const std::vector<Onset>& a, const std::vector<Onset>& b) {
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); i++)
        if(a[i].tick != b[i].tick || a[i].dur != b[i].dur || std::abs(a[i].amp - b[i].amp) > 1e-6f) return false;
    return true;
}
static std::string str(const std::vector<int>& v) {
    std::string s;
    for(int t : v) s += std::to_string(t) + " ";
    return s;
}

static Layer grid(int division, float density) {
    Layer L;
    L.generator = GEN_GRID;
    L.division = division;
    L.density = density;
    return L;
}

// Invariants every cycle must hold
static void checkInvariants(const Engine& e, int lane, int64_t from, int64_t to, const char* what) {
    const int cT = e.layers[lane].cycleTicks();
    for(int64_t c = from; c < to; c++) {
        const auto v = e.realizeCycle(lane, c);
        const auto next = e.realizeCycle(lane, c + 1);
        for(size_t i = 0; i < v.size(); i++) {
            CHECK(v[i].tick >= 0 && v[i].tick < cT, "%s lane %d cycle %lld: tick %d out of the cycle", what, lane, (long long)c, v[i].tick);
            if(i > 0) CHECK(v[i].tick > v[i - 1].tick, "%s lane %d cycle %lld: ticks not strictly increasing", what, lane, (long long)c);
            CHECK(v[i].dur >= 1, "%s: duration < 1", what);
            CHECK(v[i].amp > 0.0f && v[i].amp <= 1.0f, "%s: amplitude %f out of range", what, v[i].amp);
            const int nextTick = i + 1 < v.size() ? v[i + 1].tick : (next.empty() ? cT * 2 : cT + next.front().tick);
            const int gap = nextTick - v[i].tick;
            CHECK(gap <= 1 || v[i].dur < gap, "%s lane %d cycle %lld tick %d: gate %d reaches the next onset (gap %d)",
                  what, lane, (long long)c, v[i].tick, v[i].dur, gap);
        }
    }
}

int main() {
    // ── Determinism ──
    {
        Engine e;
        e.layers[0] = grid(12, 0.5f);
        const auto a = e.realizeCycle(0, 5);
        e.changedAll();
        const auto b = e.realizeCycle(0, 5);
        CHECK(sameCycle(a, b), "the same cycle twice differs");
        e.layers[0].seed = 7; e.changedAll();
        bool anyDiff = false;
        for(int c = 0; c < 8; c++) {
            Engine f; f.layers[0] = grid(12, 0.5f);
            if(!sameCycle(f.realizeCycle(0, c), e.realizeCycle(0, c))) anyDiff = true;
        }
        CHECK(anyDiff, "another seed gives the same rhythm");
    }

    // ── Density is monotonic ──
    {
        Engine lo, hi;
        lo.layers[0] = grid(12, 0.3f);
        hi.layers[0] = grid(12, 0.7f);
        for(int c = 0; c < 16; c++) {
            const auto a = ticksOf(lo.realizeCycle(0, c)), b = ticksOf(hi.realizeCycle(0, c));
            CHECK(std::includes(b.begin(), b.end(), a.begin(), a.end()), "cycle %d: density 0.3 hits not all in density 0.7 (%s / %s)", c, str(a).c_str(), str(b).c_str());
        }
    }

    // ── Subdivision: long on strong ──
    {
        Engine e;
        Layer L; L.generator = GEN_SUBDIVISION; L.division = 12; L.complexity = 0.9f; L.longOnStrong = 1.0f;
        e.layers[0] = L;
        int splitSecondHalf = 0;
        for(int c = 0; c < 16; c++) {
            const auto t = ticksOf(e.realizeCycle(0, c));
            CHECK(!t.empty() && t.front() == 0, "cycle %d: subdivision does not start on 1", c);
            for(int x : t) CHECK(x == 0 || x >= 96, "cycle %d: the downbeat half (strength 1) was split: %s", c, str(t).c_str());
            if(t.size() > 2) splitSecondHalf++;
        }
        CHECK(splitSecondHalf > 0, "the weaker half never splits");
    }

    // ── Push across the barline ──
    {
        Engine e;
        Layer L = grid(48, 1.0f); L.push = 1.0f; L.crossBarline = 1.0f; L.dispUnit = 12;
        e.layers[0] = L;
        const auto t = ticksOf(e.realizeCycle(0, 3));
        CHECK(t == std::vector<int>({36, 84, 132, 180}), "pushed beats: %s", str(t).c_str());
        e.layers[0].crossBarline = 0.0f; e.changedAll();
        const auto u = ticksOf(e.realizeCycle(0, 3));
        CHECK(u == std::vector<int>({0, 36, 84, 132}), "barline closed: %s", str(u).c_str());
    }

    // ── Relations ──
    {
        Engine e; e.numLayers = 2;
        e.layers[0] = grid(12, 0.5f);
        for(int mode : {REL_FOLLOW, REL_AVOID, REL_UNISON, REL_COMPLEMENT}) {
            Layer L = grid(12, 0.5f); L.seed = 99; L.relMode = mode; L.relLane = 1; L.relAmount = 1.0f;
            e.layers[1] = L; e.changedAll();
            for(int c = 0; c < 8; c++) {
                const auto a = e.realizeCycle(0, c);
                const auto ta = ticksOf(a), tb = ticksOf(e.realizeCycle(1, c));
                std::vector<int> inter;
                std::set_intersection(ta.begin(), ta.end(), tb.begin(), tb.end(), std::back_inserter(inter));
                if(mode == REL_FOLLOW) CHECK(inter.size() == tb.size(), "follow: lane 2 plays where lane 1 does not");
                if(mode == REL_AVOID) CHECK(inter.empty(), "avoid: coincidences %s", str(inter).c_str());
                if(mode == REL_UNISON) CHECK(inter.size() == ta.size(), "unison: lane 1 hits missing in lane 2");
                if(mode == REL_COMPLEMENT)
                    for(int t : tb)
                        for(const auto& o : a)
                            CHECK(!(o.tick <= t && t < o.tick + o.dur), "complement: lane 2 hit %d inside a lane 1 gate", t);
            }
        }
        // A lane cannot relate to itself or a lane below
        Layer L = grid(12, 0.5f); L.relMode = REL_AVOID; L.relLane = 2;
        e.layers[1] = L; e.changedAll();
        Engine ref; ref.numLayers = 2; ref.layers[0] = grid(12, 0.5f); ref.layers[1] = grid(12, 0.5f);
        CHECK(sameCycle(e.realizeCycle(1, 2), ref.realizeCycle(1, 2)), "a relation to itself had an effect");
    }

    // ── Response: replays the lane above, delayed ──
    {
        Engine e; e.numLayers = 2;
        e.layers[0] = grid(24, 0.5f);
        Layer L = grid(12, 1.0f); L.relMode = REL_RESPONSE; L.relLane = 1; L.relAmount = 1.0f; L.respDelay = 96;
        e.layers[1] = L;
        for(int c = 1; c < 6; c++) {
            std::vector<int> expected;
            for(int64_t k = c - 1; k <= c; k++)
                for(const auto& o : e.realizeCycle(0, k)) {
                    const int64_t t = k * 192 + o.tick + 96 - (int64_t)c * 192;
                    if(t >= 0 && t < 192) expected.push_back((int)t);
                }
            std::sort(expected.begin(), expected.end());
            CHECK(ticksOf(e.realizeCycle(1, c)) == expected, "response cycle %d: %s vs %s", c,
                  str(ticksOf(e.realizeCycle(1, c))).c_str(), str(expected).c_str());
        }
    }

    // ── Choke ──
    {
        Engine e; e.numLayers = 2;
        e.layers[0] = grid(24, 1.0f);
        Layer L = grid(12, 1.0f); L.durTicks = 40; L.chokeLane = 1;
        e.layers[1] = L;
        for(const auto& o : e.realizeCycle(1, 2)) {
            const int nextChoke = (o.tick / 24 + 1) * 24;
            CHECK(o.tick + o.dur <= nextChoke, "choke: gate at %d lasts %d past the choking hit at %d", o.tick, o.dur, nextChoke);
        }
    }

    // ── Ornaments ──
    {
        Engine e;
        Layer L = grid(96, 1.0f); L.shift = 48; L.ghost = 1.0f; L.ghostSide = 0; L.ghostLevel = 0.3f;
        e.layers[0] = L;
        auto v = e.realizeCycle(0, 1);
        CHECK(ticksOf(v) == std::vector<int>({36, 48, 132, 144}), "ghosts before the backbeats: %s", str(ticksOf(v)).c_str());
        for(const auto& o : v) if(o.kind == KIND_GHOST) CHECK(std::abs(o.amp - 0.3f) < 1e-4f, "ghost level %f", o.amp);

        L.ghost = 0.0f; L.flam = 1.0f; L.flamTicks = 2;
        e.layers[0] = L; e.changedAll();
        CHECK(ticksOf(e.realizeCycle(0, 1)) == std::vector<int>({46, 48, 142, 144}), "flams: %s", str(ticksOf(e.realizeCycle(0, 1))).c_str());

        L.flam = 0.0f; L.ratchet = 1.0f; L.ratchetCount = 3; L.ratchetUnit = 6; L.ratchetShape = 1;
        e.layers[0] = L; e.changedAll();
        v = e.realizeCycle(0, 1);
        CHECK(ticksOf(v) == std::vector<int>({48, 54, 60, 144, 150, 156}), "ratchets: %s", str(ticksOf(v)).c_str());
        CHECK(v.size() == 6 && v[0].amp < v[1].amp && v[1].amp < v[2].amp, "crescendo ratchet not rising");
    }

    // ── Euclidean accents and long notes over the hits ──
    {
        Engine e;
        Layer L = grid(24, 1.0f); L.accentHits = 3; L.accentAmount = 1.0f; L.longHits = 3; L.longTicks = 20; L.durTicks = 4;
        e.layers[0] = L;
        const auto v = e.realizeCycle(0, 0);
        int loud = 0, longs = 0;
        for(const auto& o : v) { if(o.amp > 0.99f) loud++; if(o.dur == 20) longs++; }
        CHECK(v.size() == 8 && loud == 3, "euclid accents: %d loud of %zu", loud, v.size());
        CHECK(longs == 3, "euclid long notes: %d", longs);
    }

    // ── Form, renew, keep ──
    {
        Engine e;
        Layer L = grid(12, 0.5f); L.phraseLen = 4; L.form = "AAAB"; L.bStrength = 0.5f; L.renew = 0.0f;
        e.layers[0] = L;
        CHECK(sameCycle(e.realizeCycle(0, 0), e.realizeCycle(0, 1)) && sameCycle(e.realizeCycle(0, 0), e.realizeCycle(0, 2)), "AAA cycles differ");
        CHECK(!sameCycle(e.realizeCycle(0, 0), e.realizeCycle(0, 3)), "B is the same as A");
        for(int c = 4; c < 12; c++) CHECK(sameCycle(e.realizeCycle(0, c), e.realizeCycle(0, c % 4)), "renew 0: phrase does not repeat (cycle %d)", c);

        e.layers[0].renew = 1.0f; e.changedAll();
        CHECK(!sameCycle(e.realizeCycle(0, 0), e.realizeCycle(0, 4)), "renew 1: the next phrase repeats");
        e.setKeep(0, true, 5);
        for(int c = 8; c < 16; c++) CHECK(sameCycle(e.realizeCycle(0, c), e.realizeCycle(0, 4 + c % 4)), "keep: phrase not frozen (cycle %d)", c);
        e.setKeep(0, false, 5);
        CHECK(!sameCycle(e.realizeCycle(0, 8), e.realizeCycle(0, 4)), "keep off: still frozen");
    }

    // ── Fill ──
    {
        Engine e;
        Layer L = grid(48, 1.0f); L.phraseLen = 4; L.fillTicks = 48; L.fillIntensity = 1.0f;
        e.layers[0] = L;
        int fillHits = 0, otherHits = 0;
        for(int c = 0; c < 16; c++)
            for(const auto& o : e.realizeCycle(0, c))
                if(o.tick >= 144) (c % 4 == 3 ? fillHits : otherHits)++;
        CHECK(fillHits > otherHits, "fill: %d hits in the fills vs %d elsewhere (same number of beats)", fillHits, otherHits);
    }

    // ── Mutate lands on the next cycle; undo / redo ──
    {
        Engine e;
        e.layers[0] = grid(12, 0.5f);
        e.layers[0].mutations = 3;
        std::vector<std::vector<Onset>> before;
        for(int c = 0; c < 12; c++) before.push_back(e.realizeCycle(0, c));
        e.mutate(0, 2, 1234);
        for(int c = 0; c <= 2; c++) CHECK(sameCycle(before[c], e.realizeCycle(0, c)), "mutate changed cycle %d (now = 2)", c);
        bool changed = false;
        for(int c = 3; c < 12; c++) if(!sameCycle(before[c], e.realizeCycle(0, c))) changed = true;
        CHECK(changed, "mutate changed nothing");
        bool highlighted = false;
        for(int c = 3; c < 7; c++) if(!e.changedTicks(0, c).empty()) highlighted = true;
        CHECK(highlighted || !changed, "no highlight after a mutation");
        const auto mutated = e.realizeCycle(0, 8);
        CHECK(e.undo(0, 3), "undo failed");
        for(int c = 4; c < 12; c++) CHECK(sameCycle(before[c], e.realizeCycle(0, c)), "undo: cycle %d not restored", c);
        CHECK(e.redo(0, 3), "redo failed");
        CHECK(sameCycle(mutated, e.realizeCycle(0, 8)), "redo: mutation not restored");
        e.setKeep(0, true, 3);
        const auto genome = e.layers[0].genome();
        e.mutate(0, 3, 99);
        CHECK(e.layers[0].genome() == genome, "mutate changed a kept lane");
        e.setKeep(0, false, 3);

        e.like(0);
        e.reroll(0, 3, 555);
        e.like(0);
        CHECK(e.layers[0].liked.size() == 2, "like: %zu liked", e.layers[0].liked.size());
        e.crossover(0, 0, 1, 3);
        bool hasSplice = false;
        for(const auto& op : e.layers[0].ops) if(op.type == OP_SPLICE) hasSplice = true;
        CHECK(hasSplice, "crossover: no splice operator");
        checkInvariants(e, 0, 0, 16, "after evolution");
    }

    // ── Every operator keeps the invariants ──
    {
        for(int type = 0; type < OP_COUNT; type++) {
            Engine e;
            Layer L = grid(12, 0.6f); L.durMode = DUR_LEGATO; L.legato = 0.9f;
            for(uint32_t s = 1; s <= 4; s++) { Op op; op.type = type; op.seed = s * 7919u; op.aux = 42; L.ops.push_back(op); }
            e.layers[0] = L;
            checkInvariants(e, 0, 0, 12, ("operator " + std::to_string(type)).c_str());
        }
    }

    // ── Markov learns where the hits go ──
    {
        Engine e;
        Layer L; L.generator = GEN_MARKOV; L.division = 12; L.examples = "x...x...x...x... x...x...x...x...";
        e.layers[0] = L;
        for(int c = 0; c < 8; c++) {
            const auto t = ticksOf(e.realizeCycle(0, c));
            CHECK(t == std::vector<int>({0, 48, 96, 144}), "markov from four-on-the-floor: %s", str(t).c_str());
        }
        e.numLayers = 2;
        e.layers[0] = grid(12, 0.5f);
        e.changedAll();
        const std::string ex = e.captureExamples(0, 0, 2);
        CHECK(ex.size() == 33 && ex[16] == ' ', "capture: '%s'", ex.c_str());
    }

    // ── Groove ──
    {
        Engine e;
        Layer L = grid(12, 1.0f); L.groove = GROOVE_SWING16; L.swing = 0.66f;
        e.layers[0] = L;
        const auto t = ticksOf(e.realizeCycle(0, 0));
        CHECK(t.size() == 16 && t[1] == 16 && t[3] == 40, "swing 16ths: %s", str(t).c_str());
        e.layers[0].groove = GROOVE_STRAIGHT; e.layers[0].humanize = 3; e.changedAll();
        checkInvariants(e, 0, 0, 8, "humanize");
    }

    // ── Build-up, swell, phrase accent, B strength 0 ──
    {
        Engine e;
        Layer L = grid(12, 1.0f); L.phraseLen = 4; L.buildUp = 1.0f;
        e.layers[0] = L;
        size_t first = 0, last = 0;
        for(int p = 0; p < 8; p++) { first += e.realizeCycle(0, p * 4).size(); last += e.realizeCycle(0, p * 4 + 3).size(); }
        CHECK(last > first * 2, "build-up: %zu hits in first cycles vs %zu in last", first, last);

        L = grid(48, 1.0f); L.phraseLen = 4; L.swell = 1.0f;
        e.layers[0] = L; e.changedAll();
        CHECK(e.realizeCycle(0, 0)[0].amp < e.realizeCycle(0, 3)[3].amp, "swell: amplitude does not grow over the phrase");

        L = grid(48, 1.0f); L.phraseLen = 2; L.level = 1.0f; L.variation = 0.0f; L.ampMetric = 0.0f; L.curve = 1.0f;
        L.accentHits = 0; L.phraseAccent = 1.0f; L.swell = 0.5f;
        e.layers[0] = L; e.changedAll();
        CHECK(e.realizeCycle(0, 2)[0].amp > 0.99f && e.realizeCycle(0, 3)[0].amp < 0.99f, "phrase accent: only the phrase downbeat is at max");

        L = grid(12, 0.5f); L.phraseLen = 4; L.form = "AABB"; L.bStrength = 0.0f; L.renew = 0.0f;
        e.layers[0] = L; e.changedAll();
        CHECK(sameCycle(e.realizeCycle(0, 0), e.realizeCycle(0, 2)), "B strength 0: B differs from A");
    }

    // ── Markov learns a captured lane ──
    {
        Engine e; e.numLayers = 2;
        Layer src = grid(48, 1.0f); src.shift = 24;   // offbeat 8ths
        e.layers[0] = src;
        Layer L; L.generator = GEN_MARKOV; L.division = 12;
        e.layers[1] = L;
        e.layers[1].examples = e.captureExamples(0, 0, 4);
        e.changedAll();
        for(int c = 0; c < 4; c++)
            CHECK(ticksOf(e.realizeCycle(1, c)) == std::vector<int>({24, 72, 120, 168}), "markov from offbeats: %s", str(ticksOf(e.realizeCycle(1, c))).c_str());
        checkInvariants(e, 1, -4, 4, "negative cycles");
    }

    // ── Everything at once, and speed ──
    {
        Engine e; e.numLayers = 8;
        for(int i = 0; i < 8; i++) {
            Layer L;
            L.generator = i % 5;
            L.division = i % 2 ? 12 : 24;
            L.cycleBeats = 3 + i % 4;
            L.density = 0.7f;
            L.weighting = 0.5f; L.push = 0.2f; L.drag = 0.1f; L.crossBarline = 0.5f;
            L.ghost = 0.3f; L.flam = 0.2f; L.ratchet = 0.1f;
            L.phraseLen = 4; L.form = i % 2 ? "AAAB" : ""; L.renew = 0.5f; L.fillTicks = 48;
            L.buildUp = 0.3f; L.swell = 0.3f; L.phraseAccent = 0.5f;
            L.accentHits = 3; L.longHits = 2; L.durMode = i % 3; L.legato = 0.8f;
            L.groove = i % 5; L.humanize = 1;
            if(i > 0) { L.relMode = 1 + i % 5; L.relLane = i; L.chokeLane = (i + 1) % 2 ? i : 0; L.dynMode = 1 + i % 2; L.dynLane = 1; }
            e.layers[i] = L;
        }
        const auto start = std::chrono::steady_clock::now();
        for(int i = 0; i < 8; i++) checkInvariants(e, i, -2, 40, "all rules");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("8 lanes x 42 cycles with every rule: %.1f ms\n", ms);
        CHECK(ms < 2000.0, "too slow: %.1f ms", ms);
        // A change to lane 1 regenerates everything from scratch: time it
        const auto s2 = std::chrono::steady_clock::now();
        e.layers[0].density = 0.6f; e.changed(0);
        for(int i = 0; i < 8; i++) for(int c = 0; c < 4; c++) e.realizeCycle(i, c);
        const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s2).count();
        std::printf("regenerate 8 lanes x 4 cycles after an edit: %.2f ms\n", ms2);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
