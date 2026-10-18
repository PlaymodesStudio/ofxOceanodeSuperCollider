// Tests for MetroBox's rhythm engine (src/nodes/scMetroBoxEngine.*).
// Build and run:  tools/metrobox_test/run.sh

#include "../../src/nodes/scMetroBoxEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

// Steps every `division` ticks with a chance; no Smart, no accent, a new
// phrase every time (so cycles differ)
static Layer steps(int division, float chance) {
    Layer L;
    L.pattern = PAT_STEPS;
    L.division = division;
    L.density = chance;
    L.smart = 0.0f;
    L.accent = 0.0f;
    L.change = 1.0f;
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
    // ── Every / Offset / Length: a plain metro ──
    {
        Engine e;
        Layer L = steps(48, 1.0f); L.shift = 24; L.durTicks = 12;
        e.layers[0] = L;
        const auto v = e.realizeCycle(0, 3);
        CHECK(ticksOf(v) == std::vector<int>({24, 72, 120, 168}), "offbeat quarters: %s", str(ticksOf(v)).c_str());
        for(const auto& o : v) CHECK(o.dur == 12 && std::abs(o.amp - 1.0f) < 1e-6f, "plain metro: dur %d amp %f", o.dur, o.amp);
    }

    // ── Determinism ──
    {
        Engine e;
        e.layers[0] = steps(12, 0.5f);
        const auto a = e.realizeCycle(0, 5);
        e.changedAll();
        CHECK(sameCycle(a, e.realizeCycle(0, 5)), "the same cycle twice differs");
        e.layers[0].seed = 7; e.changedAll();
        bool anyDiff = false;
        for(int c = 0; c < 8; c++) {
            Engine f; f.layers[0] = steps(12, 0.5f);
            if(!sameCycle(f.realizeCycle(0, c), e.realizeCycle(0, c))) anyDiff = true;
        }
        CHECK(anyDiff, "another seed gives the same rhythm");
    }

    // ── Chance is monotonic, with and without Smart ──
    for(float smart : {0.0f, 0.7f}) {
        Engine lo, hi;
        lo.layers[0] = steps(12, 0.3f); lo.layers[0].smart = smart;
        hi.layers[0] = steps(12, 0.7f); hi.layers[0].smart = smart;
        for(int c = 0; c < 16; c++) {
            const auto a = ticksOf(lo.realizeCycle(0, c)), b = ticksOf(hi.realizeCycle(0, c));
            CHECK(std::includes(b.begin(), b.end(), a.begin(), a.end()), "smart %.1f cycle %d: chance 0.3 hits not all in 0.7 (%s / %s)", smart, c, str(a).c_str(), str(b).c_str());
        }
    }

    // ── Smart keeps the downbeats and drops weak 16ths first; chance 0 is silence ──
    {
        Engine e;
        Layer L = steps(12, 0.3f); L.smart = 1.0f;
        e.layers[0] = L;
        int downbeats = 0, weak = 0, weakTotal = 0;
        for(int c = 0; c < 32; c++) {
            const auto t = ticksOf(e.realizeCycle(0, c));
            if(!t.empty() && t.front() == 0) downbeats++;
            for(int x : t) if(x % 24 == 12) weak++;
            weakTotal += 8;
        }
        CHECK(downbeats == 32, "smart: downbeat kept in %d of 32 cycles", downbeats);
        CHECK(weak < weakTotal * 0.15f, "smart: %d of %d weak 16ths kept", weak, weakTotal);
        e.layers[0].density = 0.0f; e.changedAll();
        for(int c = 0; c < 8; c++) CHECK(e.realizeCycle(0, c).empty(), "chance 0 still plays");
        e.layers[0].density = 1.0f; e.changedAll();
        for(int c = 0; c < 8; c++) CHECK(e.realizeCycle(0, c).size() == 16, "smart drops hits at chance 100%%: %zu", e.realizeCycle(0, c).size());
    }

    // ── Euclidean ──
    {
        Engine e;
        Layer L = steps(12, 1.0f); L.pattern = PAT_EUCLIDEAN; L.eucSteps = 8; L.eucHits = 3;
        e.layers[0] = L;
        CHECK(ticksOf(e.realizeCycle(0, 0)).size() == 6, "euclid 3/8 over two rounds: %s", str(ticksOf(e.realizeCycle(0, 0))).c_str());
    }

    // ── Free: strong positions resist splitting with Smart ──
    {
        Engine e;
        Layer L; L.pattern = PAT_FREE; L.division = 12; L.complexity = 0.9f; L.smart = 1.0f; L.change = 1.0f;
        e.layers[0] = L;
        int splitSecondHalf = 0;
        for(int c = 0; c < 16; c++) {
            const auto t = ticksOf(e.realizeCycle(0, c));
            CHECK(!t.empty() && t.front() == 0, "cycle %d: free does not start on 1", c);
            for(int x : t) CHECK(x == 0 || x >= 96, "cycle %d: the downbeat half (strength 1) was split: %s", c, str(t).c_str());
            if(t.size() > 2) splitSecondHalf++;
        }
        CHECK(splitSecondHalf > 0, "the weaker half never splits");
    }

    // ── Push across the barline ──
    {
        Engine e;
        Layer L = steps(48, 1.0f); L.push = 1.0f;
        e.layers[0] = L;
        const auto t = ticksOf(e.realizeCycle(0, 3));
        CHECK(t == std::vector<int>({36, 84, 132, 180}), "pushed beats: %s", str(t).c_str());
    }

    // ── Relations ──
    {
        Engine e; e.numLayers = 2;
        e.layers[0] = steps(12, 0.5f);
        for(int mode : {REL_FOLLOW, REL_AVOID}) {
            Layer L = steps(12, 0.5f); L.seed = 99; L.relMode = mode; L.relLane = 1; L.relAmount = 1.0f;
            e.layers[1] = L; e.changedAll();
            for(int c = 0; c < 8; c++) {
                const auto ta = ticksOf(e.realizeCycle(0, c)), tb = ticksOf(e.realizeCycle(1, c));
                std::vector<int> inter;
                std::set_intersection(ta.begin(), ta.end(), tb.begin(), tb.end(), std::back_inserter(inter));
                if(mode == REL_FOLLOW) CHECK(inter.size() == tb.size(), "follow: lane 2 plays where lane 1 does not");
                if(mode == REL_AVOID) CHECK(inter.empty(), "avoid: coincidences %s", str(inter).c_str());
            }
        }
        // A lane cannot relate to itself
        Layer L = steps(12, 0.5f); L.relMode = REL_AVOID; L.relLane = 2;
        e.layers[1] = L; e.changedAll();
        Engine ref; ref.numLayers = 2; ref.layers[0] = steps(12, 0.5f); ref.layers[1] = steps(12, 0.5f);
        CHECK(sameCycle(e.realizeCycle(1, 2), ref.realizeCycle(1, 2)), "a relation to itself had an effect");
    }

    // ── Links to lanes below work; a loop is refused ──
    {
        Engine e; e.numLayers = 2;
        Layer A = steps(12, 0.5f); A.relMode = REL_AVOID; A.relLane = 2; A.seed = 5;
        e.layers[0] = A;
        e.layers[1] = steps(12, 0.5f);
        e.changedAll();
        CHECK(e.link(0, LINK_REL) == 1, "a link to the lane below was refused");
        for(int c = 0; c < 8; c++) {
            const auto ta = ticksOf(e.realizeCycle(0, c)), tb = ticksOf(e.realizeCycle(1, c));
            std::vector<int> inter;
            std::set_intersection(ta.begin(), ta.end(), tb.begin(), tb.end(), std::back_inserter(inter));
            CHECK(inter.empty(), "avoid a lane below: coincidences %s", str(inter).c_str());
        }
        // lane 2 following lane 1 would close the loop: ignored
        e.layers[1].relMode = REL_FOLLOW; e.layers[1].relLane = 1; e.changed(1);
        CHECK(e.link(1, LINK_REL) == -1 && e.wouldLoop(1, 0), "a loop was accepted");
        e.layers[1].chokeLane = 1; e.changed(1);
        CHECK(e.link(1, LINK_CHOKE) == -1, "a choke loop was accepted");
        checkInvariants(e, 0, 0, 8, "refused loop");
        // cutting lane 1's link frees lane 2's
        e.layers[0].relMode = REL_NONE; e.changed(0);
        CHECK(e.link(1, LINK_REL) == 0, "the link did not come back once the loop was gone");
    }

    // ── Moving a lane: it keeps its rhythm, the links follow it ──
    {
        Engine e; e.numLayers = 3;
        e.layers[0] = steps(12, 0.5f); e.layers[0].name = "Kick";
        e.layers[1] = steps(24, 0.7f); e.layers[1].name = "Snare"; e.layers[1].seed = 3;
        e.layers[2] = steps(12, 0.6f); e.layers[2].name = "Hat"; e.layers[2].relMode = REL_AVOID; e.layers[2].relLane = 1;
        e.changedAll();
        std::vector<std::vector<Onset>> before;
        for(int l = 0; l < 3; l++) before.push_back(e.realizeCycle(l, 4));
        e.mutate(1, 2, 99);
        const auto snareMutated = e.realizeCycle(1, 6);
        e.moveLane(0, 2);   // Kick to the bottom: Snare, Hat, Kick
        CHECK(e.layers[0].name == "Snare" && e.layers[1].name == "Hat" && e.layers[2].name == "Kick", "move order: %s %s %s",
              e.layers[0].name.c_str(), e.layers[1].name.c_str(), e.layers[2].name.c_str());
        CHECK(e.layers[1].relLane == 3, "the hat's link did not follow the kick: %d", e.layers[1].relLane);
        CHECK(e.canUndo(0), "the snare's undo history did not move with it");
        CHECK(sameCycle(e.realizeCycle(0, 6), snareMutated), "the snare changed when moved");
        CHECK(sameCycle(e.realizeCycle(1, 4), before[2]), "the hat changed when moved");
    }

    // ── Answer: replays the lane above, delayed ──
    {
        Engine e; e.numLayers = 2;
        e.layers[0] = steps(24, 0.5f);
        Layer L = steps(12, 1.0f); L.relMode = REL_ANSWER; L.relLane = 1; L.relAmount = 1.0f; L.respDelay = 96;
        e.layers[1] = L;
        for(int c = 1; c < 6; c++) {
            std::vector<int> expected;
            for(int64_t k = c - 1; k <= c; k++)
                for(const auto& o : e.realizeCycle(0, k)) {
                    const int64_t t = k * 192 + o.tick + 96 - (int64_t)c * 192;
                    if(t >= 0 && t < 192) expected.push_back((int)t);
                }
            std::sort(expected.begin(), expected.end());
            CHECK(ticksOf(e.realizeCycle(1, c)) == expected, "answer cycle %d: %s vs %s", c,
                  str(ticksOf(e.realizeCycle(1, c))).c_str(), str(expected).c_str());
        }
    }

    // ── Choke ──
    {
        Engine e; e.numLayers = 2;
        e.layers[0] = steps(24, 1.0f);
        Layer L = steps(12, 1.0f); L.durTicks = 40; L.chokeLane = 1;
        e.layers[1] = L;
        for(const auto& o : e.realizeCycle(1, 2)) {
            const int nextChoke = (o.tick / 24 + 1) * 24;
            CHECK(o.tick + o.dur <= nextChoke, "choke: gate at %d lasts %d past the choking hit at %d", o.tick, o.dur, nextChoke);
        }
    }

    // ── Ornaments ──
    {
        Engine e;
        Layer L = steps(96, 1.0f); L.shift = 48; L.ghost = 1.0f;
        e.layers[0] = L;
        auto v = e.realizeCycle(0, 1);
        CHECK(ticksOf(v) == std::vector<int>({36, 48, 132, 144}), "ghosts before the backbeats: %s", str(ticksOf(v)).c_str());
        for(const auto& o : v) if(o.kind == KIND_GHOST) CHECK(o.amp < 0.5f, "ghost too loud: %f", o.amp);

        L.ghost = 0.0f; L.flam = 1.0f;
        e.layers[0] = L; e.changedAll();
        CHECK(ticksOf(e.realizeCycle(0, 1)) == std::vector<int>({46, 48, 142, 144}), "flams: %s", str(ticksOf(e.realizeCycle(0, 1))).c_str());

        L.flam = 0.0f; L.ratchet = 1.0f; L.ratchetUnit = 6;
        e.layers[0] = L; e.changedAll();
        v = e.realizeCycle(0, 1);
        CHECK(ticksOf(v) == std::vector<int>({48, 54, 60, 144, 150, 156}), "ratchets: %s", str(ticksOf(v)).c_str());
    }

    // ── Accents: strong beats, every Nth hit, random; Soft .. Loud ──
    {
        Engine e;
        Layer L = steps(12, 1.0f); L.accent = 1.0f; L.accentSource = ACC_STRONG;
        e.layers[0] = L;
        auto v = e.realizeCycle(0, 0);
        CHECK(v[0].amp > 0.99f && v[1].amp < v[2].amp && v[2].amp < v[4].amp, "strong accents: %f %f %f %f", v[0].amp, v[1].amp, v[2].amp, v[4].amp);

        L = steps(24, 1.0f); L.accent = 1.0f; L.accentSource = ACC_EUCLID; L.accentHits = 3;
        e.layers[0] = L; e.changedAll();
        v = e.realizeCycle(0, 0);
        int loud = 0;
        for(const auto& o : v) if(o.amp > 0.99f) loud++;
        CHECK(v.size() == 8 && loud == 3, "euclid accents: %d loud of %zu", loud, v.size());

        L.accentSource = ACC_RANDOM; L.ampMin = 0.4f; L.level = 0.8f;
        e.layers[0] = L; e.changedAll();
        for(const auto& o : e.realizeCycle(0, 0)) CHECK(o.amp >= 0.399f && o.amp <= 0.801f, "range: amp %f outside 0.4..0.8", o.amp);

        L = steps(48, 1.0f); L.accent = 0.0f; L.ampMin = 0.0f;
        e.layers[0] = L; e.changedAll();
        for(const auto& o : e.realizeCycle(0, 0)) CHECK(std::abs(o.amp - 1.0f) < 1e-6f, "accent 0 not flat: %f", o.amp);
    }

    // ── Longer on strong ──
    {
        Engine e;
        Layer L = steps(24, 1.0f); L.durTicks = 4; L.longOnStrong = 1.0f;
        e.layers[0] = L;
        const auto v = e.realizeCycle(0, 0);
        CHECK(v[0].dur > v[1].dur, "longer on strong: downbeat %d vs offbeat %d", v[0].dur, v[1].dur);
    }

    // ── Change zones ──
    {
        CHECK(changeZone(0.0f) == CHANGE_FIXED && changeZone(0.25f) == CHANGE_LOOP && changeZone(0.5f) == CHANGE_EVOLVING
              && changeZone(0.8f) == CHANGE_VARYING && changeZone(1.0f) == CHANGE_NEW, "change zones");
        CHECK(evolveEvery(0.4f) == 4 && evolveEvery(0.64f) == 1, "evolve every: %d %d", evolveEvery(0.4f), evolveEvery(0.64f));
        CHECK(renewChance(0.7f) > 0.0f && renewChance(0.7f) < renewChance(0.85f) && renewChance(0.95f) == 1.0f, "renew chance");

        Engine e;
        Layer L = steps(12, 0.5f); L.phraseLen = 2;
        e.layers[0] = L;
        // New: phrases differ
        CHECK(!sameCycle(e.realizeCycle(0, 8), e.realizeCycle(0, 10)), "new: the next phrase repeats");
        // Loop: the phrase playing now (cycles 8, 9) repeats forever
        std::vector<std::vector<Onset>> phrase4 = {e.realizeCycle(0, 8), e.realizeCycle(0, 9)};
        e.setChange(0, 0.25f, 9);
        for(int c = 10; c < 40; c++) CHECK(sameCycle(e.realizeCycle(0, c), phrase4[c % 2]), "loop: cycle %d is not the 2-cycle loop", c);
        CHECK(!sameCycle(e.realizeCycle(0, 10), e.realizeCycle(0, 11)), "loop: the two cycles of the loop are the same");
        // Moving inside the frozen zones keeps the loop
        e.setChange(0, 0.5f, 12);
        CHECK(sameCycle(e.realizeCycle(0, 20), phrase4[0]), "evolving: lost the loop");
        e.setChange(0, 0.25f, 12);

        // Mutate works on a loop, and the loop still repeats afterwards
        e.mutate(0, 11, 77);
        CHECK(e.layers[0].ops.size() == 1, "loop: mutate refused");
        for(int c = 14; c < 30; c++) CHECK(sameCycle(e.realizeCycle(0, c), e.realizeCycle(0, 12 + c % 2)), "loop after mutate: cycle %d", c);

        // Fixed: same loop, no mutation
        const auto before = e.realizeCycle(0, 20);
        e.setChange(0, 0.0f, 20);
        CHECK(sameCycle(before, e.realizeCycle(0, 20)), "fixed changed the loop");
        e.mutate(0, 20, 5);
        e.reroll(0, 20, 9);
        CHECK(e.layers[0].ops.size() == 1 && e.layers[0].seed == 0, "fixed: mutation not blocked");

        // Varying: some phrases repeat, some are new
        e.setChange(0, 0.75f, 20);
        int repeats = 0, news = 0;
        for(int p = 10; p < 60; p++) (sameCycle(e.realizeCycle(0, p * 2), e.realizeCycle(0, p * 2 - 2)) ? repeats : news)++;
        CHECK(repeats > 0 && news > 0, "varying: %d repeats, %d new", repeats, news);

        // Many mutations stay capped
        e.setChange(0, 0.5f, 20);
        for(int i = 0; i < 40; i++) e.mutate(0, 20 + i, 1000 + i);
        CHECK((int)e.layers[0].ops.size() <= MAX_OPS, "evolve: %zu operators kept", e.layers[0].ops.size());
        checkInvariants(e, 0, 0, 80, "evolve");
    }

    // ── Fill ──
    {
        Engine e;
        Layer L = steps(48, 1.0f); L.phraseLen = 4; L.fill = 1.0f;
        e.layers[0] = L;
        CHECK(L.fillTicks() == 96, "fill length %d", L.fillTicks());
        int fillHits = 0, otherHits = 0;
        for(int c = 0; c < 16; c++)
            for(const auto& o : e.realizeCycle(0, c))
                if(o.tick >= 96) (c % 4 == 3 ? fillHits : otherHits)++;
        CHECK(fillHits > otherHits, "fill: %d hits in the fills vs %d elsewhere (same number of beats)", fillHits, otherHits);
    }

    // ── Mutate lands on the next cycle; undo / redo ──
    {
        Engine e;
        e.layers[0] = steps(12, 0.5f);
        std::vector<std::vector<Onset>> before;
        for(int c = 0; c < 12; c++) before.push_back(e.realizeCycle(0, c));
        bool changed = false;
        for(int i = 0; i < 3; i++) e.mutate(0, 2, 1234 + i);
        for(int c = 0; c <= 2; c++) CHECK(sameCycle(before[c], e.realizeCycle(0, c)), "mutate changed cycle %d (now = 2)", c);
        for(int c = 3; c < 12; c++) if(!sameCycle(before[c], e.realizeCycle(0, c))) changed = true;
        CHECK(changed, "mutate changed nothing");
        bool highlighted = false;
        for(int c = 3; c < 7; c++) if(!e.changedTicks(0, c).empty()) highlighted = true;
        CHECK(highlighted || !changed, "no highlight after a mutation");
        const auto mutated = e.realizeCycle(0, 8);
        for(int i = 0; i < 3; i++) CHECK(e.undo(0, 3), "undo failed");
        for(int c = 4; c < 12; c++) CHECK(sameCycle(before[c], e.realizeCycle(0, c)), "undo: cycle %d not restored", c);
        for(int i = 0; i < 3; i++) CHECK(e.redo(0, 3), "redo failed");
        CHECK(sameCycle(mutated, e.realizeCycle(0, 8)), "redo: mutation not restored");
        e.reroll(0, 3, 555);
        CHECK(e.layers[0].seed == 555 && e.layers[0].ops.empty(), "reroll");
        checkInvariants(e, 0, 0, 16, "after evolution");
    }

    // ── Every operator keeps the invariants ──
    for(int type = 0; type < OP_COUNT; type++) {
        Engine e;
        Layer L = steps(12, 0.6f); L.durTicks = 30; L.longOnStrong = 1.0f;
        for(uint32_t s = 1; s <= 4; s++) { Op op; op.type = type; op.seed = s * 7919u; L.ops.push_back(op); }
        e.layers[0] = L;
        checkInvariants(e, 0, 0, 12, ("operator " + std::to_string(type)).c_str());
    }

    // ── Swing and humanize ──
    {
        Engine e;
        Layer L = steps(12, 1.0f); L.swing = 0.64f;   // offbeat 16th at 0.66
        e.layers[0] = L;
        const auto t = ticksOf(e.realizeCycle(0, 0));
        CHECK(t.size() == 16 && t[1] == 16 && t[3] == 40, "swing 16ths: %s", str(t).c_str());
        e.layers[0].swingUnit = SWING_8THS; e.layers[0].division = 24; e.changedAll();
        const auto u = ticksOf(e.realizeCycle(0, 0));
        CHECK(u.size() == 8 && u[1] == 32, "swing 8ths: %s", str(u).c_str());
        e.layers[0].swing = 0.0f; e.layers[0].humanize = 0.5f; e.changedAll();
        checkInvariants(e, 0, 0, 8, "humanize");
    }

    // ── Build-up ──
    {
        Engine e;
        Layer L = steps(12, 1.0f); L.phraseLen = 4; L.buildUp = 1.0f;
        e.layers[0] = L;
        size_t first = 0, last = 0;
        for(int p = 0; p < 8; p++) { first += e.realizeCycle(0, p * 4).size(); last += e.realizeCycle(0, p * 4 + 3).size(); }
        CHECK(last > first * 2, "build-up: %zu hits in first cycles vs %zu in last", first, last);
        checkInvariants(e, 0, -4, 4, "negative cycles");
    }

    // ── Seed 0: unseeded, a phrase left plays something new next time; loops repeat ──
    {
        Engine e; e.globalSeed = 0;
        Layer L = steps(12, 0.5f); L.phraseLen = 1;   // Change: New
        e.layers[0] = L;
        const auto first = e.realizeCycle(0, 3);
        CHECK(sameCycle(first, e.realizeCycle(0, 3)), "unseeded: the same pass differs (the window would lie)");
        e.leavePhrase(0, 3);
        CHECK(!sameCycle(first, e.realizeCycle(0, 3)), "unseeded: a phrase played again repeats");
        Engine f; f.globalSeed = 0; f.layers[0] = L;
        CHECK(!sameCycle(e.realizeCycle(0, 7), f.realizeCycle(0, 7)), "unseeded: two sessions play the same");
        e.setChange(0, 0.25f, 3);   // Loop
        const auto loop = e.realizeCycle(0, 10);
        e.leavePhrase(0, 10);
        CHECK(sameCycle(loop, e.realizeCycle(0, 10)), "unseeded: a loop stopped repeating");
        Engine g; g.layers[0] = L;   // seeded: leavePhrase changes nothing
        const auto seeded = g.realizeCycle(0, 3);
        g.leavePhrase(0, 3);
        CHECK(sameCycle(seeded, g.realizeCycle(0, 3)), "seeded: leaving a phrase changed it");
    }

    // ── Everything at once, and speed ──
    {
        Engine e; e.numLayers = 8;
        for(int i = 0; i < 8; i++) {
            Layer L;
            L.pattern = i % 3;
            L.division = i % 2 ? 12 : 24;
            L.cycleBeats = 3 + i % 4;
            L.density = 0.7f; L.smart = 0.5f; L.accent = 0.6f; L.accentSource = i % 3;
            L.change = (float)i / 7.0f;
            L.push = 0.2f; L.drag = 0.1f;
            L.ghost = 0.3f; L.flam = 0.2f; L.ratchet = 0.1f;
            L.phraseLen = 4; L.fill = 0.6f; L.buildUp = 0.3f; L.longOnStrong = 0.5f;
            L.swing = 0.3f; L.swingUnit = i % 2; L.humanize = 0.2f;
            if(i > 0) { L.relMode = 1 + i % 3; L.relLane = i; L.chokeLane = (i + 1) % 2 ? i : 0; L.dynMode = 1 + i % 2; L.dynLane = 1; }
            e.layers[i] = L;
        }
        const auto start = std::chrono::steady_clock::now();
        for(int i = 0; i < 8; i++) checkInvariants(e, i, -2, 40, "all rules");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("8 lanes x 42 cycles with every rule: %.1f ms\n", ms);
        CHECK(ms < 2000.0, "too slow: %.1f ms", ms);
        const auto s2 = std::chrono::steady_clock::now();
        e.layers[0].density = 0.6f; e.changed(0);
        for(int i = 0; i < 8; i++) for(int c = 0; c < 4; c++) e.realizeCycle(i, c);
        const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s2).count();
        std::printf("regenerate 8 lanes x 4 cycles after an edit: %.2f ms\n", ms2);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
