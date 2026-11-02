// HarmonyBox engine tests: chord symbols, naming, diatonic chords, every
// style's generation (determinism, locks, sizes), voicing and text import.
// Run: tools/harmonybox_test/run.sh   (add -v to print the progressions)

#include "../../src/nodes/boxSynths/harmonyBox/scHarmonyBoxEngine.h"
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace harmonybox;

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if(!(cond)){ failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while(0)

static std::string nameOf(const std::string& sym){
    Chord c;
    return parseChord(sym, c) ? c.name(false) : "<none>";
}

int main(int argc, char** argv){
    const bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;

    // ── Symbols
    struct P { const char* in; const char* out; };
    for(auto p : std::vector<P>{
            {"C", "C"}, {"Am", "Am"}, {"Dm7", "Dm7"}, {"G7", "G7"}, {"Fmaj7", "Fmaj7"}, {"FM7", "Fmaj7"},
            {"BbΔ7", "A#maj7"}, {"Bø", "Bm7b5"}, {"C-7", "Cm7"}, {"Eb6/9", "D#6/9"}, {"F/C", "F/C"},
            {"C/E", "C/E"}, {"G7alt", "G7alt"}, {"Abmaj7#11", "G#maj7#11"}, {"Dsus", "Dsus4"}, {"D(2,4,5)", "Dsus24"},
            {"E(b2,5)", "E(b2,5)"}, {"c#m9", "C#m9"}, {"F#°7", "F#dim7"}, {"Bb13", "A#13"}, {"Gm(maj7)", "GmMaj7"},
            {"A69", "A6/9"}, {"Dbadd2", "C#add9"}, {"Em11/D", "Em11/D"}})
        CHECK(nameOf(p.in) == p.out, "parse %s -> %s (want %s)", p.in, nameOf(p.in).c_str(), p.out);
    {
        Chord c;
        CHECK(!parseChord("H7", c), "H7 should not parse");
        CHECK(!parseChord("Cxyz", c), "Cxyz should not parse");
        CHECK(parseChord("Bb7", c) && c.name(true) == "Bb7", "flats print");
    }

    // ── Naming a set of notes
    CHECK(fromPcs({0, 4, 7, 9}, 9).name(false) == "Am7", "{C E G A}/A = Am7, got %s", fromPcs({0, 4, 7, 9}, 9).name(false).c_str());
    CHECK(fromPcs({0, 4, 7, 9}, 0).name(false) == "C6", "{C E G A}/C = C6");
    CHECK(fromPcs({4, 7, 0}, 4).name(false) == "C/E", "C/E, got %s", fromPcs({4, 7, 0}, 4).name(false).c_str());
    CHECK(fromPcs({2, 4, 7, 9}, 2).name(false) == "D(2,4,5)" || fromPcs({2, 4, 7, 9}, 2).name(false) == "Dsus24",
          "D E G A over D, got %s", fromPcs({2, 4, 7, 9}, 2).name(false).c_str());

    // ── Diatonic sevenths, ninths, thirteenths
    {
        const char* want[7] = {"Cmaj7", "Dm7", "Em7", "Fmaj7", "G7", "Am7", "Bm7b5"};
        for(int d = 0; d < 7; d++){
            const auto n = diatonic(0, modeScale(MODE_MAJOR), d, 4).name(false);
            CHECK(n == want[d], "degree %d: %s (want %s)", d, n.c_str(), want[d]);
        }
        CHECK(diatonic(0, modeScale(MODE_MAJOR), 0, 5).name(false) == "Cmaj9", "Cmaj9");
        CHECK(diatonic(0, modeScale(MODE_MAJOR), 4, 7).name(false) == "G13", "G13, got %s", diatonic(0, modeScale(MODE_MAJOR), 4, 7).name(false).c_str());
        CHECK(diatonic(0, modeScale(MODE_MAJOR), 1, 7).name(false) == "Dm13" || diatonic(0, modeScale(MODE_MAJOR), 1, 7).name(false) == "Dm11",
              "ii stacked, got %s", diatonic(0, modeScale(MODE_MAJOR), 1, 7).name(false).c_str());
        CHECK(diatonic(0, modeScale(MODE_MAJOR), 2, 5).name(false) == "Em7", "iii leaves the b9 out");
        CHECK(diatonic(9, modeScale(MODE_HARMONIC), 4, 4).name(false) == "E7", "A harmonic minor V7");
    }

    // ── Roman numerals
    {
        Key k{0, MODE_MAJOR};
        CHECK(roman(makeChord(7, "7"), k) == "V7", "V7, got %s", roman(makeChord(7, "7"), k).c_str());
        CHECK(roman(makeChord(2, "m7"), k) == "ii7", "ii7");
        CHECK(roman(makeChord(8, "maj7"), k) == "bVIΔ", "bVIΔ, got %s", roman(makeChord(8, "maj7"), k).c_str());
        CHECK(roman(makeChord(0, "", 4), k) == "I6", "I6");
        Key m{9, MODE_MINOR};
        CHECK(roman(makeChord(4, ""), m) == "V", "V in A minor");
        CHECK(roman(makeChord(8, "dim7"), m) == "#vii°7", "#vii°7 in A minor, got %s", roman(makeChord(8, "dim7"), m).c_str());
    }

    // ── Chord scale
    {
        Key k{0, MODE_MAJOR};
        auto s = chordScale(makeChord(2, "7"), k);   // D7 in C: F# replaces F
        std::vector<float> want{0, 2, 4, 5, 7, 9, 10};  // D mixolydian relative to D
        CHECK(s == want, "D7 scale in C");
    }

    // ── Generation, every style
    double worstMs = 0.0;
    for(int style = 0; style < NUM_STYLES; style++){
        for(int n : {1, 2, 4, 8, 16}){
            for(uint64_t seed : {1ull, 7ull, 12345ull}){
                Engine e;
                e.s = Settings::styleDefaults(style);
                e.s.key = (int)(seed % 12);
                e.s.numChords = n;
                const auto t0 = std::chrono::steady_clock::now();
                e.generate(seed);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                worstMs = std::max(worstMs, ms);
                CHECK(e.size() == n, "%s n=%d size %d", styleNames()[style].c_str(), n, e.size());
                CHECK((int)e.voicings.size() == n, "voicings");
                for(int i = 0; i < n; i++){
                    const auto p = e.pitch(i);
                    CHECK((int)p.size() == e.s.voices, "%s voices %d, got %d (%s)", styleNames()[style].c_str(), e.s.voices, (int)p.size(), e.name(i).c_str());
                    for(float x : p) CHECK(x >= e.s.low - 1 && x <= e.s.high + 13, "%s note %g out of range (%s)", styleNames()[style].c_str(), x, e.name(i).c_str());
                    CHECK(pc((int)e.bass(i)) == e.slots[i].chord.bassPc(), "bass pitch class");
                    Chord back;
                    CHECK(parseChord(e.name(i), back) && back.same(e.slots[i].chord), "%s name round trip: %s",
                          styleNames()[style].c_str(), e.name(i).c_str());
                }
                Engine f;
                f.s = e.s;
                f.generate(seed);
                CHECK(f.progressionText() == e.progressionText(), "deterministic");
                if(verbose && seed == 7 && (n == 8 || n == 4)){
                    std::printf("%-12s %-2d in %s %-8s:", styleNames()[style].c_str(), n, keyNames()[e.s.key].c_str(), modeNames()[e.s.mode].c_str());
                    for(int i = 0; i < n; i++) std::printf(" %s(%s)", e.name(i).c_str(), e.romanAt(i).c_str());
                    std::printf("\n");
                }
            }
        }
    }
    std::printf("slowest generate: %.1f ms\n", worstMs);

    // ── Variety across seeds (the generator is not stuck on one answer)
    for(int style = 0; style < NUM_STYLES; style++){
        std::set<std::string> seen;
        for(uint64_t seed = 1; seed <= 20; seed++){
            Engine e;
            e.s = Settings::styleDefaults(style);
            e.s.numChords = 8;
            e.generate(seed);
            seen.insert(e.progressionText());
        }
        CHECK(seen.size() >= 8, "%s: only %d different progressions in 20 seeds", styleNames()[style].c_str(), (int)seen.size());
    }

    // ── Locks and single-slot dice
    {
        Engine e;
        e.s.numChords = 6;
        e.generate(3);
        e.setChordText(2, "Ebmaj7#11");
        CHECK(e.slots[2].locked, "typed chord locks");
        const std::string kept = e.name(2);
        e.generate(99);
        CHECK(e.name(2) == kept, "locked slot kept: %s vs %s", e.name(2).c_str(), kept.c_str());
        const std::string before = e.progressionText();
        e.regenerateSlot(4, 1234);
        int changedSlots = 0;
        Engine ref; ref.s = e.s;
        CHECK(e.name(2) == kept, "dice keeps others");
        (void)before; (void)changedSlots;
        Chord old = e.slots[4].chord;
        e.regenerateSlot(4, 555);
        CHECK(!e.slots[4].chord.same(old), "dice gives a different chord");
    }

    // ── NumChords grows by generating the new ones only
    {
        Engine e;
        e.s.numChords = 4;
        e.generate(5);
        const std::string first = e.name(0) + e.name(1) + e.name(2) + e.name(3);
        e.setNumChords(8, 6);
        CHECK(e.size() == 8, "grown");
        CHECK(e.name(0) + e.name(1) + e.name(2) + e.name(3) == first, "first four kept");
        e.setNumChords(3, 7);
        CHECK(e.size() == 3, "shrunk");
    }

    // ── Key change moves the chords
    {
        Engine e;
        e.setChords(parseProgression("C Am F G"));
        e.setKey(2);
        CHECK(e.progressionText() == "D Bm G A", "transposed to D: %s", e.progressionText().c_str());
    }

    // ── Voice leading moves less than voicing each chord alone
    {
        Engine e;
        e.s.numChords = 8;
        e.s.style = STYLE_JAZZ;
        e.generate(11);
        auto movement = [&](){
            float m = 0.0f;
            for(int i = 0; i < e.size(); i++){
                const auto& a = e.voicings[i].upper;
                const auto& b = e.voicings[(i + 1) % e.size()].upper;
                for(size_t j = 0; j < a.size() && j < b.size(); j++) m += std::abs(a[j] - b[j]);
            }
            return m;
        };
        const float led = movement();
        e.s.voiceLeading = false;
        e.revoice();
        const float alone = movement();
        CHECK(led < alone, "voice leading %g vs alone %g", led, alone);
        for(int spread = 0; spread < NUM_SPREADS; spread++){
            e.s.spread = spread;
            e.s.voiceLeading = true;
            e.revoice();
            for(int i = 0; i < e.size(); i++) CHECK(!e.voicings[i].upper.empty(), "spread %d voiced", spread);
        }
        e.s.bass = false;
        e.s.voices = 3;
        e.setChords(parseProgression("C/E F/A G/B"));
        for(int i = 0; i < 3; i++) CHECK(pc((int)e.pitch(i)[0]) == e.slots[i].chord.bassPc(), "no separate bass: slash note lowest");
    }

    // ── Text import (as the AI answers)
    {
        int k = -1, m = -1;
        auto p = parseProgression("Key: A minor\n1. Am9 | Fmaj7#11 | Dm7, E7b9\nSome words here.\n", &k, &m);
        CHECK(k == 9 && m == MODE_MINOR, "key line, got %d %d", k, m);
        CHECK(p.size() == 4, "4 chords, got %d", (int)p.size());
        if(p.size() == 4) CHECK(p[3].name(false) == "E7b9", "E7b9");
        int k2 = -1, m2 = -1;
        parseProgression("Key: Eb dorian\nEbm7 Ab7", &k2, &m2);
        CHECK(k2 == 3 && m2 == MODE_DORIAN, "Eb dorian");
    }

    // ── State round trip
    {
        Engine e;
        e.s = Settings::styleDefaults(STYLE_REICH);
        e.s.numChords = 7;
        e.generate(42);
        e.slots[3].locked = true;
        Engine f;
        f.fromJson(e.toJson());
        CHECK(f.progressionText() == e.progressionText(), "json chords");
        CHECK(f.slots[3].locked && f.s.style == STYLE_REICH && f.s.numChords == 7, "json settings");
        CHECK(f.pitch(2) == e.pitch(2), "json voicing");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
