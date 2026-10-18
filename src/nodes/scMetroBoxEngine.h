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
//  Per cycle, in order:
//    material   which random stream the cycle uses: its phrase (renewed or
//               repeated, or frozen by Keep) and its letter in the form
//    raw        the generator's onsets, shifted, kept by density, metric
//               weighting, build-up and fills, plus anchors
//    displace   push / drag across the barline
//    relation   follow / avoid / complement / unison / response to a lane above
//    genome     the mutation operators (placement ones)
//    ornaments  ratchets, flams, ghosts
//    amplitude  metric, variation, dynamics stage, phrase shape, Euclidean
//               accents, genome (amplitude ones), duck / reinforce
//    groove     swing / laid back / pushed and humanize (timing)
//    durations  fixed / legato / metric, Euclidean long notes, genome
//               (duration ones), barline, choke
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
constexpr int MAX_VALUES      = 8;    // Weighted generator entries
constexpr int64_t NO_CYCLE    = std::numeric_limits<int64_t>::min();

enum Generator : int { GEN_GRID = 0, GEN_EUCLIDEAN, GEN_WEIGHTED, GEN_SUBDIVISION, GEN_MARKOV };
enum DurationMode : int { DUR_FIXED = 0, DUR_LEGATO, DUR_METRIC };
enum AnchorMode : int { ANCHOR_NONE = 0, ANCHOR_DOWNBEAT, ANCHOR_GROUPS, ANCHOR_BEATS, ANCHOR_BACKBEATS };
enum RelationMode : int { REL_NONE = 0, REL_FOLLOW, REL_AVOID, REL_COMPLEMENT, REL_UNISON, REL_RESPONSE };
enum DynamicsMode : int { DYN_NONE = 0, DYN_DUCK, DYN_REINFORCE };
enum OnsetKind : int { KIND_MAIN = 0, KIND_GHOST, KIND_FLAM, KIND_RATCHET };
enum GrooveMode : int { GROOVE_STRAIGHT = 0, GROOVE_SWING16, GROOVE_SWING8, GROOVE_LAID_BACK, GROOVE_PUSHED };
// Repeat: Off = new material every phrase (Renew); Loop = the phrase playing
// when it was switched on repeats forever; Evolve = that loop, mutated every
// few loops.
enum RepeatMode : int { REPEAT_OFF = 0, REPEAT_LOOP, REPEAT_EVOLVE };
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
    OP_SPLICE,         // the second half comes from another genome (crossover)
    OP_COUNT
};
constexpr int OP_RANDOM_COUNT = OP_SPLICE;  // the ones Mutate picks from

struct Op {
    int type = OP_SPLIT;
    uint32_t seed = 0;
    uint32_t aux = 0;   // OP_SPLICE: the other genome's seed
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
    std::string name = "Layer";
    bool  mute       = false;
    int   generator  = GEN_GRID;
    int   cycleBeats = 4;
    int   seed       = 0;      // the genome's seed (Re-roll changes it)
    std::vector<Op> ops;       // the genome's mutations
    float density    = 1.0f;   // chance to keep each onset
    int   shift      = 0;      // ticks, applied to every generated onset (wraps in the cycle)
    // Grid / Euclidean / Subdivision (finest step) / Markov (step)
    int   division   = 12;     // ticks per step
    int   eucSteps   = 16;
    int   eucHits    = 5;
    int   eucRotation = 0;
    // Weighted durations (beats) and their weights
    std::vector<float> values  = {1.0f, 0.5f, 0.25f};
    std::vector<float> weights = {1.0f, 1.0f, 1.0f};
    // Subdivision
    float complexity   = 0.5f;
    float longOnStrong = 0.5f;
    float triplets     = 0.0f;
    // Markov: example rhythms, one per word ("x..x..x.x...x..."), x = hit
    std::string examples = "x...x...x...x... x..x..x...x.x...";
    // Metric
    std::string grouping = "";
    bool  feelBar      = true;
    float weighting    = 0.0f;
    int   anchorMode   = ANCHOR_NONE;
    float anchorAmount = 1.0f;
    float push         = 0.0f;
    float drag         = 0.0f;
    int   dispUnit     = 12;
    float crossBarline = 1.0f;
    // Phrase
    int   phraseLen    = 4;      // cycles
    std::string form   = "";     // e.g. "AAAB"; empty = every cycle its own
    float renew        = 1.0f;   // chance a phrase brings new material (0 = it repeats)
    float bStrength    = 0.3f;   // how far the other letters mutate A
    int   fillTicks    = 0;      // fill at the end of the phrase (0 = none)
    float fillIntensity = 0.6f;
    float buildUp      = 0.0f;   // density grows over the phrase
    float phraseAccent = 0.0f;   // the phrase downbeat louder
    float swell        = 0.0f;   // amplitude grows over the phrase
    // Ornaments
    float ghost        = 0.0f;
    int   ghostSide    = 0;      // 0 before, 1 after, 2 either
    float ghostLevel   = 0.3f;
    float flam         = 0.0f;
    int   flamTicks    = 2;
    float ratchet      = 0.0f;
    int   ratchetCount = 3;
    int   ratchetUnit  = 6;
    int   ratchetShape = 0;      // 0 flat, 1 crescendo, 2 decrescendo
    // Relations, to lanes above (1-based lane number, 0 = none)
    int   relMode      = REL_NONE;
    int   relLane      = 0;
    float relAmount    = 1.0f;
    int   respDelay    = 96;     // REL_RESPONSE: ticks the other lane is answered after
    int   chokeLane    = 0;
    int   dynMode      = DYN_NONE;
    int   dynLane      = 0;
    float dynAmount    = 0.5f;
    // Groove (timing)
    int   groove       = GROOVE_STRAIGHT;
    float swing        = 0.6f;   // 0.5 straight .. 0.75 hard
    int   humanize     = 0;      // ticks of seeded timing spread
    // Duration
    int   durMode    = DUR_FIXED;
    int   durTicks   = 6;
    float legato     = 0.5f;
    int   durShort   = 6;
    int   durLong    = 48;
    int   longHits   = 0;        // Euclidean long notes over the cycle's hits
    int   longTicks  = 48;
    // Amplitude
    float level      = 1.0f;
    float ampMin     = 0.0f;
    float ampMetric  = 0.0f;
    float variation  = 0.0f;
    float curve      = 1.0f;
    int   tiers      = 0;
    int   accentHits = 0;        // Euclidean accents over the cycle's hits
    float accentAmount = 0.5f;
    bool  accentsLong = false;   // long notes are the accented ones
    // Repeat and evolution
    int     repeat     = REPEAT_OFF;
    bool    keep       = false;  // locked: no mutation (and the phrase frozen)
    int64_t keepPhrase = 0;      // the frozen phrase (Loop, Evolve, Keep)
    int   mutations    = 1;      // operators added by one Mutate
    int   autoEvery    = 2;      // Evolve: loops between mutations
    float autoChance   = 0.5f;   // Evolve: chance of a mutation each time
    std::vector<Genome> liked;

    int cycleTicks() const { return cycleBeats * TICKS_PER_BEAT; }
    Genome genome() const { return {seed, ops}; }
};

class Engine {
public:
    Engine();

    std::array<Layer, MAX_LAYERS> layers;
    int numLayers = 4;
    int globalSeed = 1;
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

    int64_t cycleAt(int lane, double beat) const;
    int64_t phraseOf(int lane, int64_t cycle) const;     // phrase index of a cycle
    bool inFill(int lane, int64_t cycle, int tick) const;

    // ── Evolution (a change lands at nowCycle + 1) ──
    void mutate(int lane, int64_t nowCycle, uint32_t randomSeed);
    void reroll(int lane, int64_t nowCycle, int newSeed);
    bool undo(int lane, int64_t nowCycle);
    bool redo(int lane, int64_t nowCycle);
    void like(int lane);
    void crossover(int lane, int a, int b, int64_t nowCycle);
    void setKeep(int lane, bool on, int64_t nowCycle);
    void setRepeat(int lane, int mode, int64_t nowCycle);
    bool frozen(int lane) const { return layers[lane].keep || layers[lane].repeat != REPEAT_OFF; }
    bool canUndo(int lane) const { return !state[lane].undo.empty(); }
    bool canRedo(int lane) const { return !state[lane].redo.empty(); }
    // Hits of a cycle that were not there before the last change (for the
    // window's highlight); empty when the cycle is not one it changed.
    std::vector<int> changedTicks(int lane, int64_t cycle) const;
    void forgetHighlightsBefore(int lane, int64_t cycle);

    // Markov: example words from another lane's cycles, on a 16th grid
    std::string captureExamples(int sourceLane, int64_t fromCycle, int cycles) const;

    // Strength of a tick (exposed for tests and the window)
    float strengthAt(int lane, int64_t cycle, int tick) const;

private:
    struct Material {
        uint64_t key = 0;      // lane key from the global seed and the genome seed
        uint64_t mat = 0;      // the cycle's random material
        Genome genome;
        int extraOps = 0;      // letters other than A: mutations of A
        uint64_t extraSeed = 0;
    };
    struct LaneState {
        std::vector<Genome> undo, redo;
        bool hasBefore = false;
        Genome before;
        int64_t changeFrom = NO_CYCLE;
        std::map<int64_t, std::vector<int>> oldTicks;   // for the highlight
    };
    std::array<LaneState, MAX_LAYERS> state;
    mutable std::array<std::map<int64_t, std::vector<Onset>>, MAX_LAYERS> cache;

    struct Item {
        int tick; float strength; int kind; float factor;
        bool tied = false; float durScale = 1.0f; bool longNote = false; float amp = 0.0f;
    };
    struct AbsOnset { int64_t tick; int dur; };

    Genome genomeAt(int lane, int64_t cycle) const;
    Material material(int lane, int64_t cycle) const;
    int64_t materialPhrase(int lane, int64_t cycle, uint64_t key) const;
    float effectiveDensity(int lane) const;
    std::vector<int> groupStarts(int lane) const;
    std::vector<int> rawCycle(int lane, int64_t cycle, const Material& m) const;
    std::vector<int> markovCycle(int lane, int64_t cycle, const Material& m) const;
    int displaced(int lane, int64_t cycle, int tick, const Material& m) const;
    std::vector<Item> placeItems(int lane, int64_t cycle) const;
    std::vector<AbsOnset> laneOnsets(int lane, int64_t fromTick, int64_t toTick) const;
    int validLane(int lane, int laneNumber) const;
    float amplitudeOf(int lane, const Material& m, int tick, float strength) const;
    float phraseProgress(int lane, int64_t cycle, int tick) const;
    void applyPlacementOps(int lane, int64_t cycle, const Material& m, std::vector<Item>& items) const;
    void applyAmplitudeOps(int lane, const Material& m, std::vector<Item>& items) const;
    std::vector<Op> opsOf(const Material& m) const;
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
