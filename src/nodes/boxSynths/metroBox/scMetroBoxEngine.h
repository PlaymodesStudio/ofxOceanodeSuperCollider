//
//  scMetroBoxEngine.h
//  ofxOceanodeSuperCollider
//
//  MetroBox's rhythm generation, free of openFrameworks, ImGui and
//  SuperCollider (so it can be tested on its own, see tools/metrobox_test).
//
//  Everything a lane plays is a pure function of (global seed, lane settings,
//  genome, cycle index): same inputs, same onsets. The node (scMetroBox) owns
//  one engine, draws its cycles and writes them into the player's buffer.
//
//  A lane has a few main settings (the easy row in the window: Every, Offset,
//  Chance, Length, Accent, Change, Smart) and some advanced ones. Per cycle,
//  in order:
//    material   which random stream the cycle uses: its phrase, new or
//               repeated depending on Change
//    raw        the pattern's onsets (Steps / Euclidean / Free), offset, kept
//               by Chance (Smart drops the weak positions first), build-up
//               and fills
//    displace   push / drag across the barline
//    relation   follow / avoid / answer another lane
//    genome     the mutation operators (placement ones)
//    ornaments  ratchets, flams, ghosts
//    amplitude  accents (strong beats / every Nth hit / random) in the soft ..
//               loud range, fills, genome (amplitude ones), duck / boost
//    groove     swing and humanize (timing)
//    durations  Length, longer on strong positions, genome, choke
//

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace metrobox {

constexpr int MAX_LAYERS      = 8;
constexpr int TICKS_PER_BEAT  = 48;
constexpr int MAX_CYCLE_BEATS = 16;
constexpr int MAX_CYCLE_TICKS = MAX_CYCLE_BEATS * TICKS_PER_BEAT;
constexpr int64_t NO_CYCLE    = std::numeric_limits<int64_t>::min();

enum Pattern : int { PAT_STEPS = 0, PAT_EUCLIDEAN, PAT_FREE };
enum AccentSource : int { ACC_STRONG = 0, ACC_EUCLID, ACC_RANDOM };
enum RelationMode : int { REL_NONE = 0, REL_FOLLOW, REL_AVOID, REL_ANSWER };
enum DynamicsMode : int { DYN_NONE = 0, DYN_DUCK, DYN_BOOST };
enum SwingUnit : int { SWING_16THS = 0, SWING_8THS };
enum OnsetKind : int { KIND_MAIN = 0, KIND_GHOST, KIND_FLAM, KIND_RATCHET };
// A lane's links to other lanes: relation, choke, dynamics
enum LinkKind : int { LINK_REL = 0, LINK_CHOKE, LINK_DYN, LINK_COUNT };

// Change, one knob from frozen to always new, read as five zones:
//   Fixed     the phrase playing when it got here repeats; no mutation
//   Loop      the same, but Mutate / Re-roll work
//   Evolving  the loop mutates itself every few phrases (more often to the right)
//   Varying   now and then a new phrase, otherwise the last one again
//   New       a new phrase every time
enum ChangeZone : int { CHANGE_FIXED = 0, CHANGE_LOOP, CHANGE_EVOLVING, CHANGE_VARYING, CHANGE_NEW };
int   changeZone(float change);
bool  changeFrozen(float change);     // Fixed, Loop, Evolving: the phrase repeats
int   evolveEvery(float change);      // Evolving: phrases between mutations
float renewChance(float change);      // chance a phrase brings new material

constexpr int MAX_OPS = 24;     // a genome keeps its latest mutations only

// Mutation operators. Each picks its target (which hit, which half) with a
// hash of its own seed and the cycle's material, so a repeated phrase is
// mutated the same way every time it comes back.
enum OpType : int {
    OP_SPLIT = 0,      // a hit splits its gap: one more hit halfway
    OP_MERGE,          // a hit (not the first) is removed
    OP_NUDGE,          // a hit moves a 16th earlier or later
    OP_ROTATE,         // the whole cycle rotates by a 16th
    OP_REROLL_HALF,    // one half of the cycle is generated again
    OP_TIE,            // a hit absorbs the next one (one long gate)
    OP_LENGTHEN,       // a gate doubles
    OP_SHORTEN,        // a gate halves
    OP_ACCENT_MOVE,    // a hit's amplitude swaps with its neighbour's
    OP_ACCENT_SWAP,    // two hits swap amplitudes
    OP_INVERT_ACCENTS, // loud hits become soft and the other way round
    OP_PROMOTE,        // the softest hit becomes a normal one
    OP_COUNT
};

struct Op {
    int type = OP_SPLIT;
    uint32_t seed = 0;
};

struct Genome {
    int seed = 0;
    std::vector<Op> ops;
    bool operator==(const Genome& o) const;
};

struct Onset {
    int   tick = 0;          // in the cycle
    int   dur  = 1;          // ticks
    float amp  = 1.0f;
    float strength = 0.0f;   // metric strength of the position it was generated on
    int   kind = KIND_MAIN;
};

struct Layer {
    std::string name = "Lane";
    int   id         = 0;      // the lane's own random stream, wherever it moves (0: its position + 1)
    bool  mute       = false;
    bool  advanced   = false;  // the window shows the advanced panel (no effect on the rhythm)

    // ── Main (the easy row) ──
    int   division   = 24;     // Every: ticks per step
    int   shift      = 0;      // Offset: ticks added to every onset (wraps in the cycle)
    float density    = 1.0f;   // Chance to keep each onset
    int   durTicks   = 6;      // Length
    float accent     = 0.3f;   // how much the accented hits stand out
    float change     = 0.25f;  // see ChangeZone
    float smart      = 0.5f;   // weak positions dropped first, strong ones kept

    // ── Pattern ──
    int   pattern    = PAT_STEPS;
    int   cycleBeats = 4;
    int   seed       = 0;      // the genome's seed (Re-roll changes it)
    std::vector<Op> ops;       // the genome's mutations
    int   eucSteps   = 16;
    int   eucHits    = 5;
    int   eucRotation = 0;
    float complexity = 0.5f;   // Free: chance a segment splits
    float triplets   = 0.0f;   // Free: chance a split is in three
    std::string grouping = ""; // beats per group, e.g. "3+3+2"; empty = auto

    // ── Feel ──
    float swing      = 0.0f;   // 0 straight .. 1 hard (offbeat at 3/4)
    int   swingUnit  = SWING_16THS;
    float humanize   = 0.0f;   // seeded timing spread, 0..1 = up to a 32nd early or late
    float push       = 0.0f;   // chance a hit on a beat comes a 16th early
    float drag       = 0.0f;   // chance it comes a 16th late
    float ghost      = 0.0f;   // chance of a quiet hit a 16th before a hit
    float flam       = 0.0f;   // chance of a grace hit just before a hit
    float ratchet    = 0.0f;   // chance a hit becomes a quick burst
    int   ratchetUnit = 6;     // ticks between the burst's hits

    // ── Shape ──
    float longOnStrong = 0.0f; // gates longer on strong positions (up to 4x)
    int   accentSource = ACC_STRONG;
    int   accentHits   = 3;    // ACC_EUCLID: accented hits per cycle, evenly spread
    float ampMin       = 0.0f; // Soft
    float level        = 1.0f; // Loud

    // ── Over time ──
    int   phraseLen  = 4;      // cycles
    float fill       = 0.0f;   // fill at the end of each phrase (0 = none)
    float buildUp    = 0.0f;   // density grows over the phrase
    int64_t keepPhrase = 0;    // the phrase that repeats while Change is frozen

    // ── Lanes (1-based lane number, 0 = none): any other lane, as long as
    //    the links don't loop (see Engine::link) ──
    int   relMode    = REL_NONE;
    int   relLane    = 0;
    float relAmount  = 1.0f;
    int   respDelay  = 96;     // REL_ANSWER: ticks the other lane is answered after
    int   chokeLane  = 0;
    int   dynMode    = DYN_NONE;
    int   dynLane    = 0;
    float dynAmount  = 0.5f;

    int cycleTicks() const { return cycleBeats * TICKS_PER_BEAT; }
    int fillTicks() const;      // 0, one beat or two (from Fill), within the cycle
    Genome genome() const { return {seed, ops}; }
};

class Engine {
public:
    Engine();

    std::array<Layer, MAX_LAYERS> layers;
    int numLayers = 4;
    int globalSeed = 1;          // 0: no seed (see leavePhrase)
    std::vector<float> densityScale = {1.0f};   // one for all lanes, or one per lane
    double mainBarBeats = 4.0;

    // Called whenever a lane's output may have changed (it and the lanes
    // below it): the node rewrites their buffer slots.
    std::function<void(int lane)> onChanged;

    // The cycle with durations (cached)
    std::vector<Onset> realizeCycle(int lane, int64_t cycle) const;
    // A lane's settings changed: regenerate it and the lanes below it
    void changed(int lane);
    void changedAll();

    // Moves a lane to another position, with everything it has (settings,
    // genome, undo history); the other lanes' links follow the lanes they
    // pointed to
    void moveLane(int from, int to);

    // The lane a link really uses (-1: none). Links are taken lane by lane,
    // top to bottom (relation, choke, dynamics): one that would close a loop
    // (1 follows 2 while 2 follows 1) is ignored.
    int link(int lane, int kind) const;
    int linkSetting(int lane, int kind) const;           // the lane number set (1-based, 0 = none)
    bool wouldLoop(int lane, int otherLane) const;       // a link from lane to otherLane (0-based)

    int64_t cycleAt(int lane, double beat) const;
    int64_t phraseOf(int lane, int64_t cycle) const;     // phrase index of a cycle
    bool inFill(int lane, int64_t cycle, int tick) const;

    // Seed 0: no seed. The random streams come from this session, and a
    // phrase that has played (the node calls this when the playhead leaves
    // it) draws new material the next time it comes round (a loop or a seek
    // plays something else). A frozen phrase (Fixed / Loop / Evolving)
    // still repeats.
    bool unseeded() const { return globalSeed == 0; }
    void leavePhrase(int lane, int64_t phrase);

    // Change: entering a frozen zone keeps the phrase playing now
    void setChange(int lane, float change, int64_t nowCycle);

    // ── Evolution (a change lands at nowCycle + 1) ──
    bool canMutate(int lane) const { return changeZone(layers[lane].change) != CHANGE_FIXED; }
    void mutate(int lane, int64_t nowCycle, uint32_t randomSeed);
    void reroll(int lane, int64_t nowCycle, int newSeed);
    bool undo(int lane, int64_t nowCycle);
    bool redo(int lane, int64_t nowCycle);
    bool canUndo(int lane) const { return !state[lane].undo.empty(); }
    bool canRedo(int lane) const { return !state[lane].redo.empty(); }
    // Hits of a cycle that were not there before the last change (for the
    // window's highlight); empty when the cycle is not one it changed.
    std::vector<int> changedTicks(int lane, int64_t cycle) const;
    void forgetHighlightsBefore(int lane, int64_t cycle);

    // Strength of a tick (exposed for tests and the window)
    float strengthAt(int lane, int64_t cycle, int tick) const;

private:
    struct Material {
        uint64_t key = 0;      // lane key from the global seed and the genome seed
        uint64_t mat = 0;      // the cycle's random material
        Genome genome;
    };
    struct LaneState {
        std::vector<Genome> undo, redo;
        bool hasBefore = false;
        Genome before;
        int64_t changeFrom = NO_CYCLE;
        std::map<int64_t, std::vector<int>> oldTicks;   // for the highlight
    };
    std::array<LaneState, MAX_LAYERS> state;
    // Unseeded: times each phrase was left, per lane
    std::array<std::map<int64_t, int>, MAX_LAYERS> passes;
    uint64_t sessionSalt = 0;
    int passOf(int lane, int64_t phrase) const;
    mutable std::array<std::map<int64_t, std::vector<Onset>>, MAX_LAYERS> cache;

    struct Item {
        int tick; float strength; int kind; float factor;
        bool tied = false; float durScale = 1.0f; float amp = 0.0f;
    };
    struct AbsOnset { int64_t tick; int dur; };

    Genome genomeAt(int lane, int64_t cycle) const;
    Material material(int lane, int64_t cycle) const;
    int64_t materialPhrase(int lane, int64_t cycle, uint64_t key) const;
    float effectiveDensity(int lane) const;
    std::vector<int> groupStarts(int lane) const;
    std::vector<int> rawCycle(int lane, int64_t cycle, const Material& m) const;
    int displaced(int lane, int tick, const Material& m) const;
    std::vector<Item> placeItems(int lane, int64_t cycle) const;
    std::vector<AbsOnset> laneOnsets(int lane, int64_t fromTick, int64_t toTick) const;
    mutable bool linksDirty = true;
    mutable std::array<std::array<int, LINK_COUNT>, MAX_LAYERS> links;
    void computeLinks() const;
    bool reaches(int from, int to) const;                // along the links taken
    std::vector<int> dependents(int lane) const;         // lanes that hear this one (through links)
    float phraseProgress(int lane, int64_t cycle, int tick) const;
    void applyPlacementOps(int lane, int64_t cycle, const Material& m, std::vector<Item>& items) const;
    void applyAmplitudeOps(int lane, const Material& m, std::vector<Item>& items) const;
    void pushUndo(int lane, int64_t nowCycle);
    void beginChange(int lane, int64_t nowCycle);
    void setGenome(int lane, const Genome& g);
};

// Helpers shared with the node
std::vector<int> autoGroups(int beats);
float levelStrength(int tick, const std::vector<int>& groupTicks);
uint64_t hashOf(uint64_t a, uint64_t b, uint64_t c = 0, uint64_t d = 0);
float unit(uint64_t h);
int64_t floorDiv(int64_t a, int64_t b);
int64_t positiveMod64(int64_t value, int64_t length);
int64_t tickAt(double beat);

} // namespace metrobox
