//
//  scMetroBoxEngine.cpp
//  ofxOceanodeSuperCollider
//

#include "scMetroBoxEngine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>

namespace metrobox {

namespace {

constexpr int TPB = TICKS_PER_BEAT;
constexpr int SIXTEENTH = TPB / 4;
constexpr int RATCHET_HITS = 3;
constexpr float GHOST_LEVEL = 0.3f;
constexpr int FLAM_TICKS = 2;

// Salts: each kind of decision draws from its own hash, so changing one
// setting (chance, say) does not reshuffle the others.
enum Salt : uint64_t {
    SALT_KEEP = 1, SALT_AMP, SALT_SUBDIV, SALT_DISP, SALT_REL, SALT_GHOST, SALT_FLAM, SALT_RATCHET,
    SALT_PHRASE, SALT_MATERIAL, SALT_FILLX, SALT_HUMAN, SALT_REROLL, SALT_OPS
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
        if(ops[i].type != o.ops[i].type || ops[i].seed != o.ops[i].seed) return false;
    return true;
}

// ── Change ──────────────────────────────────────────────────────────────────

int changeZone(float c) {
    if(c < 0.15f) return CHANGE_FIXED;
    if(c < 0.40f) return CHANGE_LOOP;
    if(c < 0.65f) return CHANGE_EVOLVING;
    if(c < 0.90f) return CHANGE_VARYING;
    return CHANGE_NEW;
}

bool changeFrozen(float c) { return changeZone(c) <= CHANGE_EVOLVING; }

int evolveEvery(float c) {
    const float t = clampf((c - 0.40f) / 0.25f, 0.0f, 1.0f);
    return std::clamp((int)std::lround(4.0f - 3.0f * t), 1, 4);
}

float renewChance(float c) {
    switch(changeZone(c)) {
        case CHANGE_VARYING: return 0.15f + 0.6f * clampf((c - 0.65f) / 0.25f, 0.0f, 1.0f);
        case CHANGE_NEW:     return 1.0f;
        default:             return 0.0f;
    }
}

int Layer::fillTicks() const {
    if(fill <= 0.0f) return 0;
    return std::min(fill < 0.5f ? TPB : 2 * TPB, cycleTicks());
}

// ════════════════════════════════════════════════════════════════════════════
// Engine
// ════════════════════════════════════════════════════════════════════════════

Engine::Engine() {
    for(int i = 0; i < MAX_LAYERS; i++) layers[i].name = "Lane " + std::to_string(i + 1);
    for(auto& l : links) l.fill(-1);
    sessionSalt = ((uint64_t)std::random_device{}() << 32) ^ std::random_device{}();
}

// The lane and every lane that hears it (if the links themselves changed:
// every lane)
void Engine::changed(int lane) {
    if(lane < 0 || lane >= MAX_LAYERS) return;
    const auto before = links;
    const bool wasDirty = linksDirty;
    linksDirty = true;
    computeLinks();
    std::vector<int> lanes;
    if(wasDirty || before != links) for(int j = 0; j < MAX_LAYERS; j++) lanes.push_back(j);
    else { lanes = dependents(lane); lanes.push_back(lane); }
    for(int j : lanes) {
        cache[j].clear();
        if(onChanged) onChanged(j);
    }
}

void Engine::changedAll() {
    linksDirty = true;
    computeLinks();
    for(int j = 0; j < MAX_LAYERS; j++) {
        cache[j].clear();
        if(onChanged) onChanged(j);
    }
}

// ── Links between lanes ─────────────────────────────────────────────────────

int Engine::linkSetting(int lane, int kind) const {
    const Layer& L = layers[lane];
    switch(kind) {
        case LINK_REL:   return L.relMode != REL_NONE ? L.relLane : 0;
        case LINK_CHOKE: return L.chokeLane;
        default:         return L.dynMode != DYN_NONE ? L.dynLane : 0;
    }
}

bool Engine::reaches(int from, int to) const {
    std::vector<int> stack = {from};
    std::array<bool, MAX_LAYERS> seen{};
    while(!stack.empty()) {
        const int l = stack.back();
        stack.pop_back();
        if(l == to) return true;
        if(l < 0 || l >= MAX_LAYERS || seen[l]) continue;
        seen[l] = true;
        for(int k = 0; k < LINK_COUNT; k++) if(links[l][k] >= 0) stack.push_back(links[l][k]);
    }
    return false;
}

void Engine::computeLinks() const {
    if(!linksDirty) return;
    for(auto& l : links) l.fill(-1);
    for(int lane = 0; lane < numLayers && lane < MAX_LAYERS; lane++)
        for(int k = 0; k < LINK_COUNT; k++) {
            const int other = linkSetting(lane, k) - 1;
            if(other < 0 || other == lane || other >= numLayers) continue;
            if(reaches(other, lane)) continue;   // it would loop
            links[lane][k] = other;
        }
    linksDirty = false;
}

int Engine::link(int lane, int kind) const {
    computeLinks();
    return lane >= 0 && lane < MAX_LAYERS ? links[lane][kind] : -1;
}

bool Engine::wouldLoop(int lane, int otherLane) const {
    computeLinks();
    if(otherLane == lane) return true;
    // the lane's own links don't count: they are what would be replaced
    auto saved = links[lane];
    links[lane].fill(-1);
    const bool loop = reaches(otherLane, lane);
    links[lane] = saved;
    return loop;
}

std::vector<int> Engine::dependents(int lane) const {
    computeLinks();
    std::vector<int> out;
    for(int j = 0; j < MAX_LAYERS; j++) if(j != lane && reaches(j, lane)) out.push_back(j);
    return out;
}

void Engine::moveLane(int from, int to) {
    if(from < 0 || from >= MAX_LAYERS || to < 0 || to >= MAX_LAYERS || from == to) return;
    // order[new position] = old position
    std::array<int, MAX_LAYERS> order, newOf;
    for(int i = 0; i < MAX_LAYERS; i++) order[i] = i;
    const int moved = order[from];
    if(from < to) std::rotate(order.begin() + from, order.begin() + from + 1, order.begin() + to + 1);
    else std::rotate(order.begin() + to, order.begin() + from, order.begin() + from + 1);
    order[to] = moved;
    for(int i = 0; i < MAX_LAYERS; i++) newOf[order[i]] = i;
    // Every lane keeps the random stream it had at its old position
    for(int i = 0; i < MAX_LAYERS; i++) if(layers[i].id <= 0) layers[i].id = i + 1;
    auto oldLayers = layers;
    auto oldState = state;
    auto oldPasses = passes;
    for(int i = 0; i < MAX_LAYERS; i++) { layers[i] = oldLayers[order[i]]; state[i] = oldState[order[i]]; passes[i] = oldPasses[order[i]]; }
    auto remap = [&](int& n) { if(n >= 1 && n <= MAX_LAYERS) n = newOf[n - 1] + 1; };
    for(auto& L : layers) { remap(L.relLane); remap(L.chokeLane); remap(L.dynLane); }
    changedAll();
}

float Engine::effectiveDensity(int lane) const {
    float scale = 1.0f;
    if(densityScale.size() == 1) scale = densityScale[0];
    else if(lane < (int)densityScale.size()) scale = densityScale[lane];
    return clampf(layers[lane].density * scale, 0.0f, 1.0f);
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
    const int f = L.fillTicks();
    return f > 0 && positiveMod64(cycle, n) == n - 1 && tick >= L.cycleTicks() - f;
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

// The cycle's own hierarchy, and the main bar's at the same absolute
// position: a 5-beat lane still leans on the bar's downbeat when it lands there.
float Engine::strengthAt(int lane, int64_t cycle, int tick) const {
    const Layer& L = layers[lane];
    float s = levelStrength(tick, groupStarts(lane));
    const int barTicks = std::max(1, (int)std::lround(mainBarBeats * TPB));
    const int barBeats = (int)std::lround(mainBarBeats);
    const std::vector<int> barGroups = barBeats * TPB == barTicks ? groupTicksOf(autoGroups(barBeats)) : std::vector<int>{0};
    const int64_t absTick = cycle * L.cycleTicks() + tick;
    return std::max(s, levelStrength((int)positiveMod64(absTick, barTicks), barGroups));
}

// ── Material ────────────────────────────────────────────────────────────────

Genome Engine::genomeAt(int lane, int64_t cycle) const {
    const LaneState& st = state[lane];
    if(st.hasBefore && cycle < st.changeFrom) return st.before;
    return layers[lane].genome();
}

// Which random stream a cycle uses: its phrase's (see materialPhrase), and
// its place in the phrase.
Engine::Material Engine::material(int lane, int64_t cycle) const {
    const Layer& L = layers[lane];
    Material m;
    m.genome = genomeAt(lane, cycle);
    const uint64_t id = L.id > 0 ? (uint64_t)L.id : (uint64_t)lane + 1;
    m.key = hashOf(unseeded() ? sessionSalt : (uint64_t)(uint32_t)globalSeed, (uint64_t)(uint32_t)m.genome.seed, id);
    const int64_t q = positiveMod64(cycle, std::max(1, L.phraseLen));
    const int64_t pm = materialPhrase(lane, cycle, m.key);
    const uint64_t pass = changeFrozen(L.change) ? 0 : (uint64_t)passOf(lane, pm);
    m.mat = hashOf(m.key ^ (pass * 0x9E3779B97F4A7C15ull), (uint64_t)pm, (uint64_t)q, SALT_MATERIAL);
    return m;
}

// The phrase whose material a cycle plays: the kept one while Change is
// frozen, otherwise the last phrase that renewed.
int64_t Engine::materialPhrase(int lane, int64_t cycle, uint64_t key) const {
    const Layer& L = layers[lane];
    if(changeFrozen(L.change)) return L.keepPhrase;
    const int64_t p = floorDiv(cycle, std::max(1, L.phraseLen));
    const float renew = renewChance(L.change);
    if(renew >= 1.0f || p <= 0) return p;
    for(int64_t k = p; k >= 1 && k > p - 256; k--)
        if(unit(hashOf(key ^ ((uint64_t)passOf(lane, k) * 0x9E3779B97F4A7C15ull), (uint64_t)k, SALT_PHRASE)) < renew) return k;
    return 0;
}

int Engine::passOf(int lane, int64_t phrase) const {
    if(!unseeded()) return 0;
    const auto it = passes[lane].find(phrase);
    return it == passes[lane].end() ? 0 : it->second;
}

void Engine::leavePhrase(int lane, int64_t phrase) {
    if(!unseeded() || lane < 0 || lane >= MAX_LAYERS || changeFrozen(layers[lane].change)) return;
    passes[lane][phrase]++;
    if(passes[lane].size() > 4096) passes[lane].clear();
    changed(lane);
}

void Engine::setChange(int lane, float change, int64_t nowCycle) {
    Layer& L = layers[lane];
    change = clampf(change, 0.0f, 1.0f);
    // Freeze the phrase playing now (unless it is frozen already)
    if(changeFrozen(change) && !changeFrozen(L.change))
        L.keepPhrase = materialPhrase(lane, nowCycle, material(lane, nowCycle).key);
    L.change = change;
    changed(lane);
}

// ── Pattern ─────────────────────────────────────────────────────────────────

// The pattern's onsets (offset), kept by Chance, Smart, build-up and fills.
// A tick is kept with a hash of (material, tick) against its chance, so
// raising Chance only ever adds hits and lowering it only removes them.
std::vector<int> Engine::rawCycle(int lane, int64_t cycle, const Material& m) const {
    const Layer& L = layers[lane];
    const int cT = L.cycleTicks();
    const float density = effectiveDensity(lane);
    const float fillAmount = L.fill;
    std::vector<int> gen;

    if(L.pattern == PAT_FREE) {
        // Each group is a segment; a segment splits in two (or three) with a
        // chance that falls on strong positions (Smart) and rises in a fill,
        // down to the Every step. Every segment that stays whole is an onset.
        const int finest = std::max(1, L.division);
        const std::vector<int> groups = groupStarts(lane);
        Stream stream{hashOf(m.mat, SALT_SUBDIV)};
        std::function<void(int, int)> split = [&](int start, int len) {
            const bool two = len % 2 == 0 && len / 2 >= finest;
            const bool three = len % 3 == 0 && len / 3 >= finest;
            const int at = (int)positiveMod64((int64_t)start + L.shift, cT);
            const float s = strengthAt(lane, cycle, at);
            float chance = L.complexity * (1.0f - L.smart * s);
            if(inFill(lane, cycle, at)) chance += fillAmount * (1.0f - chance);
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
        const int step = std::max(1, L.division);
        const int n = std::max(1, L.eucSteps);
        const int k = std::clamp(L.eucHits, 0, n);
        int i = 0;
        for(int t = 0; t < cT; t += step, i++) {
            bool hit = true;
            if(L.pattern == PAT_EUCLIDEAN) {
                const int64_t s = positiveMod64((int64_t)i + L.eucRotation, n);
                hit = (s * k) % n < k;
            }
            if(hit) gen.push_back(t);
        }
    }

    // Smart: the chance is bent per position, density ^ w, w below 1 on
    // strong positions and above 1 on weak ones. 100% still plays every hit
    // and 0% none; in between the weak ones go first. The cycle and group
    // starts are protected too.
    const float protect = L.smart * std::min(1.0f, density * 4.0f);
    std::vector<int> out;
    out.reserve(gen.size() + 16);
    for(int t : gen) {
        const int st = (int)positiveMod64((int64_t)t + L.shift, cT);
        const float s = strengthAt(lane, cycle, st);
        const float build = 1.0f - L.buildUp * (1.0f - phraseProgress(lane, cycle, st));
        float chance = std::pow(density, std::exp(L.smart * 2.5f * (0.5f - s))) * build;
        if(s >= 0.8f) chance = std::max(chance, protect);
        if(inFill(lane, cycle, st)) chance += fillAmount * (1.0f - chance);
        if(unit(hashOf(m.mat, (uint64_t)st, SALT_KEEP)) < chance) out.push_back(st);
    }

    // A fill also brings its own 16ths, whatever the pattern
    const int fillLen = L.fillTicks();
    if(fillLen > 0) {
        const int n = std::max(1, L.phraseLen);
        if(positiveMod64(cycle, n) == n - 1) {
            const int start = ((cT - fillLen + SIXTEENTH - 1) / SIXTEENTH) * SIXTEENTH;
            for(int t = std::max(0, start); t < cT; t += SIXTEENTH)
                if(unit(hashOf(m.mat, (uint64_t)t, SALT_FILLX)) < fillAmount * 0.7f) out.push_back(t);
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Push (anticipation) or drag of an onset on a beat by a 16th. A move across
// the cycle start lands in the neighbouring cycle.
int Engine::displaced(int lane, int tick, const Material& m) const {
    const Layer& L = layers[lane];
    if(tick % TPB != 0 || (L.push <= 0.0f && L.drag <= 0.0f)) return tick;
    const float r = unit(hashOf(m.mat, (uint64_t)tick, SALT_DISP));
    if(r < L.push) return tick - SIXTEENTH;
    if(r < L.push + L.drag) return tick + SIXTEENTH;
    return tick;
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

    for(const Op& op : m.genome.ops) {
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
                if(gap < 2 * SIXTEENTH) break;
                const int t = items[j].tick + std::max(SIXTEENTH, (gap / 2) / SIXTEENTH * SIXTEENTH);
                if(t < next && t < cT && !occupied(t)) items.push_back({t, strengthAt(lane, cycle, t), KIND_MAIN, 1.0f});
                break;
            }
            case OP_MERGE:
                if(n > 1) items.erase(items.begin() + 1 + s.pick(n - 1));
                break;
            case OP_NUDGE: {
                if(n == 0) break;
                Item& it = items[s.pick(n)];
                const int t = it.tick + (s.next() < 0.5f ? -SIXTEENTH : SIXTEENTH);
                if(t >= 0 && t < cT && !occupied(t)) it.tick = t;
                break;
            }
            case OP_ROTATE: {
                const int d = s.next() < 0.5f ? -SIXTEENTH : SIXTEENTH;
                for(auto& it : items) it.tick = (int)positiveMod64((int64_t)it.tick + d, cT);
                break;
            }
            case OP_REROLL_HALF: {
                const int h = s.pick(2);
                const int lo = h * cT / 2, hi = (h + 1) * cT / 2;
                Material alt = m;
                alt.mat = hashOf(m.mat, (uint64_t)op.seed, SALT_REROLL);
                items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& it) { return it.tick >= lo && it.tick < hi; }), items.end());
                for(int t : rawCycle(lane, cycle, alt))
                    if(t >= lo && t < hi) items.push_back({t, strengthAt(lane, cycle, t), KIND_MAIN, 1.0f});
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
    for(const Op& op : m.genome.ops) {
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
    //    dragged in from the previous one. Strength (and so the accent) is
    //    the one of the position the onset was generated on: an anticipated
    //    downbeat keeps its accent.
    std::vector<Item> mains;
    for(int t : rawCycle(lane, cycle, m)) {
        const int d = displaced(lane, t, m);
        if(d >= 0 && d < cT) mains.push_back({d, strengthAt(lane, cycle, t), KIND_MAIN, 1.0f});
    }
    if(L.push > 0.0f) {
        const Material mn = material(lane, cycle + 1);
        for(int t : rawCycle(lane, cycle + 1, mn)) {
            const int d = displaced(lane, t, mn);
            if(d < 0) mains.push_back({cT + d, strengthAt(lane, cycle + 1, t), KIND_MAIN, 1.0f});
        }
    }
    if(L.drag > 0.0f) {
        const Material mp = material(lane, cycle - 1);
        for(int t : rawCycle(lane, cycle - 1, mp)) {
            const int d = displaced(lane, t, mp);
            if(d >= cT) mains.push_back({d - cT, strengthAt(lane, cycle - 1, t), KIND_MAIN, 1.0f});
        }
    }
    std::sort(mains.begin(), mains.end(), byTick);
    mains.erase(std::unique(mains.begin(), mains.end(), sameTick), mains.end());

    // 2. Relation to a lane above
    const int rel = link(lane, LINK_REL);
    if(rel >= 0) {
        auto roll = [&](int t) { return unit(hashOf(m.mat, (uint64_t)t, SALT_REL)) < L.relAmount; };
        std::vector<Item> kept;
        if(L.relMode == REL_ANSWER) {
            // Play what the other lane played respDelay earlier
            for(const auto& o : laneOnsets(rel, from - L.respDelay, from + cT - L.respDelay)) {
                const int64_t t = o.tick + L.respDelay - from;
                if(t >= 0 && t < cT && roll((int)t)) kept.push_back({(int)t, strengthAt(lane, cycle, (int)t), KIND_MAIN, 1.0f});
            }
        } else {
            std::vector<int> ticks;
            for(const auto& o : laneOnsets(rel, from, from + cT)) if(o.tick >= from) ticks.push_back((int)(o.tick - from));
            for(const auto& it : mains) {
                const bool coincide = std::binary_search(ticks.begin(), ticks.end(), it.tick);
                const bool drop = (L.relMode == REL_FOLLOW ? !coincide : coincide) && roll(it.tick);
                if(!drop) kept.push_back(it);
            }
        }
        mains = kept;
        std::sort(mains.begin(), mains.end(), byTick);
        mains.erase(std::unique(mains.begin(), mains.end(), sameTick), mains.end());
    }

    // 3. Genome (placement and duration operators)
    applyPlacementOps(lane, cycle, m, mains);

    // 4. Ornaments
    std::vector<Item> all = mains;
    for(size_t j = 0; j < mains.size(); j++) {
        const int t = all[j].tick;
        const int prev = j > 0 ? mains[j - 1].tick : -1;
        const int next = j + 1 < mains.size() ? mains[j + 1].tick : cT;
        float ratchet = L.ratchet;
        if(inFill(lane, cycle, t)) ratchet += L.fill * 0.5f * (1.0f - ratchet);
        if(ratchet > 0.0f && unit(hashOf(m.mat, (uint64_t)t, SALT_RATCHET)) < ratchet) {
            for(int r = 1; r < RATCHET_HITS; r++) {
                const int rt = t + r * std::max(1, L.ratchetUnit);
                if(rt >= next || rt >= cT) break;
                all.push_back({rt, all[j].strength, KIND_RATCHET, 1.0f});
            }
        }
        if(L.flam > 0.0f && unit(hashOf(m.mat, (uint64_t)t, SALT_FLAM)) < L.flam) {
            const int ft = t - FLAM_TICKS;
            if(ft > prev && ft >= 0) all.push_back({ft, all[j].strength, KIND_FLAM, 0.5f});
        }
        if(L.ghost > 0.0f && unit(hashOf(m.mat, (uint64_t)t, SALT_GHOST)) < L.ghost) {
            const int gt = t - SIXTEENTH;
            if(gt > prev && gt >= 0) all.push_back({gt, strengthAt(lane, cycle, gt), KIND_GHOST, 1.0f});
        }
    }
    std::sort(all.begin(), all.end(), [](const Item& a, const Item& b) { return a.tick != b.tick ? a.tick < b.tick : a.kind < b.kind; });
    all.erase(std::unique(all.begin(), all.end(), sameTick), all.end());

    // 5. Amplitudes: the accent makes the unaccented hits up to 75% softer,
    //    in the Soft .. Loud range
    std::vector<int> accented;   // ACC_EUCLID: indices of the main hits
    if(L.accentSource == ACC_EUCLID) {
        int count = 0;
        for(const auto& it : all) if(it.kind == KIND_MAIN) count++;
        const int k = std::min(std::max(0, L.accentHits), count);
        for(int i = 0; i < count; i++) if(k > 0 && ((int64_t)i * k) % count < k) accented.push_back(i);
    }
    const int fillLen = L.fillTicks();
    int mainIndex = 0;
    for(auto& it : all) {
        if(it.kind == KIND_GHOST) { it.amp = GHOST_LEVEL * L.level; continue; }
        float a = 1.0f;
        switch(L.accentSource) {
            case ACC_STRONG: a = 1.0f - L.accent * (1.0f - it.strength); break;
            case ACC_RANDOM: a = 1.0f - 0.75f * L.accent * unit(hashOf(m.mat, (uint64_t)it.tick, SALT_AMP)); break;
            case ACC_EUCLID: {
                const bool on = it.kind == KIND_MAIN && std::find(accented.begin(), accented.end(), mainIndex) != accented.end();
                a = on ? 1.0f : 1.0f - 0.75f * L.accent;
                break;
            }
        }
        if(it.kind == KIND_MAIN) mainIndex++;
        float amp = (L.ampMin + (L.level - L.ampMin) * clampf(a, 0.0f, 1.0f)) * it.factor;
        if(inFill(lane, cycle, it.tick)) {
            const float fp = (float)(it.tick - (cT - fillLen)) / (float)std::max(1, fillLen);
            amp *= 1.0f - 0.4f * L.fill * (1.0f - fp);
        }
        it.amp = amp;
    }
    applyAmplitudeOps(lane, m, all);
    // Duck / boost against a lane above
    const int dyn = link(lane, LINK_DYN);
    if(dyn >= 0) {
        std::vector<int> ticks;
        for(const auto& o : laneOnsets(dyn, from, from + cT)) if(o.tick >= from) ticks.push_back((int)(o.tick - from));
        for(auto& it : all)
            if(std::binary_search(ticks.begin(), ticks.end(), it.tick))
                it.amp = L.dynMode == DYN_DUCK ? it.amp * (1.0f - L.dynAmount) : it.amp * (1.0f + L.dynAmount);
    }
    for(auto& it : all) it.amp = clampf(it.amp, 0.001f, 1.0f);

    // 6. Groove: swing and humanize
    const float humanTicks = clampf(L.humanize, 0.0f, 1.0f) * (TPB / 8);
    if(L.swing > 0.0f || humanTicks > 0.0f) {
        const int pair = L.swingUnit == SWING_8THS ? TPB : TPB / 2;
        const int off = (int)std::lround(pair * (0.5f + 0.25f * clampf(L.swing, 0.0f, 1.0f))) - pair / 2;
        for(auto& it : all) {
            const int t = it.tick;
            int shift = t % pair == pair / 2 ? off : 0;
            if(humanTicks > 0.0f)
                shift += (int)std::lround((unit(hashOf(m.mat, (uint64_t)t, SALT_HUMAN)) * 2.0f - 1.0f) * humanTicks);
            it.tick = std::clamp(t + shift, 0, cT - 1);
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
    const int chokeLane = link(lane, LINK_CHOKE);
    if(chokeLane >= 0 && !items.empty()) choke = laneOnsets(chokeLane, cycle * cT, cycle * cT + cT + 4 * TPB);

    std::vector<Onset> out;
    out.reserve(items.size());
    for(size_t j = 0; j < items.size(); j++) {
        const Item& it = items[j];
        const int nextTick = j + 1 < items.size() ? items[j + 1].tick : cT + nextFirst;
        const int gap = std::max(1, nextTick - it.tick);
        int dur = (int)std::lround(L.durTicks * (1.0f + 3.0f * L.longOnStrong * it.strength));
        if(it.tied) dur = gap;
        dur = (int)std::lround(dur * it.durScale);
        // The gate falls at least a tick before the next onset of its lane,
        // so a receiver always sees a fresh rising edge.
        dur = std::clamp(dur, 1, std::max(1, gap - 1));
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

// Before a genome change: remember the current genome for the cycles until
// nowCycle (so the change lands on the next cycle, never in the middle of
// one), and the current hits of the next cycles for the highlight.
void Engine::beginChange(int lane, int64_t nowCycle) {
    LaneState& st = state[lane];
    // A change still waiting for its cycle: the cycles until then keep the
    // genome they had before the first one
    if(st.hasBefore && st.changeFrom == nowCycle + 1) return;
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

void Engine::pushUndo(int lane, int64_t nowCycle) {
    LaneState& st = state[lane];
    st.undo.push_back(layers[lane].genome());
    if(st.undo.size() > 64) st.undo.erase(st.undo.begin());
    st.redo.clear();
    beginChange(lane, nowCycle);
}

void Engine::setGenome(int lane, const Genome& g) {
    layers[lane].seed = g.seed;
    layers[lane].ops = g.ops;
    changed(lane);
}

void Engine::mutate(int lane, int64_t nowCycle, uint32_t randomSeed) {
    if(!canMutate(lane)) return;
    pushUndo(lane, nowCycle);
    Genome g = layers[lane].genome();
    Stream s{hashOf(randomSeed, (uint64_t)g.ops.size(), (uint64_t)lane, SALT_OPS)};
    Op op;
    op.type = s.pick(OP_COUNT);
    op.seed = (uint32_t)(s.next() * 4294967295.0f);
    g.ops.push_back(op);
    // Only the latest mutations stay: an evolving loop slowly forgets its
    // oldest changes instead of piling them up
    if((int)g.ops.size() > MAX_OPS) g.ops.erase(g.ops.begin(), g.ops.end() - MAX_OPS);
    setGenome(lane, g);
}

void Engine::reroll(int lane, int64_t nowCycle, int newSeed) {
    if(!canMutate(lane)) return;
    pushUndo(lane, nowCycle);
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

} // namespace metrobox
