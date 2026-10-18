//
//  scMetroBoxEngine.cpp
//  ofxOceanodeSuperCollider
//

#include "scMetroBoxEngine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <sstream>

namespace metrobox {

namespace {

constexpr int TPB = TICKS_PER_BEAT;

// Salts: each kind of decision draws from its own hash, so changing one
// setting (density, say) does not reshuffle the others.
enum Salt : uint64_t {
    SALT_KEEP = 1, SALT_AMP, SALT_CHOICE, SALT_SUBDIV, SALT_ANCHOR, SALT_DISP, SALT_CROSS, SALT_CROSSDUR,
    SALT_REL, SALT_FILL, SALT_GHOST, SALT_GHOSTSIDE, SALT_FLAM, SALT_RATCHET, SALT_PHRASE, SALT_MATERIAL,
    SALT_LETTER, SALT_FILLX, SALT_HUMAN, SALT_REROLL, SALT_MARKOV, SALT_OPS
};

uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Sequential stream, for choices that depend on the previous ones
struct Stream {
    uint64_t state;
    float next() { state = mix64(state); return unit(state); }
    int pick(int n) { return n <= 0 ? 0 : std::min(n - 1, (int)(next() * n)); }
};

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

std::vector<std::string> splitWords(const std::string& s, const std::string& separators) {
    std::vector<std::string> out;
    std::string cur;
    for(char ch : s) {
        if(separators.find(ch) != std::string::npos) { if(!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    if(!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<int> groupTicksOf(const std::vector<int>& beatsPerGroup) {
    std::vector<int> ticks;
    int t = 0;
    for(int b : beatsPerGroup) { ticks.push_back(t); t += b * TPB; }
    return ticks;
}

// Index of a strength level, for the Markov tables
int strengthClass(float s) {
    static const float levels[] = {1.0f, 0.8f, 0.6f, 0.4f, 0.35f, 0.25f, 0.2f, 0.12f, 0.05f};
    for(int i = 0; i < 9; i++) if(std::abs(s - levels[i]) < 1e-4f) return i;
    return 8;
}

bool isAmplitudeOp(int type) {
    return type == OP_ACCENT_MOVE || type == OP_ACCENT_SWAP || type == OP_INVERT_ACCENTS || type == OP_PROMOTE;
}

}

// ════════════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════════════

uint64_t hashOf(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return mix64(mix64(mix64(mix64(a) ^ b) ^ c) ^ d);
}

float unit(uint64_t h) {
    return (float)((h >> 40) * (1.0 / 16777216.0));
}

int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

int64_t positiveMod64(int64_t value, int64_t length) {
    if(length <= 0) return 0;
    int64_t r = value % length;
    return r < 0 ? r + length : r;
}

int64_t tickAt(double beat) {
    return (int64_t)std::floor(beat * TPB + 1e-6);
}

// Strength of a tick from the meter hierarchy: cycle start, group starts,
// beats, 8ths, 8th triplets, 16ths, 16th triplets, 32nds.
float levelStrength(int t, const std::vector<int>& groupTicks) {
    if(t == 0) return 1.0f;
    for(int g : groupTicks) if(t == g) return 0.8f;
    if(t % TPB == 0) return 0.6f;
    if(t % 24 == 0) return 0.4f;
    if(t % 16 == 0) return 0.35f;
    if(t % 12 == 0) return 0.25f;
    if(t % 8 == 0) return 0.2f;
    if(t % 6 == 0) return 0.12f;
    return 0.05f;
}

// Default grouping of a cycle in beats: 4 = 2+2, 6 = 3+3, 8 = 4+4, 5 = 3+2,
// 7 = 3+2+2...
std::vector<int> autoGroups(int beats) {
    if(beats <= 3) return {std::max(1, beats)};
    std::vector<int> g;
    if(beats % 4 == 0 && beats >= 8) g.assign(beats / 4, 4);
    else if(beats % 3 == 0) g.assign(beats / 3, 3);
    else if(beats % 2 == 0) g.assign(beats / 2, 2);
    else { g.push_back(3); g.insert(g.end(), (beats - 3) / 2, 2); }
    return g;
}

bool Genome::operator==(const Genome& o) const {
    if(seed != o.seed || ops.size() != o.ops.size()) return false;
    for(size_t i = 0; i < ops.size(); i++)
        if(ops[i].type != o.ops[i].type || ops[i].seed != o.ops[i].seed || ops[i].aux != o.ops[i].aux) return false;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Engine
// ════════════════════════════════════════════════════════════════════════════

Engine::Engine() {
    for(int i = 0; i < MAX_LAYERS; i++) layers[i].name = "Layer " + std::to_string(i + 1);
}

void Engine::changed(int lane) {
    if(lane < 0 || lane >= MAX_LAYERS) return;
    for(int j = lane; j < MAX_LAYERS; j++) {
        cache[j].clear();
        if(onChanged) onChanged(j);
    }
}

void Engine::changedAll() { changed(0); }

float Engine::effectiveDensity(int lane) const {
    float scale = 1.0f;
    if(densityScale.size() == 1) scale = densityScale[0];
    else if(lane < (int)densityScale.size()) scale = densityScale[lane];
    return clampf(layers[lane].density * scale, 0.0f, 1.0f);
}

int Engine::validLane(int lane, int laneNumber) const {
    const int idx = laneNumber - 1;
    return idx >= 0 && idx < lane && idx < numLayers ? idx : -1;
}

int64_t Engine::cycleAt(int lane, double beat) const {
    return floorDiv(tickAt(beat), layers[lane].cycleTicks());
}

int64_t Engine::phraseOf(int lane, int64_t cycle) const {
    return floorDiv(cycle, std::max(1, layers[lane].phraseLen));
}

bool Engine::inFill(int lane, int64_t cycle, int tick) const {
    const Layer& L = layers[lane];
    const int n = std::max(1, L.phraseLen);
    return L.fillTicks > 0 && positiveMod64(cycle, n) == n - 1 && tick >= L.cycleTicks() - L.fillTicks;
}

float Engine::phraseProgress(int lane, int64_t cycle, int tick) const {
    const Layer& L = layers[lane];
    const int n = std::max(1, L.phraseLen);
    const int cT = L.cycleTicks();
    return (float)(positiveMod64(cycle, n) * cT + tick) / (float)(n * cT);
}

std::vector<int> Engine::groupStarts(int lane) const {
    const Layer& L = layers[lane];
    std::vector<int> beats;
    int sum = 0;
    for(const auto& part : splitWords(L.grouping, "+ ")) {
        const int b = std::atoi(part.c_str());
        if(b <= 0) { beats.clear(); sum = -1; break; }
        beats.push_back(b);
        sum += b;
    }
    if(sum != L.cycleBeats) beats = autoGroups(L.cycleBeats);
    return groupTicksOf(beats);
}

float Engine::strengthAt(int lane, int64_t cycle, int tick) const {
    const Layer& L = layers[lane];
    float s = levelStrength(tick, groupStarts(lane));
    if(L.feelBar) {
        // The main bar's hierarchy at the same absolute position: a 5-beat
        // lane still leans on the bar's downbeat whenever it lands there.
        const int barTicks = std::max(1, (int)std::lround(mainBarBeats * TPB));
        const int barBeats = (int)std::lround(mainBarBeats);
        const std::vector<int> barGroups = barBeats * TPB == barTicks ? groupTicksOf(autoGroups(barBeats)) : std::vector<int>{0};
        const int64_t absTick = cycle * L.cycleTicks() + tick;
        s = std::max(s, levelStrength((int)positiveMod64(absTick, barTicks), barGroups));
    }
    return s;
}

// ── Material ────────────────────────────────────────────────────────────────

Genome Engine::genomeAt(int lane, int64_t cycle) const {
    const LaneState& st = state[lane];
    if(st.hasBefore && cycle < st.changeFrom) return st.before;
    return layers[lane].genome();
}

// Which random stream a cycle uses. Its phrase either brings new material
// (chance Renew) or repeats the last one that did; Keep freezes it. In a
// form, every letter takes the A material, the other letters mutated by it.
Engine::Material Engine::material(int lane, int64_t cycle) const {
    const Layer& L = layers[lane];
    Material m;
    m.genome = genomeAt(lane, cycle);
    m.key = hashOf((uint64_t)(uint32_t)globalSeed, (uint64_t)(uint32_t)m.genome.seed, (uint64_t)lane + 1);
    const int n = std::max(1, L.phraseLen);
    const int64_t q = positiveMod64(cycle, n);
    const int64_t pm = materialPhrase(lane, cycle, m.key);

    uint64_t letter = (uint64_t)q;
    if(!L.form.empty()) {
        const char ch = (char)std::toupper((unsigned char)L.form[(size_t)(q % (int64_t)L.form.size())]);
        letter = 0;
        if(ch != 'A') {
            m.extraOps = (int)std::lround(L.bStrength * 6.0f);
            m.extraSeed = hashOf(m.key, (uint64_t)pm, (uint64_t)(unsigned char)ch, SALT_LETTER);
        }
    }
    m.mat = hashOf(m.key, (uint64_t)pm, letter, SALT_MATERIAL);
    return m;
}

// The phrase whose material a cycle plays: a frozen one (Loop, Evolve,
// Keep), or the last phrase that renewed (chance Renew each phrase).
int64_t Engine::materialPhrase(int lane, int64_t cycle, uint64_t key) const {
    const Layer& L = layers[lane];
    if(frozen(lane)) return L.keepPhrase;
    const int64_t p = floorDiv(cycle, std::max(1, L.phraseLen));
    if(L.renew >= 1.0f || p <= 0) return p;
    for(int64_t k = p; k >= 1 && k > p - 256; k--)
        if(unit(hashOf(key, (uint64_t)k, SALT_PHRASE)) < L.renew) return k;
    return 0;
}

std::vector<Op> Engine::opsOf(const Material& m) const {
    std::vector<Op> ops = m.genome.ops;
    Stream s{m.extraSeed};
    for(int i = 0; i < m.extraOps; i++) {
        Op op;
        op.type = s.pick(OP_RANDOM_COUNT);
        op.seed = (uint32_t)(s.next() * 4294967295.0f);
        ops.push_back(op);
    }
    return ops;
}

// ── Generators ──────────────────────────────────────────────────────────────

// Markov: hit / rest transitions learned from the example words, keyed by
// the metric strength of the position and the two previous steps.
std::vector<int> Engine::markovCycle(int lane, int64_t cycle, const Material& m) const {
    const Layer& L = layers[lane];
    const int cT = L.cycleTicks();
    constexpr int CLASSES = 9;
    float hits[CLASSES][4] = {}, total[CLASSES][4] = {};
    float classHits[CLASSES] = {}, classTotal[CLASSES] = {};
    float allHits = 0, allTotal = 0;
    for(const auto& word : splitWords(L.examples, " ,;\n\t")) {
        const int len = (int)word.size();
        const int ticks = len * 12;
        const std::vector<int> groups = ticks % TPB == 0 ? groupTicksOf(autoGroups(ticks / TPB)) : std::vector<int>{0};
        int prev = 0;
        for(int i = 0; i < len; i++) {
            const bool hit = word[i] == 'x' || word[i] == 'X' || word[i] == '1';
            const int cls = strengthClass(levelStrength(i * 12, groups));
            total[cls][prev] += 1; classTotal[cls] += 1; allTotal += 1;
            if(hit) { hits[cls][prev] += 1; classHits[cls] += 1; allHits += 1; }
            prev = ((prev << 1) | (hit ? 1 : 0)) & 3;
        }
    }
    std::vector<int> out;
    if(allTotal <= 0) return out;
    const std::vector<int> groups = groupStarts(lane);
    const int step = std::max(1, L.division);
    Stream stream{hashOf(m.mat, SALT_MARKOV)};
    int prev = 0;
    for(int t = 0; t < cT; t += step) {
        const int cls = strengthClass(levelStrength(t, groups));
        float p = allHits / allTotal;
        if(total[cls][prev] > 0) p = hits[cls][prev] / total[cls][prev];
        else if(classTotal[cls] > 0) p = classHits[cls] / classTotal[cls];
        const bool hit = stream.next() < p;
        if(hit) out.push_back(t);
        prev = ((prev << 1) | (hit ? 1 : 0)) & 3;
    }
    return out;
}

// The generator's onsets (shifted), kept by density, metric weighting,
// build-up and fills, plus the anchors. A tick is kept with a hash of
// (material, tick) against its chance, so raising the density only ever adds
// hits and lowering it only removes them.
std::vector<int> Engine::rawCycle(int lane, int64_t cycle, const Material& m) const {
    const Layer& L = layers[lane];
    const int cT = L.cycleTicks();
    const float density = effectiveDensity(lane);
    const std::vector<int> groups = groupStarts(lane);
    std::vector<int> gen;

    if(L.generator == GEN_GRID || L.generator == GEN_EUCLIDEAN) {
        const int step = std::max(1, L.division);
        const int n = std::max(1, L.eucSteps);
        const int k = std::clamp(L.eucHits, 0, n);
        int i = 0;
        for(int t = 0; t < cT; t += step, i++) {
            bool hit = true;
            if(L.generator == GEN_EUCLIDEAN) {
                const int64_t s = positiveMod64((int64_t)i + L.eucRotation, n);
                hit = (s * k) % n < k;
            }
            if(hit) gen.push_back(t);
        }
    } else if(L.generator == GEN_WEIGHTED) {
        // From the cycle start, draw the next interval from the list until the
        // cycle is full. Every cycle starts on its downbeat; the last interval
        // is cut at the cycle end.
        const int count = std::clamp((int)L.values.size(), 0, MAX_VALUES);
        float total = 0.0f;
        for(int j = 0; j < count; j++) total += j < (int)L.weights.size() ? std::max(0.0f, L.weights[j]) : 0.0f;
        Stream stream{hashOf(m.mat, SALT_CHOICE)};
        for(int t = 0; t < cT && count > 0;) {
            int pick = count - 1;
            if(total > 0.0f) {
                float r = stream.next() * total;
                for(int j = 0; j < count; j++) {
                    r -= j < (int)L.weights.size() ? std::max(0.0f, L.weights[j]) : 0.0f;
                    if(r < 0.0f) { pick = j; break; }
                }
            } else {
                pick = stream.pick(count);
            }
            gen.push_back(t);
            t += std::max(1, (int)std::lround(L.values[pick] * TPB));
        }
    } else if(L.generator == GEN_SUBDIVISION) {
        // Each group is a segment; a segment splits in two (or three) with a
        // chance that falls on strong positions (and rises in a fill), down
        // to the finest step. Every segment that stays whole is an onset.
        const int finest = std::max(1, L.division);
        Stream stream{hashOf(m.mat, SALT_SUBDIV)};
        std::function<void(int, int)> split = [&](int start, int len) {
            const bool two = len % 2 == 0 && len / 2 >= finest;
            const bool three = len % 3 == 0 && len / 3 >= finest;
            const int at = (int)positiveMod64((int64_t)start + L.shift, cT);
            const float s = strengthAt(lane, cycle, at);
            float chance = L.complexity * (1.0f - L.longOnStrong * s);
            if(inFill(lane, cycle, at)) chance += L.fillIntensity * (1.0f - chance);
            if((two || three) && stream.next() < chance) {
                const int parts = three && (!two || stream.next() < L.triplets) ? 3 : 2;
                for(int p = 0; p < parts; p++) split(start + p * len / parts, len / parts);
            } else {
                gen.push_back(start);
            }
        };
        for(size_t g = 0; g < groups.size(); g++) {
            const int end = g + 1 < groups.size() ? groups[g + 1] : cT;
            split(groups[g], end - groups[g]);
        }
    } else {
        gen = markovCycle(lane, cycle, m);
    }

    std::vector<int> out;
    out.reserve(gen.size() + 16);
    for(int t : gen) {
        const int st = (int)positiveMod64((int64_t)t + L.shift, cT);
        const float s = strengthAt(lane, cycle, st);
        const float build = 1.0f - L.buildUp * (1.0f - phraseProgress(lane, cycle, st));
        float chance = density * build * (1.0f - L.weighting + L.weighting * s);
        if(inFill(lane, cycle, st)) chance += L.fillIntensity * (1.0f - chance);
        if(unit(hashOf(m.mat, (uint64_t)st, SALT_KEEP)) < chance) out.push_back(st);
    }

    // A fill also brings its own 16ths, whatever the generator
    if(L.fillTicks > 0 && L.fillIntensity > 0.0f) {
        const int n = std::max(1, L.phraseLen);
        if(positiveMod64(cycle, n) == n - 1) {
            const int start = ((cT - L.fillTicks + 11) / 12) * 12;
            for(int t = std::max(0, start); t < cT; t += 12)
                if(unit(hashOf(m.mat, (uint64_t)t, SALT_FILLX)) < L.fillIntensity * 0.7f) out.push_back(t);
        }
    }

    if(L.anchorMode != ANCHOR_NONE) {
        std::vector<int> anchors;
        if(L.anchorMode == ANCHOR_DOWNBEAT) anchors = {0};
        else if(L.anchorMode == ANCHOR_GROUPS) anchors = groups;
        else
            for(int b = 0; b < L.cycleBeats; b++)
                if(L.anchorMode == ANCHOR_BEATS || b % 2 == 1) anchors.push_back(b * TPB);
        for(int a : anchors)
            if(unit(hashOf(m.mat, (uint64_t)a, SALT_ANCHOR)) < L.anchorAmount) out.push_back(a);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Push (anticipation) or drag of an onset on a beat by one unit. A move across
// the cycle start lands in the neighbouring cycle, if the barline lets it.
int Engine::displaced(int lane, int64_t cycle, int tick, const Material& m) const {
    const Layer& L = layers[lane];
    if(tick % TPB != 0 || (L.push <= 0.0f && L.drag <= 0.0f)) return tick;
    const int cT = L.cycleTicks();
    const float r = unit(hashOf(m.mat, (uint64_t)tick, SALT_DISP));
    int moved = tick;
    if(r < L.push) moved = tick - L.dispUnit;
    else if(r < L.push + L.drag) moved = tick + L.dispUnit;
    if((moved < 0 || moved >= cT) && unit(hashOf(m.mat, (uint64_t)tick, SALT_CROSS)) >= L.crossBarline)
        moved = tick;
    return moved;
}

float Engine::amplitudeOf(int lane, const Material& m, int tick, float strength) const {
    const Layer& L = layers[lane];
    const float u = unit(hashOf(m.mat, (uint64_t)tick, SALT_AMP));
    const float metric = 1.0f - L.ampMetric + L.ampMetric * strength;
    float a = clampf(metric * (1.0f - L.variation * u), 0.0f, 1.0f);
    a = std::pow(a, std::max(0.1f, L.curve));
    if(L.tiers >= 2) a = std::round(a * (L.tiers - 1)) / (float)(L.tiers - 1);
    return clampf(L.ampMin + (L.level - L.ampMin) * a, 0.001f, 1.0f);
}

// Onsets (with durations) of another lane between two absolute ticks, gates
// reaching into the range included.
std::vector<Engine::AbsOnset> Engine::laneOnsets(int lane, int64_t fromTick, int64_t toTick) const {
    std::vector<AbsOnset> out;
    const int cT = layers[lane].cycleTicks();
    for(int64_t k = floorDiv(fromTick, cT) - 1; k <= floorDiv(toTick - 1, cT); k++) {
        for(const auto& o : realizeCycle(lane, k)) {
            const int64_t at = k * cT + o.tick;
            if(at < toTick && at + o.dur > fromTick) out.push_back({at, o.dur});
        }
    }
    std::sort(out.begin(), out.end(), [](const AbsOnset& a, const AbsOnset& b) { return a.tick < b.tick; });
    return out;
}

// ── Genome ──────────────────────────────────────────────────────────────────

void Engine::applyPlacementOps(int lane, int64_t cycle, const Material& m, std::vector<Item>& items) const {
    const Layer& L = layers[lane];
    const int cT = L.cycleTicks();
    auto sortItems = [&] {
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.tick < b.tick; });
        items.erase(std::unique(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.tick == b.tick; }), items.end());
    };
    auto occupied = [&](int t) { for(const auto& it : items) if(it.tick == t) return true; return false; };
    auto regenerateHalf = [&](int lo, int hi, const Material& alt) {
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& it) { return it.tick >= lo && it.tick < hi; }), items.end());
        for(int t : rawCycle(lane, cycle, alt))
            if(t >= lo && t < hi) items.push_back({t, strengthAt(lane, cycle, t), KIND_MAIN, 1.0f});
    };

    for(const Op& op : opsOf(m)) {
        if(isAmplitudeOp(op.type)) continue;
        Stream s{hashOf((uint64_t)op.seed, m.mat, SALT_OPS)};
        sortItems();
        const int n = (int)items.size();
        switch(op.type) {
            case OP_SPLIT: {
                if(n == 0) break;
                const int j = s.pick(n);
                const int next = j + 1 < n ? items[j + 1].tick : cT;
                const int gap = next - items[j].tick;
                if(gap < 24) break;
                const int t = items[j].tick + std::max(12, (gap / 2) / 12 * 12);
                if(t < next && t < cT && !occupied(t)) items.push_back({t, strengthAt(lane, cycle, t), KIND_MAIN, 1.0f});
                break;
            }
            case OP_MERGE:
                if(n > 1) items.erase(items.begin() + 1 + s.pick(n - 1));
                break;
            case OP_NUDGE: {
                if(n == 0) break;
                Item& it = items[s.pick(n)];
                const int t = it.tick + (s.next() < 0.5f ? -12 : 12);
                if(t >= 0 && t < cT && !occupied(t)) it.tick = t;
                break;
            }
            case OP_ROTATE: {
                const int d = s.next() < 0.5f ? -12 : 12;
                for(auto& it : items) it.tick = (int)positiveMod64((int64_t)it.tick + d, cT);
                break;
            }
            case OP_REROLL_HALF: {
                const int h = s.pick(2);
                Material alt = m;
                alt.mat = hashOf(m.mat, (uint64_t)op.seed, SALT_REROLL);
                regenerateHalf(h * cT / 2, (h + 1) * cT / 2, alt);
                break;
            }
            case OP_SPLICE: {
                Material alt = m;
                alt.key = hashOf((uint64_t)(uint32_t)globalSeed, (uint64_t)op.aux, (uint64_t)lane + 1);
                alt.mat = hashOf(alt.key, m.mat, SALT_MATERIAL);
                regenerateHalf(cT / 2, cT, alt);
                break;
            }
            case OP_TIE: {
                if(n < 2) break;
                const int j = s.pick(n - 1);
                items[j].tied = true;
                items.erase(items.begin() + j + 1);
                break;
            }
            case OP_LENGTHEN:
                if(n > 0) items[s.pick(n)].durScale *= 2.0f;
                break;
            case OP_SHORTEN:
                if(n > 0) items[s.pick(n)].durScale *= 0.5f;
                break;
            default: break;
        }
    }
    sortItems();
}

void Engine::applyAmplitudeOps(int lane, const Material& m, std::vector<Item>& items) const {
    const Layer& L = layers[lane];
    for(const Op& op : opsOf(m)) {
        if(!isAmplitudeOp(op.type)) continue;
        Stream s{hashOf((uint64_t)op.seed, m.mat, SALT_OPS)};
        std::vector<Item*> mains;
        for(auto& it : items) if(it.kind == KIND_MAIN) mains.push_back(&it);
        const int n = (int)mains.size();
        switch(op.type) {
            case OP_ACCENT_MOVE:
                if(n > 1) { const int j = s.pick(n - 1); std::swap(mains[j]->amp, mains[j + 1]->amp); }
                break;
            case OP_ACCENT_SWAP:
                if(n > 1) { const int a = s.pick(n), b = s.pick(n); std::swap(mains[a]->amp, mains[b]->amp); }
                break;
            case OP_INVERT_ACCENTS: {
                if(n < 2) break;
                float lo = 1.0f, hi = 0.0f;
                for(auto* it : mains) { lo = std::min(lo, it->amp); hi = std::max(hi, it->amp); }
                for(auto* it : mains) it->amp = lo + hi - it->amp;
                break;
            }
            case OP_PROMOTE: {
                Item* softest = nullptr;
                for(auto& it : items) if(!softest || it.amp < softest->amp) softest = &it;
                if(softest) softest->amp = std::max(softest->amp, L.level * 0.75f);
                break;
            }
            default: break;
        }
    }
}

// ── A cycle ─────────────────────────────────────────────────────────────────

std::vector<Engine::Item> Engine::placeItems(int lane, int64_t cycle) const {
    const Layer& L = layers[lane];
    const int cT = L.cycleTicks();
    const int64_t from = cycle * cT;
    const Material m = material(lane, cycle);

    auto byTick = [](const Item& a, const Item& b) { return a.tick != b.tick ? a.tick < b.tick : a.strength > b.strength; };
    auto sameTick = [](const Item& a, const Item& b) { return a.tick == b.tick; };

    // 1. Own onsets, displaced, plus the ones pushed in from the next cycle or
    //    dragged in from the previous one. Strength (and so amplitude) is the
    //    one of the position the onset was generated on: an anticipated
    //    downbeat keeps its accent.
    std::vector<Item> mains;
    for(int t : rawCycle(lane, cycle, m)) {
        const int d = displaced(lane, cycle, t, m);
        if(d >= 0 && d < cT) mains.push_back({d, strengthAt(lane, cycle, t), KIND_MAIN, 1.0f});
    }
    if(L.push > 0.0f && L.crossBarline > 0.0f) {
        const Material mn = material(lane, cycle + 1);
        for(int t : rawCycle(lane, cycle + 1, mn)) {
            const int d = displaced(lane, cycle + 1, t, mn);
            if(d < 0) mains.push_back({cT + d, strengthAt(lane, cycle + 1, t), KIND_MAIN, 1.0f});
        }
    }
    if(L.drag > 0.0f && L.crossBarline > 0.0f) {
        const Material mp = material(lane, cycle - 1);
        for(int t : rawCycle(lane, cycle - 1, mp)) {
            const int d = displaced(lane, cycle - 1, t, mp);
            if(d >= cT) mains.push_back({d - cT, strengthAt(lane, cycle - 1, t), KIND_MAIN, 1.0f});
        }
    }
    std::sort(mains.begin(), mains.end(), byTick);
    mains.erase(std::unique(mains.begin(), mains.end(), sameTick), mains.end());

    // 2. Relation to a lane above
    const int rel = L.relMode != REL_NONE ? validLane(lane, L.relLane) : -1;
    if(rel >= 0) {
        auto roll = [&](int t, uint64_t salt) { return unit(hashOf(m.mat, (uint64_t)t, salt)) < L.relAmount; };
        std::vector<Item> kept;
        if(L.relMode == REL_RESPONSE) {
            // Answer what the other lane played respDelay earlier
            for(const auto& o : laneOnsets(rel, from - L.respDelay, from + cT - L.respDelay)) {
                const int64_t t = o.tick + L.respDelay - from;
                if(t >= 0 && t < cT && roll((int)t, SALT_REL)) kept.push_back({(int)t, strengthAt(lane, cycle, (int)t), KIND_MAIN, 1.0f});
            }
            mains = kept;
        } else {
            const auto other = laneOnsets(rel, from, from + cT);
            std::vector<int> ticks;
            for(const auto& o : other) if(o.tick >= from) ticks.push_back((int)(o.tick - from));
            auto has = [&](int t) { return std::binary_search(ticks.begin(), ticks.end(), t); };
            if(L.relMode == REL_FOLLOW || L.relMode == REL_AVOID) {
                for(const auto& it : mains) {
                    const bool coincide = has(it.tick);
                    const bool drop = (L.relMode == REL_FOLLOW ? !coincide : coincide) && roll(it.tick, SALT_REL);
                    if(!drop) kept.push_back(it);
                }
                mains = kept;
            } else if(L.relMode == REL_UNISON) {
                for(int t : ticks)
                    if(roll(t, SALT_REL)) mains.push_back({t, strengthAt(lane, cycle, t), KIND_MAIN, 1.0f});
            } else if(L.relMode == REL_COMPLEMENT) {
                // Out of the other lane's hits, and into its silences (no
                // gate sounding) on this lane's step
                for(const auto& it : mains) if(!has(it.tick)) kept.push_back(it);
                mains = kept;
                const int step = L.generator == GEN_WEIGHTED ? 12 : std::max(1, L.division);
                for(int p = 0; p < cT; p += step) {
                    bool busy = false;
                    for(const auto& o : other) {
                        const int64_t at = o.tick - from;
                        if(at <= p && p < at + o.dur) { busy = true; break; }
                    }
                    if(!busy && roll(p, SALT_FILL)) mains.push_back({p, strengthAt(lane, cycle, p), KIND_MAIN, 1.0f});
                }
            }
        }
        std::sort(mains.begin(), mains.end(), byTick);
        mains.erase(std::unique(mains.begin(), mains.end(), sameTick), mains.end());
    }

    // 3. Genome (placement and duration operators)
    applyPlacementOps(lane, cycle, m, mains);

    // 4. Ornaments
    std::vector<Item> all = mains;
    for(size_t j = 0; j < mains.size(); j++) {
        Item& it = all[j];
        const int t = it.tick;
        const int prev = j > 0 ? mains[j - 1].tick : -1;
        const int next = j + 1 < mains.size() ? mains[j + 1].tick : cT;
        float ratchet = L.ratchet;
        if(inFill(lane, cycle, t)) ratchet += L.fillIntensity * 0.5f * (1.0f - ratchet);
        if(ratchet > 0.0f && unit(hashOf(m.mat, (uint64_t)t, SALT_RATCHET)) < ratchet) {
            const int n = std::max(2, L.ratchetCount);
            auto factor = [&](int r) {
                const float x = (float)r / (n - 1);
                return L.ratchetShape == 1 ? 0.4f + 0.6f * x : L.ratchetShape == 2 ? 1.0f - 0.6f * x : 1.0f;
            };
            it.factor = factor(0);
            for(int r = 1; r < n; r++) {
                const int rt = t + r * std::max(1, L.ratchetUnit);
                if(rt >= next || rt >= cT) break;
                all.push_back({rt, it.strength, KIND_RATCHET, factor(r)});
            }
        }
        if(L.flam > 0.0f && unit(hashOf(m.mat, (uint64_t)t, SALT_FLAM)) < L.flam) {
            const int ft = t - std::max(1, L.flamTicks);
            if(ft > prev && ft >= 0) all.push_back({ft, it.strength, KIND_FLAM, 0.5f});
        }
        if(L.ghost > 0.0f && unit(hashOf(m.mat, (uint64_t)t, SALT_GHOST)) < L.ghost) {
            int side = L.ghostSide;
            if(side == 2) side = unit(hashOf(m.mat, (uint64_t)t, SALT_GHOSTSIDE)) < 0.5f ? 0 : 1;
            const int gt = side == 0 ? t - 12 : t + 12;
            if(gt > prev && gt < next && gt >= 0 && gt < cT) all.push_back({gt, strengthAt(lane, cycle, gt), KIND_GHOST, 1.0f});
        }
    }
    std::sort(all.begin(), all.end(), [](const Item& a, const Item& b) { return a.tick != b.tick ? a.tick < b.tick : a.kind < b.kind; });
    all.erase(std::unique(all.begin(), all.end(), sameTick), all.end());

    // 5. Amplitudes
    const int n = std::max(1, L.phraseLen);
    const bool phraseStart = positiveMod64(cycle, n) == 0;
    for(auto& it : all) {
        float amp = it.kind == KIND_GHOST ? L.ghostLevel * L.level : amplitudeOf(lane, m, it.tick, it.strength) * it.factor;
        const float progress = phraseProgress(lane, cycle, it.tick);
        amp *= 1.0f - L.swell * (1.0f - progress);
        if(phraseStart && it.tick == 0 && it.kind == KIND_MAIN) amp += (L.level - amp) * L.phraseAccent;
        if(inFill(lane, cycle, it.tick)) {
            const float fp = (float)(it.tick - (cT - L.fillTicks)) / (float)std::max(1, L.fillTicks);
            amp *= 1.0f - 0.4f * L.fillIntensity * (1.0f - fp);
        }
        it.amp = amp;
    }
    // Euclidean accents and long notes, spread over the cycle's hits
    {
        std::vector<Item*> hits;
        for(auto& it : all) if(it.kind == KIND_MAIN) hits.push_back(&it);
        const int count = (int)hits.size();
        auto euclid = [&](int k, int i) { return k > 0 && count > 0 && ((int64_t)i * std::min(k, count)) % count < std::min(k, count); };
        for(int i = 0; i < count; i++) {
            const bool accent = euclid(L.accentHits, i);
            if(L.accentHits > 0) {
                if(accent) hits[i]->amp += (L.level - hits[i]->amp) * L.accentAmount;
                else hits[i]->amp *= 1.0f - 0.5f * L.accentAmount;
            }
            hits[i]->longNote = L.accentsLong && L.accentHits > 0 ? accent : euclid(L.longHits, i);
        }
    }
    applyAmplitudeOps(lane, m, all);
    // Duck / reinforce against a lane above
    const int dyn = L.dynMode != DYN_NONE ? validLane(lane, L.dynLane) : -1;
    if(dyn >= 0) {
        std::vector<int> ticks;
        for(const auto& o : laneOnsets(dyn, from, from + cT)) if(o.tick >= from) ticks.push_back((int)(o.tick - from));
        for(auto& it : all)
            if(std::binary_search(ticks.begin(), ticks.end(), it.tick))
                it.amp = L.dynMode == DYN_DUCK ? it.amp * (1.0f - L.dynAmount) : it.amp * (1.0f + L.dynAmount);
    }
    for(auto& it : all) it.amp = clampf(it.amp, 0.001f, 1.0f);

    // 6. Groove: swing / laid back / pushed, and humanize
    if(L.groove != GROOVE_STRAIGHT || L.humanize > 0) {
        for(auto& it : all) {
            int off = 0;
            const int t = it.tick;
            switch(L.groove) {
                case GROOVE_SWING16: if(t % 24 == 12) off = (int)std::lround(24.0f * L.swing) - 12; break;
                case GROOVE_SWING8:  if(t % 48 == 24) off = (int)std::lround(48.0f * L.swing) - 24; break;
                case GROOVE_LAID_BACK: off = t % TPB == 0 ? 1 : 2; break;
                case GROOVE_PUSHED: if(t % TPB != 0) off = -2; break;
                default: break;
            }
            if(L.humanize > 0)
                off += (int)std::lround((unit(hashOf(m.mat, (uint64_t)t, SALT_HUMAN)) * 2.0f - 1.0f) * L.humanize);
            it.tick = std::clamp(t + off, 0, cT - 1);
        }
        std::stable_sort(all.begin(), all.end(), [](const Item& a, const Item& b) { return a.tick != b.tick ? a.tick < b.tick : a.kind < b.kind; });
        all.erase(std::unique(all.begin(), all.end(), sameTick), all.end());
    }
    return all;
}

std::vector<Onset> Engine::realizeCycle(int lane, int64_t cycle) const {
    auto& c = cache[lane];
    auto found = c.find(cycle);
    if(found != c.end()) return found->second;

    const Layer& L = layers[lane];
    const int cT = L.cycleTicks();
    const std::vector<Item> items = placeItems(lane, cycle);

    // Gap to the next onset, looking into the next cycle for the last one
    int nextFirst = cT;
    if(!items.empty()) {
        const auto next = placeItems(lane, cycle + 1);
        if(!next.empty()) nextFirst = next.front().tick;
    }
    // Choke: the next hit of the choking lane ends the gate
    std::vector<AbsOnset> choke;
    const int chokeLane = validLane(lane, L.chokeLane);
    if(chokeLane >= 0 && !items.empty()) choke = laneOnsets(chokeLane, cycle * cT, cycle * cT + cT + 4 * TPB);

    const Material m = material(lane, cycle);
    std::vector<Onset> out;
    out.reserve(items.size());
    for(size_t j = 0; j < items.size(); j++) {
        const Item& it = items[j];
        const int nextTick = j + 1 < items.size() ? items[j + 1].tick : cT + nextFirst;
        const int gap = std::max(1, nextTick - it.tick);
        int dur = L.durTicks;
        if(L.durMode == DUR_LEGATO) dur = (int)std::lround(L.legato * gap);
        else if(L.durMode == DUR_METRIC) dur = (int)std::lround(L.durShort + (L.durLong - L.durShort) * it.strength);
        if(it.longNote) dur = L.longTicks;
        if(it.tied) dur = gap;
        dur = (int)std::lround(dur * it.durScale);
        // The gate falls at least a tick before the next onset of its lane,
        // so a receiver always sees a fresh rising edge.
        dur = std::clamp(dur, 1, std::max(1, gap - 1));
        // Barline: a gate crosses the cycle end only with that chance
        if(it.tick + dur > cT && unit(hashOf(m.mat, (uint64_t)it.tick, SALT_CROSSDUR)) >= L.crossBarline)
            dur = std::max(1, cT - it.tick);
        if(!choke.empty()) {
            const int64_t at = cycle * cT + it.tick;
            for(const auto& ch : choke)
                if(ch.tick > at) { dur = std::max(1, std::min(dur, (int)(ch.tick - at))); break; }
        }
        Onset o;
        o.tick = it.tick;
        o.dur = dur;
        o.amp = it.amp;
        o.strength = it.strength;
        o.kind = it.kind;
        out.push_back(o);
    }
    if(c.size() > 512) c.clear();
    c.emplace(cycle, out);
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
// Evolution
// ════════════════════════════════════════════════════════════════════════════

// Before a genome change: remember the current genome for undo and for the
// cycles until nowCycle (so the change lands on the next cycle, never in the
// middle of one), and the current hits of the next cycles for the highlight.
void Engine::beginChange(int lane, int64_t nowCycle) {
    LaneState& st = state[lane];
    st.before = layers[lane].genome();
    st.hasBefore = true;
    st.changeFrom = nowCycle + 1;
    st.oldTicks.clear();
    const int n = std::max(1, layers[lane].phraseLen);
    for(int64_t c = st.changeFrom; c < st.changeFrom + std::max(2, n); c++) {
        std::vector<int> ticks;
        for(const auto& o : realizeCycle(lane, c)) ticks.push_back(o.tick);
        st.oldTicks[c] = ticks;
    }
}

void Engine::setGenome(int lane, const Genome& g) {
    layers[lane].seed = g.seed;
    layers[lane].ops = g.ops;
    changed(lane);
}

void Engine::mutate(int lane, int64_t nowCycle, uint32_t randomSeed) {
    Layer& L = layers[lane];
    if(L.keep) return;
    LaneState& st = state[lane];
    st.undo.push_back(L.genome());
    if(st.undo.size() > 64) st.undo.erase(st.undo.begin());
    st.redo.clear();
    beginChange(lane, nowCycle);
    Genome g = L.genome();
    Stream s{hashOf(randomSeed, (uint64_t)g.ops.size(), (uint64_t)lane, SALT_OPS)};
    for(int i = 0; i < std::max(1, L.mutations); i++) {
        Op op;
        op.type = s.pick(OP_RANDOM_COUNT);
        op.seed = (uint32_t)(s.next() * 4294967295.0f) ^ (uint32_t)i;
        g.ops.push_back(op);
    }
    // Only the latest mutations stay: an evolving loop slowly forgets its
    // oldest changes instead of piling them up
    if((int)g.ops.size() > MAX_OPS) g.ops.erase(g.ops.begin(), g.ops.end() - MAX_OPS);
    setGenome(lane, g);
}

void Engine::reroll(int lane, int64_t nowCycle, int newSeed) {
    Layer& L = layers[lane];
    if(L.keep) return;
    LaneState& st = state[lane];
    st.undo.push_back(L.genome());
    if(st.undo.size() > 64) st.undo.erase(st.undo.begin());
    st.redo.clear();
    beginChange(lane, nowCycle);
    setGenome(lane, {newSeed, {}});
}

bool Engine::undo(int lane, int64_t nowCycle) {
    LaneState& st = state[lane];
    if(st.undo.empty()) return false;
    const Genome target = st.undo.back();
    st.undo.pop_back();
    st.redo.push_back(layers[lane].genome());
    beginChange(lane, nowCycle);
    setGenome(lane, target);
    return true;
}

bool Engine::redo(int lane, int64_t nowCycle) {
    LaneState& st = state[lane];
    if(st.redo.empty()) return false;
    const Genome target = st.redo.back();
    st.redo.pop_back();
    st.undo.push_back(layers[lane].genome());
    beginChange(lane, nowCycle);
    setGenome(lane, target);
    return true;
}

void Engine::like(int lane) {
    Layer& L = layers[lane];
    const Genome g = L.genome();
    for(const auto& l : L.liked) if(l == g) return;
    L.liked.push_back(g);
    if(L.liked.size() > 16) L.liked.erase(L.liked.begin());
}

// A new genome from two liked ones: A's seed and operators, then B's second
// half (a splice from B's seed) and B's operators.
void Engine::crossover(int lane, int a, int b, int64_t nowCycle) {
    Layer& L = layers[lane];
    if(L.keep || a < 0 || b < 0 || a >= (int)L.liked.size() || b >= (int)L.liked.size()) return;
    const Genome A = L.liked[a], B = L.liked[b];
    Genome child = A;
    Op splice;
    splice.type = OP_SPLICE;
    splice.seed = (uint32_t)hashOf((uint64_t)A.seed, (uint64_t)B.seed, A.ops.size(), B.ops.size());
    splice.aux = (uint32_t)B.seed;
    child.ops.push_back(splice);
    child.ops.insert(child.ops.end(), B.ops.begin(), B.ops.end());
    LaneState& st = state[lane];
    st.undo.push_back(L.genome());
    st.redo.clear();
    beginChange(lane, nowCycle);
    setGenome(lane, child);
}

void Engine::setKeep(int lane, bool on, int64_t nowCycle) {
    Layer& L = layers[lane];
    // Freeze the material of the phrase playing now (unless a loop already did)
    if(on && !frozen(lane)) L.keepPhrase = materialPhrase(lane, nowCycle, material(lane, nowCycle).key);
    L.keep = on;
    changed(lane);
}

void Engine::setRepeat(int lane, int mode, int64_t nowCycle) {
    Layer& L = layers[lane];
    mode = std::clamp(mode, (int)REPEAT_OFF, (int)REPEAT_EVOLVE);
    if(mode != REPEAT_OFF && !frozen(lane)) L.keepPhrase = materialPhrase(lane, nowCycle, material(lane, nowCycle).key);
    L.repeat = mode;
    if(mode == REPEAT_EVOLVE) L.autoEvery = std::max(1, L.autoEvery);
    changed(lane);
}

std::vector<int> Engine::changedTicks(int lane, int64_t cycle) const {
    const LaneState& st = state[lane];
    auto it = st.oldTicks.find(cycle);
    if(it == st.oldTicks.end()) return {};
    std::vector<int> out;
    for(const auto& o : realizeCycle(lane, cycle))
        if(std::find(it->second.begin(), it->second.end(), o.tick) == it->second.end()) out.push_back(o.tick);
    return out;
}

void Engine::forgetHighlightsBefore(int lane, int64_t cycle) {
    auto& old = state[lane].oldTicks;
    for(auto it = old.begin(); it != old.end();) it = it->first < cycle ? old.erase(it) : std::next(it);
}

std::string Engine::captureExamples(int sourceLane, int64_t fromCycle, int cycles) const {
    std::string out;
    const int cT = layers[sourceLane].cycleTicks();
    const int steps = cT / 12;
    for(int64_t c = fromCycle; c < fromCycle + cycles; c++) {
        std::string word(steps, '.');
        for(const auto& o : realizeCycle(sourceLane, c)) {
            if(o.kind != KIND_MAIN) continue;
            const int i = (int)std::lround(o.tick / 12.0);
            if(i >= 0 && i < steps) word[i] = 'x';
        }
        if(!out.empty()) out += ' ';
        out += word;
    }
    return out;
}

} // namespace metrobox
