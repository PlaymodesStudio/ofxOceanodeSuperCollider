// ChordBox engine tests: tools/chordbox_test/run.sh [a preset's Chord_Sequence_N.json]
#include "../../src/nodes/scChordBoxEngine.h"
#include <cstdio>
using namespace chordbox;
static int failures = 0;
#define CHECK(c, ...) do { if(!(c)) { failures++; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while(0)
static std::string notes(const std::vector<float>& v){ std::string s; for(float x : v){ char b[16]; std::snprintf(b, 16, "%g ", x); s += b; } return s; }

int main(int argc, char** argv){
    const std::string dir = argv[1];
    Engine e;
    e.loadLibraries(dir);
    std::printf("%zu chords, %zu scales, %zu functional, %zu aliases\n", e.chordLibrary.size(), e.scaleLibrary.size(), e.functionalLibrary.size(), e.aliases.size());
    CHECK(e.chordLibrary.size() > 10, "chord library");
    {   // cypher
        float root; std::string q;
        e.parseCypher("Ebm7", root, q);
        CHECK(root == 3, "Eb root %g", root);
        Entry en = e.entryFromToken("Dm7");
        auto v = e.entryPreview(en);
        std::printf("Dm7 -> %s (quality %s)\n", notes(v).c_str(), e.normalizeQuality("m7").c_str());
        CHECK(v.size() >= 4 && v[0] == 2 && v[1] == 5, "Dm7 notes");
        en = e.entryFromToken("CmAdd9");
        std::printf("CmAdd9 -> %s\n", notes(e.entryPreview(en)).c_str());
    }
    {   // a timed progression: positions, laps, determinism
        e.progression = {e.entryFromToken("C"), e.entryFromToken("F"), e.entryFromToken("G")};
        e.progression[1].beatDuration = 2;
        e.progressionOrder = ORDER_ASC;
        e.sanitize(); e.invalidate();
        CHECK(e.eventAt(0.5).entry == 0 && e.eventAt(1.5).entry == 1 && e.eventAt(3.2).entry == 2 && e.eventAt(4.1).entry == 0, "asc positions");
        CHECK(e.eventAt(41.0).entry == e.eventAt(1.0).entry, "lap period 4 beats");
        const auto& ev = e.eventAt(9.5);
        std::printf("at 9.5: step %lld entry %d start %g lapStart %g notes %s\n", (long long)ev.k, ev.entry, ev.start, ev.lapStart, notes(ev.outs[0]).c_str());
        // transposition sequencer: +5 every lap
        e.transposeSequencerSteps = {5}; e.transposeSequencerCycles = 1; e.transposeSequencerMod = 12; e.invalidate();
        CHECK(e.eventAt(0.5).mods.sequencer == 0 && e.eventAt(4.5).mods.sequencer == 5 && e.eventAt(8.5).mods.sequencer == 10, "sequencer per lap %d %d %d",
              e.eventAt(0.5).mods.sequencer, e.eventAt(4.5).mods.sequencer, e.eventAt(8.5).mods.sequencer);
        e.progressionOrder = ORDER_DESC; e.invalidate();
        CHECK(e.eventAt(0.5).entry == 0 && e.eventAt(1.5).entry == 2 && e.eventAt(2.5).entry == 1, "desc order");
        CHECK(e.eventAt(4.5).mods.sequencer == 5, "desc: one lap = one trigger (%d)", e.eventAt(4.5).mods.sequencer);
        // random: reproducible, seed-dependent
        e.progressionOrder = ORDER_RANDOM; e.transposeSequencerSteps = {0}; e.invalidate();
        std::string a; for(int b = 0; b < 30; b++) a += std::to_string(e.eventAt(b + 0.5).entry);
        e.invalidate();
        std::string b2; for(int b = 0; b < 30; b++) b2 += std::to_string(e.eventAt(b + 0.5).entry);
        e.seed = 7; e.invalidate();
        std::string c; for(int b = 0; b < 30; b++) c += std::to_string(e.eventAt(b + 0.5).entry);
        std::printf("random entries %s / seed 7 %s\n", a.c_str(), c.c_str());
        CHECK(a == b2 && a != c, "random reproducible");
        // voice leading keeps a smooth path
        e.seed = 1; e.progressionOrder = ORDER_ASC;
        e.outputs[0].voiceLeadingMode = VL_CLASSIC; e.outputs[0].octave = 4; e.invalidate();
        for(int b = 0; b < 5; b++) std::printf("VL beat %d: %s\n", b, notes(e.eventAt(b + 0.5).outs[0]).c_str());
    }
    if(argc > 2){
        std::ifstream in(argv[2]);
        json j; in >> j;
        e.fromJson(j["state"]); e.invalidate();
        std::printf("preset: %zu steps, order %d, %zu outputs\n", e.progression.size(), e.progressionOrder, e.outputs.size());
        for(size_t i = 0; i < e.progression.size(); i++){
            Engine::Live live;
            auto o = e.playIndex(live, (int)i, true);
            std::printf("  %zu %-10s ", i, e.entryLabel(e.progression[i]).c_str());
            for(auto& v : o) std::printf("| %s", notes(v).c_str());
            std::printf("\n");
        }
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
