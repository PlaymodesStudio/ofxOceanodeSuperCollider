//
//  scChordBoxEngine.h
//  ofxOceanodeSuperCollider
//
//  ChordBox's harmony engine: the Chord Sequence node's (ofxSantiNodes) every
//  musical function, ported as is -- the libraries (chords.txt, scales.txt,
//  functional_harmony.json, cypher_aliases.json), the five step modes
//  (chord, scale, cypher, degree, functional), each output's shaping
//  (source, inversion, transpose, fold, voicing, spread, rootless, size /
//  expand, random octave / chromatic / diatonic deviation, bass, range, the
//  three voice leadings, detune, sort) -- with two changes:
//
//  • Every random draw is seeded (the node Seed, the step, the output, the
//    voice): the same position always gives the same chord.
//  • In the timed orders the progression follows the position: step k of
//    the progression starts at a beat computed from the origin (Reset) and
//    the steps' lengths; Random / Markov choices are seeded per step. The
//    timeline is generated in order from the origin and cached (voice
//    leading depends on the chord before), so a seek or a loop replays it.
//
//  The variations follow the old node: a random transpose / inversion
//  redrawn every N steps (or N laps: whenever step 1 comes back), and the
//  transposition sequencer adding its next step value every N steps / laps.
//  (Its Descending order counted two laps per lap; one here.)
//
//  The JSON state ("state" in presets and snapshots) is the old node's.
//  No openFrameworks: only nlohmann::json (tools/chordbox_test builds it).
//

#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace chordbox {

using json = nlohmann::json;

constexpr int MAX_OUTPUTS = 16;
constexpr int MAX_STEPS = 32;
constexpr int VOICES = 16;           // audio channels per output

enum EntryMode : int { MODE_CHORD = 0, MODE_SCALE, MODE_CYPHER, MODE_DEGREE, MODE_FUNCTIONAL };
enum VoicingMode : int { VOICING_NONE = 0, VOICING_CLOSE, VOICING_OPEN, VOICING_DROP2, VOICING_DROP3, VOICING_SHELL };
enum SourceMode : int { SOURCE_CHORD = 0, SOURCE_SCALE, SOURCE_ROOT, SOURCE_KEY, SOURCE_SUM };
enum VoiceLeading : int { VL_OFF = 0, VL_CLASSIC, VL_GRAVITY, VL_GRAVITY_FREE };
enum Order : int { ORDER_INDEX = 0, ORDER_ASC, ORDER_DESC, ORDER_RANDOM, ORDER_MARKOV };

inline const char* const* keyNames(){ static const char* k[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"}; return k; }
inline const char* functionalGroupKey(int g){ static const char* k[] = {"tonic", "subdominant", "dominant"}; return k[std::clamp(g, 0, 2)]; }
inline const char* functionalGroupLabel(int g){ static const char* k[] = {"Tonic", "Subdominant", "Dominant"}; return k[std::clamp(g, 0, 2)]; }

// ── Small helpers (openFrameworks' equivalents) ─────────────────────────────
inline std::string trim(const std::string& s){
    size_t a = 0, b = s.size();
    while(a < b && std::isspace((unsigned char)s[a])) a++;
    while(b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
inline std::vector<std::string> split(const std::string& s, char d){
    std::vector<std::string> out;
    std::string cur;
    for(char c : s){ if(c == d){ cur = trim(cur); if(!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
    cur = trim(cur);
    if(!cur.empty()) out.push_back(cur);
    return out;
}
inline std::vector<std::string> splitSpaces(const std::string& s){
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while(in >> w) out.push_back(w);
    return out;
}
inline int wrapIndex(int value, int size){ if(size <= 0) return -1; int w = value % size; return w < 0 ? w + size : w; }
inline int64_t floorDiv(int64_t a, int64_t b){ int64_t q = a / b; if((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
inline float pitchClass(float v){ float r = std::fmod(v, 12.0f); return r < 0.0f ? r + 12.0f : r; }
inline float sampleVector(const std::vector<float>& v, size_t i){ if(v.empty()) return 0.0f; return i < v.size() ? v[i] : v.back(); }

// ── Seeded randomness ───────────────────────────────────────────────────────
inline uint64_t mix64(uint64_t x){
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
inline uint64_t H(uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0){
    return mix64(mix64(mix64(mix64(a ^ 0xC4B0) ^ b) ^ (c * 0x9e3779b97f4a7c15ull)) ^ (d * 0xc2b2ae3d27d4eb4full));
}
inline double unit(uint64_t h){ return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
// A stream of draws (ofRandom's role in the old node)
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t s) : state(s) {}
    float uniform(){ state = mix64(state + 0x632BE59BD9B4E019ull); return (float)unit(state); }
    float range(float a, float b){ return a + (b - a) * uniform(); }
};

// ── Data ────────────────────────────────────────────────────────────────────
struct LibItem { std::string name; std::vector<float> values; };
struct FunctionalVariant { std::string label; int degree = 1; };

struct Entry {
    int mode = MODE_CHORD;
    int itemIndex = 0;
    std::string itemName;
    int functionalGroup = 0;
    int functionalVariantIndex = 0;
    std::string functionalVariantLabel;
    int degree = 1;
    int chordSize = 4;
    int stepInterval = 2;
    float diatonicDeviationProbability = 0.0f;
    int diatonicDeviationRange = 0;
    float beatDuration = 1.0f;
    std::vector<float> markovWeights;
    int transpose = 0;
    int inversion = 0;

    json toJson() const {
        return {{"mode", mode}, {"itemIndex", itemIndex}, {"itemName", itemName}, {"functionalGroup", functionalGroup},
                {"functionalVariantIndex", functionalVariantIndex}, {"functionalVariantLabel", functionalVariantLabel},
                {"degree", degree}, {"chordSize", chordSize}, {"stepInterval", stepInterval},
                {"diatonicDeviationProbability", diatonicDeviationProbability}, {"diatonicDeviationRange", diatonicDeviationRange},
                {"beatDuration", beatDuration}, {"markovWeights", markovWeights}, {"transpose", transpose}, {"inversion", inversion}};
    }
    static Entry fromJson(const json& j){
        Entry e;
        e.mode = j.value("mode", (int)MODE_CHORD);
        e.itemIndex = j.value("itemIndex", 0);
        e.itemName = j.value("itemName", std::string());
        e.functionalGroup = std::clamp(j.value("functionalGroup", 0), 0, 2);
        e.functionalVariantIndex = std::max(0, j.value("functionalVariantIndex", 0));
        e.functionalVariantLabel = j.value("functionalVariantLabel", std::string());
        e.degree = std::max(1, j.value("degree", 1));
        e.chordSize = std::max(1, j.value("chordSize", 4));
        e.stepInterval = std::max(1, j.value("stepInterval", 2));
        e.diatonicDeviationProbability = std::clamp(j.value("diatonicDeviationProbability", 0.0f), 0.0f, 100.0f);
        e.diatonicDeviationRange = std::max(0, j.value("diatonicDeviationRange", 0));
        e.beatDuration = std::max(0.001f, j.value("beatDuration", 1.0f));
        if(j.contains("markovWeights") && j["markovWeights"].is_array())
            for(const auto& w : j["markovWeights"]) if(w.is_number()) e.markovWeights.push_back(std::max(0.0f, w.get<float>()));
        e.transpose = j.value("transpose", 0);
        e.inversion = j.value("inversion", 0);
        return e;
    }
};

struct OutputConfig {
    int octave = 0;
    int transpose = 0;
    float pitchBend = 0.0f;
    float perNoteDetune = 0.0f;
    float octaveRandomProbability = 0.0f;
    int octaveRandomRange = 0;
    float chromaticDeviationProbability = 0.0f;
    int chromaticDeviationRange = 0;
    int voiceLeadingMode = VL_OFF;
    float gravityAnchorWeight = 1.5f;
    int minNote = -12;
    int maxNote = 128;
    int sourceMode = SOURCE_CHORD;
    bool addBass = false;
    int bassOct = -2;
    int inversion = 0;
    int voicingMode = VOICING_NONE;
    float voicingSpread = 0.0f;
    bool fold12 = false;
    bool rootLess = false;
    float glideMs = 0.0f;
    bool glideBeats = false;     // new: Glide in beats (glideMs then holds beats)
    int outputSize = 4;
    bool expandOutput = false;
    bool sortOutput = false;

    json toJson() const {
        return {{"octave", octave}, {"transpose", transpose}, {"pitchBend", pitchBend}, {"perNoteDetune", perNoteDetune},
                {"octaveRandomProbability", octaveRandomProbability}, {"octaveRandomRange", octaveRandomRange},
                {"chromaticDeviationProbability", chromaticDeviationProbability}, {"chromaticDeviationRange", chromaticDeviationRange},
                {"voiceLeadingMode", voiceLeadingMode}, {"gravityAnchorWeight", gravityAnchorWeight}, {"minNote", minNote},
                {"maxNote", maxNote}, {"sourceMode", sourceMode}, {"addBass", addBass}, {"bassOct", bassOct},
                {"inversion", inversion}, {"voicingMode", voicingMode}, {"voicingSpread", voicingSpread}, {"fold12", fold12},
                {"rootLess", rootLess}, {"glideMs", glideMs}, {"glideBeats", glideBeats}, {"outputSize", outputSize},
                {"expandOutput", expandOutput}, {"sortOutput", sortOutput}};
    }
    static OutputConfig fromJson(const json& j){
        OutputConfig c;
        c.octave = j.value("octave", 0);
        c.transpose = j.value("transpose", 0);
        c.pitchBend = j.value("pitchBend", 0.0f);
        c.perNoteDetune = std::max(0.0f, j.value("perNoteDetune", 0.0f));
        c.octaveRandomProbability = std::clamp(j.value("octaveRandomProbability", 0.0f), 0.0f, 100.0f);
        c.octaveRandomRange = std::max(0, j.value("octaveRandomRange", 0));
        c.chromaticDeviationProbability = std::clamp(j.value("chromaticDeviationProbability", 0.0f), 0.0f, 100.0f);
        c.chromaticDeviationRange = std::max(0, j.value("chromaticDeviationRange", 0));
        const int legacy = j.value("voiceLeading", false) ? (int)VL_CLASSIC : (int)VL_OFF;
        c.voiceLeadingMode = std::clamp(j.value("voiceLeadingMode", legacy), (int)VL_OFF, (int)VL_GRAVITY_FREE);
        c.gravityAnchorWeight = std::max(0.0f, j.value("gravityAnchorWeight", 1.5f));
        c.minNote = std::clamp(j.value("minNote", -12), -12, 128);
        c.maxNote = std::clamp(j.value("maxNote", 128), -12, 128);
        if(c.minNote > c.maxNote) std::swap(c.minNote, c.maxNote);
        if(j.contains("sourceMode")) c.sourceMode = std::clamp(j.value("sourceMode", (int)SOURCE_CHORD), (int)SOURCE_CHORD, (int)SOURCE_SUM);
        else if(j.value("scaleOnly", false)) c.sourceMode = SOURCE_SCALE;
        else if(j.value("keyOnly", false)) c.sourceMode = SOURCE_KEY;
        else if(j.value("rootOnly", false)) c.sourceMode = SOURCE_ROOT;
        c.addBass = j.value("addBass", false);
        c.bassOct = j.value("bassOct", -2);
        c.inversion = j.value("inversion", 0);
        c.voicingMode = std::clamp(j.value("voicingMode", (int)VOICING_NONE), (int)VOICING_NONE, (int)VOICING_SHELL);
        c.voicingSpread = std::max(0.0f, j.value("voicingSpread", 0.0f));
        c.fold12 = j.value("fold12", false);
        c.rootLess = j.value("rootLess", false);
        c.glideMs = j.value("glideMs", 0.0f);
        c.glideBeats = j.value("glideBeats", false);
        c.outputSize = std::max(1, j.value("outputSize", 4));
        c.expandOutput = j.value("expandOutput", false);
        c.sortOutput = j.value("sortOutput", false);
        return c;
    }
};

inline bool scaleLike(int sourceMode){ return sourceMode == SOURCE_SCALE || sourceMode == SOURCE_SUM; }
inline bool supportsDiatonicDeviation(int mode){ return mode == MODE_SCALE || mode == MODE_DEGREE || mode == MODE_FUNCTIONAL; }

// The variations in effect for a step
struct Mods { int transpose = 0; int invert = 0; int randomTranspose = 0; int randomInvert = 0; int sequencer = 0; };

class Engine {
public:
    // ── Libraries ───────────────────────────────────────────────────────────
    std::vector<LibItem> chordLibrary, scaleLibrary;
    std::unordered_map<std::string, std::array<std::vector<FunctionalVariant>, 3>> functionalLibrary;
    std::unordered_map<std::string, std::string> aliases;
    int defaultChordIndex = 0, defaultScaleIndex = 0;

    // ── State (the old node's "state") ──────────────────────────────────────
    std::vector<Entry> progression;
    std::vector<OutputConfig> outputs = std::vector<OutputConfig>(1);
    int globalKey = 0;
    int globalScaleIndex = 0;
    std::string globalScaleName;
    bool useExternalScale = false;
    std::vector<float> externalScale = {0, 2, 4, 5, 7, 9, 11};
    int globalTranspose = 0, globalInvert = 0;
    int transposeRandomRange = 0, transposeRandomQuantization = 1;
    bool transposeRandomStep = false;
    int transposeRandomCycles = 1;
    int inversionRandomRange = 0, inversionRandomQuantization = 1;
    bool inversionRandomStep = false;
    int inversionRandomCycles = 1;
    int transposeSequencerCycles = 4;
    std::vector<int> transposeSequencerSteps = {0};
    int transposeSequencerMod = 12;
    bool transposeSequencerStepMode = false;
    float globalPitchBend = 0.0f;
    int progressionOrder = ORDER_INDEX;

    // ── Timeline ────────────────────────────────────────────────────────────
    uint64_t seed = 1;          // in effect (the node: 0 -> a fresh value per pass)
    double originBeat = 0.0;    // step 1 of the timed progression (Reset)

    // ── Loading ─────────────────────────────────────────────────────────────
    static std::vector<LibItem> parsePitchClassFile(const std::string& path){
        // "<n>, <name> <v> <v> ...; comment"
        std::vector<LibItem> out;
        std::ifstream in(path);
        std::string raw;
        while(std::getline(in, raw)){
            const std::string line = trim(raw);
            if(line.empty()) continue;
            auto beforeSemicolon = split(line, ';');
            if(beforeSemicolon.empty()) continue;
            auto commaSplit = split(beforeSemicolon[0], ',');
            if(commaSplit.size() < 2) continue;
            auto tokens = splitSpaces(trim(commaSplit[1]));
            if(tokens.empty()) continue;
            LibItem item;
            item.name = tokens[0];
            for(size_t i = 1; i < tokens.size(); i++){ try { item.values.push_back(std::stof(tokens[i])); } catch(...) { item.values.push_back(0.0f); } }
            out.push_back(item);
        }
        return out;
    }
    static std::string normalizeLookupKey(const std::string& v){
        std::string n = trim(v);
        n.erase(std::remove(n.begin(), n.end(), ' '), n.end());
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return n;
    }
    // dir: the Pitchclass folder (with a trailing slash)
    void loadLibraries(const std::string& dir){
        chordLibrary = parsePitchClassFile(dir + "chords.txt");
        scaleLibrary = parsePitchClassFile(dir + "scales.txt");
        aliases.clear();
        try {
            std::ifstream in(dir + "cypher_aliases.json");
            if(in){
                json j; in >> j;
                if(j.is_object()) for(auto it = j.begin(); it != j.end(); ++it){
                    if(!it.value().is_string()) continue;
                    aliases[it.key()] = it.value().get<std::string>();
                    aliases[normalizeLookupKey(it.key())] = it.value().get<std::string>();
                }
            }
        } catch(...) {}
        functionalLibrary.clear();
        try {
            std::ifstream in(dir + "functional_harmony.json");
            if(in){
                json j; in >> j;
                if(j.is_object()) for(auto it = j.begin(); it != j.end(); ++it){
                    if(!it.value().is_object()) continue;
                    std::array<std::vector<FunctionalVariant>, 3> groups;
                    for(int g = 0; g < 3; g++){
                        const char* k = functionalGroupKey(g);
                        if(!it.value().contains(k) || !it.value()[k].is_array()) continue;
                        for(const auto& v : it.value()[k]){
                            if(!v.is_object()) continue;
                            FunctionalVariant fv;
                            fv.label = v.value("label", std::string());
                            fv.degree = std::max(1, v.value("degree", 1));
                            if(!fv.label.empty()) groups[g].push_back(fv);
                        }
                    }
                    functionalLibrary[it.key()] = groups;
                }
            }
        } catch(...) {}
        if(chordLibrary.empty()) chordLibrary.push_back({"Unavailable", {0.0f}});
        if(scaleLibrary.empty()) scaleLibrary.push_back({"Unavailable", {0.0f}});
        defaultChordIndex = std::max(0, findItem(chordLibrary, "M"));
        defaultScaleIndex = std::max(0, findItem(scaleLibrary, "major"));
        if(globalScaleName.empty() && globalScaleIndex == 0){ globalScaleIndex = defaultScaleIndex; globalScaleName = scaleLibrary[defaultScaleIndex].name; }
        sanitizeGlobalScale();
        if(progression.empty()) initializeDefaultProgression();
    }

    // ── State <-> JSON (the old node's keys) ────────────────────────────────
    json toJson() const {
        json j;
        j["numOutputs"] = (int)outputs.size();
        j["globalKey"] = globalKey;
        j["globalScaleIndex"] = globalScaleIndex;
        j["globalScaleName"] = globalScaleName;
        j["useExternalScale"] = useExternalScale;
        j["globalTranspose"] = globalTranspose;
        j["globalInvert"] = globalInvert;
        j["transposeRandomRange"] = transposeRandomRange;
        j["transposeRandomQuantization"] = transposeRandomQuantization;
        j["transposeRandomStep"] = transposeRandomStep;
        j["transposeRandomCycles"] = transposeRandomCycles;
        j["inversionRandomRange"] = inversionRandomRange;
        j["inversionRandomQuantization"] = inversionRandomQuantization;
        j["inversionRandomStep"] = inversionRandomStep;
        j["inversionRandomCycles"] = inversionRandomCycles;
        j["transposeSequencerCycles"] = transposeSequencerCycles;
        j["transposeSequencerSteps"] = transposeSequencerSteps;
        j["transposeSequencerMod"] = transposeSequencerMod;
        j["transposeSequencerStepMode"] = transposeSequencerStepMode;
        j["globalPitchBend"] = globalPitchBend;
        j["progressionOrder"] = progressionOrder;
        j["internalTimingEnabled"] = progressionOrder != ORDER_INDEX;
        j["markovEnabled"] = progressionOrder == ORDER_MARKOV;
        j["internalActiveStep"] = 0;
        j["entries"] = json::array();
        j["outputs"] = json::array();
        for(const auto& e : progression) j["entries"].push_back(e.toJson());
        for(const auto& o : outputs) j["outputs"].push_back(o.toJson());
        return j;
    }
    void fromJson(const json& j){
        globalKey = std::clamp(j.value("globalKey", 0), 0, 11);
        globalScaleIndex = std::max(0, j.value("globalScaleIndex", defaultScaleIndex));
        globalScaleName = j.value("globalScaleName", std::string());
        useExternalScale = j.value("useExternalScale", false);
        globalTranspose = j.value("globalTranspose", 0);
        globalInvert = j.value("globalInvert", 0);
        transposeRandomRange = std::max(0, j.value("transposeRandomRange", 0));
        transposeRandomQuantization = std::max(1, j.value("transposeRandomQuantization", 1));
        transposeRandomStep = j.value("transposeRandomStep", false);
        transposeRandomCycles = std::max(1, j.value("transposeRandomCycles", 1));
        inversionRandomRange = std::max(0, j.value("inversionRandomRange", 0));
        inversionRandomQuantization = std::max(1, j.value("inversionRandomQuantization", 1));
        inversionRandomStep = j.value("inversionRandomStep", false);
        inversionRandomCycles = std::max(1, j.value("inversionRandomCycles", 1));
        transposeSequencerCycles = std::max(1, j.value("transposeSequencerCycles", 4));
        if(j.contains("transposeSequencerSteps") && j["transposeSequencerSteps"].is_array()){
            transposeSequencerSteps.clear();
            for(const auto& s : j["transposeSequencerSteps"]) if(s.is_number()) transposeSequencerSteps.push_back(s.get<int>());
            if(transposeSequencerSteps.empty()) transposeSequencerSteps = {0};
        }else transposeSequencerSteps = {j.value("transposeSequencerStep", 0)};
        transposeSequencerMod = std::max(0, j.value("transposeSequencerMod", 12));
        transposeSequencerStepMode = j.value("transposeSequencerStepMode", false);
        globalPitchBend = j.value("globalPitchBend", j.value("pitchBend", 0.0f));
        if(j.contains("progressionOrder")) progressionOrder = std::clamp(j.value("progressionOrder", 0), 0, 4);
        else {
            const bool timed = j.value("internalTimingEnabled", false), markov = j.value("markovEnabled", false);
            progressionOrder = timed ? (markov ? ORDER_MARKOV : ORDER_ASC) : ORDER_INDEX;
        }
        progression.clear();
        if(j.contains("entries") && j["entries"].is_array()) for(const auto& e : j["entries"]) progression.push_back(Entry::fromJson(e));
        if(progression.empty()) initializeDefaultProgression();
        int n = 1;
        if(j.contains("outputs") && j["outputs"].is_array()) n = std::max(1, (int)j["outputs"].size());
        else n = std::max(1, j.value("numOutputs", 1));
        outputs.assign(std::min(n, MAX_OUTPUTS), OutputConfig());
        if(j.contains("outputs") && j["outputs"].is_array()){
            for(size_t i = 0; i < outputs.size() && i < j["outputs"].size(); i++) outputs[i] = OutputConfig::fromJson(j["outputs"][i]);
        }else{
            outputs[0].outputSize = std::max(1, j.value("outputSize", 4));
            outputs[0].expandOutput = j.value("expandOutput", false);
            outputs[0].sortOutput = j.value("sortOutput", false);
            outputs[0].glideMs = j.value("glideMs", 0.0f);
        }
        sanitize();
    }

    // ── Progression editing ─────────────────────────────────────────────────
    Entry defaultEntry() const {
        Entry e;
        e.mode = MODE_CHORD;
        e.itemIndex = defaultChordIndex;
        e.itemName = chordLibrary.empty() ? "M" : chordLibrary[defaultChordIndex].name;
        e.functionalVariantLabel = "I";
        return e;
    }
    void initializeDefaultProgression(){ progression.assign(4, defaultEntry()); }
    void resize(int n){
        n = std::clamp(n, 1, MAX_STEPS);
        while((int)progression.size() < n) progression.push_back(defaultEntry());
        progression.resize(n);
        sanitize();
    }
    void sanitize(){
        if(progression.empty()) initializeDefaultProgression();
        if((int)progression.size() > MAX_STEPS) progression.resize(MAX_STEPS);
        sanitizeGlobalScale();
        for(auto& e : progression) sanitizeEntry(e);
        sanitizeMarkov();
        if(outputs.empty()) outputs.assign(1, OutputConfig());
    }
    void sanitizeEntry(Entry& e) const {
        e.degree = std::max(1, e.degree);
        e.chordSize = std::max(1, e.chordSize);
        e.stepInterval = std::max(1, e.stepInterval);
        e.diatonicDeviationProbability = std::clamp(e.diatonicDeviationProbability, 0.0f, 100.0f);
        e.diatonicDeviationRange = std::max(0, e.diatonicDeviationRange);
        e.beatDuration = std::max(0.001f, e.beatDuration);
        for(auto& w : e.markovWeights) w = std::max(0.0f, w);
        if(e.mode == MODE_CYPHER){ if(e.itemName.empty()) e.itemName = "C"; return; }
        if(e.mode == MODE_FUNCTIONAL){
            e.functionalGroup = std::clamp(e.functionalGroup, 0, 2);
            applyFunctionalVariant(e);
            e.itemIndex = 0;
            e.itemName = e.functionalVariantLabel.empty() ? std::string(functionalGroupLabel(e.functionalGroup)) : e.functionalVariantLabel;
            return;
        }
        if(e.mode == MODE_DEGREE){ e.itemIndex = 0; e.itemName = "Degree " + std::to_string(e.degree); return; }
        e.mode = e.mode == MODE_SCALE ? MODE_SCALE : MODE_CHORD;
        const auto& lib = libraryFor(e.mode);
        if(lib.empty()){ e.itemIndex = 0; e.itemName.clear(); return; }
        const int byName = findItem(lib, e.itemName);
        e.itemIndex = byName >= 0 ? byName : std::clamp(e.itemIndex, 0, (int)lib.size() - 1);
        e.itemName = lib[e.itemIndex].name;
    }
    void sanitizeMarkov(){
        const int n = (int)progression.size();
        for(int i = 0; i < n; i++){
            std::vector<float> old = progression[i].markovWeights;
            progression[i].markovWeights.assign(n, 0.0f);
            for(int k = 0; k < std::min(n, (int)old.size()); k++) progression[i].markovWeights[k] = std::max(0.0f, old[k]);
            bool any = false;
            for(float w : progression[i].markovWeights) if(w > 0.0001f) any = true;
            if(!any && n > 0) progression[i].markovWeights[(i + 1) % n] = 1.0f;
        }
    }
    void sanitizeGlobalScale(){
        if(scaleLibrary.empty()){ globalScaleIndex = 0; globalScaleName.clear(); return; }
        if(globalScaleName.empty() && globalScaleIndex == 0) globalScaleIndex = defaultScaleIndex;
        const int byName = findItem(scaleLibrary, globalScaleName);
        globalScaleIndex = byName >= 0 ? byName : std::clamp(globalScaleIndex, 0, (int)scaleLibrary.size() - 1);
        globalScaleName = scaleLibrary[globalScaleIndex].name;
    }

    // ── Cypher parsing ──────────────────────────────────────────────────────
    static int findItem(const std::vector<LibItem>& lib, const std::string& name){
        for(size_t i = 0; i < lib.size(); i++) if(lib[i].name == name) return (int)i;
        return -1;
    }
    const std::vector<LibItem>& libraryFor(int mode) const { return mode == MODE_SCALE ? scaleLibrary : chordLibrary; }
    static bool supplementalChord(const std::string& name, std::vector<float>& values){
        static const std::map<std::string, std::vector<float>> defs = {{"13sus2", {0.0f, 2.0f, 7.0f, 10.0f, 21.0f}}};
        auto it = defs.find(name);
        if(it == defs.end()) return false;
        values = it->second;
        return true;
    }
    static int noteValue(const std::string& n){
        static const std::map<std::string, int> m = {{"C", 0}, {"C#", 1}, {"Db", 1}, {"D", 2}, {"D#", 3}, {"Eb", 3}, {"E", 4}, {"Fb", 4},
            {"F", 5}, {"E#", 5}, {"F#", 6}, {"Gb", 6}, {"G", 7}, {"G#", 8}, {"Ab", 8}, {"A", 9}, {"A#", 10}, {"Bb", 10}, {"B", 11}, {"Cb", 11}};
        auto it = m.find(n);
        return it == m.end() ? 0 : it->second;
    }
    std::string resolveAlias(const std::string& v) const {
        auto a = aliases.find(v);
        if(a != aliases.end()) return a->second;
        auto b = aliases.find(normalizeLookupKey(v));
        if(b != aliases.end()) return b->second;
        return v;
    }
    std::string normalizeQuality(const std::string& quality) const {
        std::string q = trim(quality);
        q.erase(std::remove(q.begin(), q.end(), ' '), q.end());
        if(q.empty()) return "M";
        auto exists = [this](const std::string& name){
            std::vector<float> v;
            if(supplementalChord(name, v)) return true;
            return findItem(chordLibrary, name) >= 0;
        };
        if(findItem(chordLibrary, q) >= 0) return q;
        auto exact = aliases.find(q);
        if(exact != aliases.end() && findItem(chordLibrary, exact->second) >= 0) return exact->second;
        // a capital M is major ("Madd9"): the aliases' case-blind lookup would read it as "madd9"
        if(q.size() > 1 && q[0] == 'M' && std::isalpha((unsigned char)q[1]) && q.compare(0, 3, "Maj") != 0 && q.compare(0, 3, "MAJ") != 0)
            return normalizeQuality(q.substr(1));
        const std::string resolved = resolveAlias(q);
        if(findItem(chordLibrary, resolved) >= 0) return resolved;
        if(q.size() > 3 && q.compare(q.size() - 3, 3, "sus") == 0){
            const std::string sus4 = q.substr(0, q.size() - 3) + "sus4";
            if(exists(sus4)) return sus4;
        }
        return resolved;
    }
    bool parseCypher(const std::string& input, float& root, std::string& quality) const {
        std::string t = trim(input);
        if(t.empty()){ root = 0.0f; quality = "M"; return false; }
        const size_t slash = t.find_first_of("/\\");
        if(slash != std::string::npos) t = t.substr(0, slash);
        if(t.empty()){ root = 0.0f; quality = "M"; return false; }
        // no note letter first ("madd9", "M7"): the quality alone, on C (the old node
        // read the "m" as a root and lost it: "madd9" played major)
        const char first = (char)std::toupper((unsigned char)t[0]);
        if(first < 'A' || first > 'G'){ root = 0.0f; quality = normalizeQuality(t); return true; }
        std::string r;
        r += first;
        size_t off = 1;
        if(t.size() > 1 && (t[1] == '#' || t[1] == 'b')){ r += t[1]; off = 2; }
        root = (float)noteValue(r);
        quality = normalizeQuality(t.substr(off));
        return true;
    }
    bool resolveCompoundQuality(const std::string& quality, std::vector<float>& values) const {
        struct Rule { std::string token, base; };
        static const std::vector<Rule> rules = {{"min", "m"}, {"m", "m"}, {"maj", "M"}, {"M", "M"}, {"dim", "dim"}, {"aug", "aug"}};
        for(const auto& rule : rules){
            if(quality.size() <= rule.token.size() || quality.compare(0, rule.token.size(), rule.token) != 0) continue;
            const std::string suffix = normalizeQuality(quality.substr(rule.token.size()));
            if(suffix != "add9" && suffix != "add11" && suffix != "add13") continue;
            const int base = findItem(chordLibrary, rule.base);
            if(base < 0) continue;
            std::vector<float> sv;
            const int si = findItem(chordLibrary, suffix);
            if(si >= 0) sv = chordLibrary[si].values;
            else if(!supplementalChord(suffix, sv)) continue;
            if(sv.empty()) continue;
            values = chordLibrary[base].values;
            values.push_back(sv.back());
            return true;
        }
        return false;
    }
    static std::vector<std::string> parseChordString(const std::string& s){
        std::string f = s;
        std::replace(f.begin(), f.end(), '|', ',');
        return split(f, ',');
    }
    Entry entryFromToken(const std::string& token) const {
        Entry e = defaultEntry();
        e.mode = MODE_CYPHER;
        e.itemName = trim(token);
        return e;
    }

    // ── Scales and degrees ──────────────────────────────────────────────────
    std::vector<float> globalScaleValues() const {
        if(useExternalScale) return externalScale;
        if(scaleLibrary.empty()) return {};
        return scaleLibrary[std::clamp(globalScaleIndex, 0, (int)scaleLibrary.size() - 1)].values;
    }
    const std::array<std::vector<FunctionalVariant>, 3>* functionalGroups() const {
        if(useExternalScale || scaleLibrary.empty()) return nullptr;
        auto it = functionalLibrary.find(scaleLibrary[std::clamp(globalScaleIndex, 0, (int)scaleLibrary.size() - 1)].name);
        return it == functionalLibrary.end() ? nullptr : &it->second;
    }
    const std::vector<FunctionalVariant>& functionalVariants(int group) const {
        static const std::vector<FunctionalVariant> none;
        const auto* g = functionalGroups();
        return g ? (*g)[std::clamp(group, 0, 2)] : none;
    }
    void applyFunctionalVariant(Entry& e) const {
        const auto& v = functionalVariants(e.functionalGroup);
        if(v.empty()){
            e.functionalVariantIndex = 0;
            if(e.functionalVariantLabel.empty()) e.functionalVariantLabel = functionalGroupLabel(e.functionalGroup);
            return;
        }
        int byName = -1;
        for(int i = 0; i < (int)v.size(); i++) if(!e.functionalVariantLabel.empty() && v[i].label == e.functionalVariantLabel){ byName = i; break; }
        e.functionalVariantIndex = byName >= 0 ? byName : std::clamp(e.functionalVariantIndex, 0, (int)v.size() - 1);
        e.functionalVariantLabel = v[e.functionalVariantIndex].label;
    }
    int resolvedDegree(const Entry& e) const {
        if(e.mode == MODE_FUNCTIONAL){
            const auto& v = functionalVariants(e.functionalGroup);
            if(!v.empty()) return std::max(1, v[std::clamp(e.functionalVariantIndex, 0, (int)v.size() - 1)].degree);
        }
        return std::max(1, e.degree);
    }
    std::vector<float> degreeValues(const Entry& e) const {
        const auto sc = globalScaleValues();
        if(sc.empty()) return {0.0f};
        const int n = (int)sc.size(), start = resolvedDegree(e) - 1, count = std::max(1, e.chordSize), step = std::max(1, e.stepInterval);
        std::vector<float> out;
        for(int i = 0; i < count; i++){
            const int si = start + i * step;
            out.push_back(sc[wrapIndex(si, n)] + (float)((int)std::floor((float)si / (float)n) * 12));
        }
        return out;
    }
    std::vector<float> entryIntervals(const Entry& e) const {
        if(e.mode == MODE_CYPHER){
            float root = 0.0f;
            std::string q;
            parseCypher(e.itemName, root, q);
            int qi = findItem(chordLibrary, q);
            if(qi < 0){
                std::vector<float> v;
                if(supplementalChord(q, v)) return v;
                if(resolveCompoundQuality(q, v)) return v;
                qi = defaultChordIndex;
            }
            return chordLibrary[qi].values;
        }
        if(e.mode == MODE_DEGREE || e.mode == MODE_FUNCTIONAL) return degreeValues(e);
        const auto& lib = libraryFor(e.mode);
        if(lib.empty()) return {0.0f};
        return lib[std::clamp(e.itemIndex, 0, (int)lib.size() - 1)].values;
    }
    std::vector<float> diatonicReference(const Entry& e) const {
        if(e.mode == MODE_SCALE){
            const auto& lib = libraryFor(e.mode);
            if(lib.empty()) return {};
            auto r = lib[std::clamp(e.itemIndex, 0, (int)lib.size() - 1)].values;
            for(auto& v : r) v += (float)e.transpose;
            return r;
        }
        if(e.mode == MODE_DEGREE || e.mode == MODE_FUNCTIONAL){
            auto r = globalScaleValues();
            for(auto& v : r) v += (float)(globalKey + e.transpose);
            return r;
        }
        return {};
    }
    float entryRoot(const Entry& e) const {
        if(e.mode == MODE_CYPHER){
            float root = 0.0f;
            std::string q;
            parseCypher(e.itemName, root, q);
            return root + (float)e.transpose;
        }
        if(e.mode == MODE_DEGREE || e.mode == MODE_FUNCTIONAL){
            const auto dv = degreeValues(e);
            if(dv.empty()) return (float)(globalKey + e.transpose);
            return (float)(globalKey + e.transpose) + dv.front();
        }
        return (float)e.transpose;
    }
    std::string entryLabel(const Entry& e) const {
        if(e.mode == MODE_CYPHER) return e.itemName.empty() ? "C" : e.itemName;
        if(e.mode == MODE_FUNCTIONAL) return e.functionalVariantLabel.empty() ? functionalGroupLabel(e.functionalGroup) : e.functionalVariantLabel;
        if(e.mode == MODE_DEGREE) return "Deg " + std::to_string(std::max(1, e.degree));
        const auto& lib = libraryFor(e.mode);
        if(lib.empty()) return "---";
        const std::string name = lib[std::clamp(e.itemIndex, 0, (int)lib.size() - 1)].name;
        return e.mode == MODE_SCALE ? name : std::string(keyNames()[(int)pitchClass((float)e.transpose)]) + name;
    }

    // ── Building a step's notes ─────────────────────────────────────────────
    static std::vector<float> applyInversion(const std::vector<float>& values, int inversion){
        if(values.empty() || inversion == 0) return values;
        std::vector<float> v = values;
        if(inversion > 0){ for(int i = 0; i < inversion; i++){ const float m = v.front() + 12.0f; v.erase(v.begin()); v.push_back(m); } }
        else for(int i = 0; i < -inversion; i++){ const float m = v.back() - 12.0f; v.pop_back(); v.insert(v.begin(), m); }
        return v;
    }
    std::vector<float> entryPreview(const Entry& e) const {
        if(e.mode == MODE_DEGREE || e.mode == MODE_FUNCTIONAL){
            auto v = degreeValues(e);
            for(auto& x : v) x += (float)(globalKey + e.transpose);
            return applyInversion(v, e.inversion);
        }
        auto v = applyInversion(entryIntervals(e), e.inversion);
        const float root = entryRoot(e);
        for(auto& x : v) x += root;
        return v;
    }
    std::vector<float> applyDiatonicDeviation(const std::vector<float>& values, const Entry& e, Rng& rng) const {
        if(!supportsDiatonicDeviation(e.mode) || e.diatonicDeviationProbability <= 0.0f || e.diatonicDeviationRange <= 0 || values.empty()) return values;
        const auto ref = diatonicReference(e);
        const int n = (int)ref.size();
        if(n <= 0) return values;
        auto out = values;
        for(auto& v : out){
            if(rng.range(0.0f, 100.0f) >= e.diatonicDeviationProbability) continue;
            const int center = (int)std::floor(v / 12.0f);
            int closest = 0;
            float best = std::numeric_limits<float>::max();
            for(int oct = center - 2; oct <= center + 2; oct++)
                for(int i = 0; i < n; i++){
                    const float d = std::abs(ref[i] + (float)(oct * 12) - v);
                    if(d < best){ best = d; closest = oct * n + i; }
                }
            int dev = 0;
            for(int guard = 0; dev == 0 && guard < 64; guard++)
                dev = (int)std::floor(rng.range(0.0f, (float)(e.diatonicDeviationRange * 2 + 1))) - e.diatonicDeviationRange;
            if(dev == 0) dev = 1;
            const int idx = closest + dev;
            v = ref[wrapIndex(idx, n)] + (float)((int)std::floor((float)idx / (float)n) * 12);
        }
        return out;
    }
    static std::vector<float> applyChromaticDeviation(const std::vector<float>& values, float probability, int range, Rng& rng){
        if(probability <= 0.0f || range <= 0 || values.empty()) return values;
        auto out = values;
        for(auto& v : out){
            if(rng.range(0.0f, 100.0f) >= probability) continue;
            int dev = 0;
            for(int guard = 0; dev == 0 && guard < 64; guard++) dev = (int)std::floor(rng.range(0.0f, (float)(range * 2 + 1))) - range;
            if(dev == 0) dev = 1;
            v += (float)dev;
        }
        return out;
    }
    std::vector<float> chordSumValues() const {
        std::set<int> pcs;
        for(const auto& e : progression) for(float v : entryPreview(e)) pcs.insert(wrapIndex((int)std::round(v), 12));
        if(pcs.empty()) return {(float)globalKey};
        return std::vector<float>(pcs.begin(), pcs.end());
    }
    std::vector<float> sourceValues(const Entry& e, const OutputConfig& c, float& root, Rng& rng) const {
        if(c.sourceMode == SOURCE_SCALE){
            root = (float)globalKey;
            auto v = globalScaleValues();
            if(!v.empty()){ for(auto& x : v) x += (float)globalKey; return v; }
            return {root};
        }
        if(c.sourceMode == SOURCE_SUM){ root = (float)globalKey; return chordSumValues(); }
        if(c.sourceMode == SOURCE_KEY){ root = (float)globalKey; return {root}; }
        if(c.sourceMode == SOURCE_ROOT){ root = entryRoot(e); return {root}; }
        root = entryRoot(e);
        return applyDiatonicDeviation(entryPreview(e), e, rng);
    }
    static std::vector<float> applyVoicing(const std::vector<float>& values, int mode){
        if(values.size() <= 1) return values;
        auto v = values;
        mode = std::clamp(mode, (int)VOICING_NONE, (int)VOICING_SHELL);
        if(mode == VOICING_NONE) return v;
        std::sort(v.begin(), v.end());
        if(mode == VOICING_CLOSE) return v;
        if(mode == VOICING_OPEN){ for(size_t i = 1; i < v.size(); i += 2) v[i] += 12.0f; std::sort(v.begin(), v.end()); return v; }
        if(mode == VOICING_DROP2){ if(v.size() >= 2){ v[v.size() - 2] -= 12.0f; std::sort(v.begin(), v.end()); } return v; }
        if(mode == VOICING_DROP3){ if(v.size() >= 3){ v[v.size() - 3] -= 12.0f; std::sort(v.begin(), v.end()); } return v; }
        if(v.size() <= 3) return v;
        std::vector<float> shell = {v.front(), v[1], v.back()};
        std::sort(shell.begin(), shell.end());
        return shell;
    }
    static std::vector<float> applyVoicingSpread(const std::vector<float>& values, float spread){
        if(values.size() <= 1 || spread <= 0.0001f) return values;
        auto v = values;
        const float den = (float)(v.size() - 1);
        for(size_t i = 1; i < v.size(); i++) v[i] += spread * (float)i / den;
        return v;
    }
    static std::vector<float> adaptSize(const std::vector<float>& values, int size, bool expand, bool sortOut){
        if(values.empty()) return {0.0f};
        size = std::max(1, size);
        std::vector<float> out;
        if(size <= (int)values.size()) out.assign(values.begin(), values.begin() + size);
        else{
            const int base = (int)values.size();
            for(int i = 0; i < size; i++) out.push_back(values[i % base] + (expand && i >= base ? 12.0f * (float)(i / base) : 0.0f));
        }
        if(sortOut) std::sort(out.begin(), out.end());
        return out;
    }
    // Hungarian assignment: each previous voice to the closest octave of a next note
    static std::vector<float> applyVoiceLeading(const std::vector<float>& prev, const std::vector<float>& next, int minNote, int maxNote){
        if(prev.empty() || next.empty()) return next;
        minNote = std::clamp(minNote, -12, 128); maxNote = std::clamp(maxNote, -12, 128);
        if(minNote > maxNote) std::swap(minNote, maxNote);
        const int n = (int)next.size();
        std::vector<std::vector<double>> cost(n, std::vector<double>(n, 0.0));
        std::vector<std::vector<float>> cand(n, std::vector<float>(n, 0.0f));
        for(int r = 0; r < n; r++){
            const float ref = sampleVector(prev, r);
            for(int c = 0; c < n; c++){
                float bestV = next[c], bestD = std::numeric_limits<float>::max();
                for(int s = -8; s <= 8; s++){
                    const float x = next[c] + (float)(s * 12);
                    if(x < minNote || x > maxNote) continue;
                    const float d = std::abs(x - ref);
                    if(d < bestD){ bestD = d; bestV = x; }
                }
                if(bestD == std::numeric_limits<float>::max())
                    for(int s = -8; s <= 8; s++){
                        const float x = next[c] + (float)(s * 12);
                        const float d = std::abs(x - ref);
                        if(d < bestD){ bestD = d; bestV = x; }
                    }
                cost[r][c] = bestD;
                cand[r][c] = bestV;
            }
        }
        const double INF = std::numeric_limits<double>::max();
        std::vector<double> u(n + 1, 0.0), v(n + 1, 0.0);
        std::vector<int> p(n + 1, 0), way(n + 1, 0);
        for(int i = 1; i <= n; i++){
            p[0] = i;
            int j0 = 0;
            std::vector<double> minv(n + 1, INF);
            std::vector<char> used(n + 1, false);
            do {
                used[j0] = true;
                const int i0 = p[j0];
                double delta = INF;
                int j1 = 0;
                for(int j = 1; j <= n; j++){
                    if(used[j]) continue;
                    const double cur = cost[i0 - 1][j - 1] - u[i0] - v[j];
                    if(cur < minv[j]){ minv[j] = cur; way[j] = j0; }
                    if(minv[j] < delta){ delta = minv[j]; j1 = j; }
                }
                for(int j = 0; j <= n; j++){
                    if(used[j]){ u[p[j]] += delta; v[j] -= delta; } else minv[j] -= delta;
                }
                j0 = j1;
            } while(p[j0] != 0);
            do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while(j0 != 0);
        }
        std::vector<int> assigned(n, 0);
        for(int j = 1; j <= n; j++) if(p[j] > 0) assigned[p[j] - 1] = j - 1;
        std::vector<float> out(n, 0.0f);
        for(int r = 0; r < n; r++) out[r] = cand[r][assigned[r]];
        return out;
    }
    static std::vector<float> applyGravity(const std::vector<float>& prev, const std::vector<float>& next, int minNote, int maxNote, float weight){
        if(next.empty()) return next;
        minNote = std::clamp(minNote, -12, 128); maxNote = std::clamp(maxNote, -12, 128);
        if(minNote > maxNote) std::swap(minNote, maxNote);
        std::vector<float> ep = prev;
        if(ep.empty()) ep.assign(next.size(), 0.0f);
        auto c = applyVoiceLeading(ep, next, minNote, maxNote);
        if(c.empty()) return c;
        std::sort(c.begin(), c.end());
        const float matched = sampleVector(ep, 0), bass = c[0], anchor = std::round(matched / 12.0f) * 12.0f;
        float bestNote = bass, bestTotal = std::numeric_limits<float>::max();
        for(int s = -8; s <= 8; s++){
            const float t = bass + (float)s * 12.0f;
            if(t < (float)minNote || t > (float)maxNote) continue;
            const float total = std::abs(t - matched) + std::abs(t - anchor) * weight;
            if(total < bestTotal){ bestTotal = total; bestNote = t; }
        }
        c[0] = bestNote;
        std::sort(c.begin(), c.end());
        return c;
    }
    static std::vector<float> applyGravityFree(const std::vector<float>& prev, const std::vector<float>& next, int minNote, int maxNote){
        if(next.empty()) return next;
        minNote = std::clamp(minNote, -12, 128); maxNote = std::clamp(maxNote, -12, 128);
        if(minNote > maxNote) std::swap(minNote, maxNote);
        std::vector<float> ep = prev;
        if(ep.empty()) ep.assign(next.size(), 0.0f);
        return applyVoiceLeading(ep, next, minNote, maxNote);
    }
    static std::vector<float> applyRange(const std::vector<float>& values, int minNote, int maxNote){
        if(values.empty()) return values;
        minNote = std::clamp(minNote, -12, 128); maxNote = std::clamp(maxNote, -12, 128);
        if(minNote > maxNote) std::swap(minNote, maxNote);
        auto out = values;
        for(auto& v : out){
            float bestV = v, bestD = std::numeric_limits<float>::max();
            for(int s = -8; s <= 8; s++){
                const float x = v + (float)(s * 12);
                if(x < minNote || x > maxNote) continue;
                const float d = std::abs(x - v);
                if(d < bestD){ bestD = d; bestV = x; }
            }
            if(bestD == std::numeric_limits<float>::max()) bestV = std::clamp(v, (float)minNote, (float)maxNote);
            v = bestV;
        }
        return out;
    }
    // One output's notes for a step (the old buildOutputValues, seeded by rngSeed)
    std::vector<float> buildOutput(const Entry& e, const OutputConfig& c, const std::vector<float>& prev, const Mods& m, uint64_t rngSeed) const {
        Rng rng(rngSeed);
        float root = 0.0f;
        auto values = sourceValues(e, c, root, rng);
        const bool scl = scaleLike(c.sourceMode);
        if(!scl) values = applyInversion(values, m.invert + c.inversion);
        if(m.transpose != 0){ for(auto& v : values) v += (float)m.transpose; root += (float)m.transpose; }
        if(c.fold12) for(auto& v : values) v = pitchClass(v);
        if(!scl){ values = applyVoicing(values, c.voicingMode); values = applyVoicingSpread(values, c.voicingSpread); }
        if(c.rootLess){
            const float rpc = pitchClass(root);
            values.erase(std::remove_if(values.begin(), values.end(), [&](float v){
                const float d = std::abs(pitchClass(v) - rpc);
                return d <= 0.0001f || std::abs(d - 12.0f) <= 0.0001f;
            }), values.end());
        }
        const int bodySize = std::max(0, c.outputSize - ((!scl && c.addBass) ? 1 : 0));
        const int coreSize = (int)values.size();
        const bool deferGrowth = c.expandOutput && c.voiceLeadingMode != VL_OFF && bodySize > coreSize;
        values = bodySize == 0 ? std::vector<float>{} : deferGrowth ? values : adaptSize(values, bodySize, c.expandOutput, false);
        if(c.octaveRandomProbability > 0.0f && c.octaveRandomRange > 0)
            for(auto& v : values){
                if(rng.range(0.0f, 100.0f) < c.octaveRandomProbability){
                    const int options = c.octaveRandomRange * 2;
                    int off = (int)std::floor(rng.range(0.0f, (float)options)) - c.octaveRandomRange;
                    if(off >= 0) off += 1;
                    v += (float)(off * 12);
                }
            }
        values = applyChromaticDeviation(values, c.chromaticDeviationProbability, c.chromaticDeviationRange, rng);
        if(!scl && c.addBass) values.insert(values.begin(), pitchClass(root) + (float)(c.bassOct * 12));
        const float pitchOff = globalPitchBend + c.pitchBend, noteOff = (float)(c.transpose + c.octave * 12);
        for(auto& v : values) v += noteOff + pitchOff;
        if(c.rootLess){
            const float rpc = pitchClass(root + noteOff + pitchOff);
            const bool keepBass = !scl && c.addBass && !values.empty();
            size_t idx = 0;
            values.erase(std::remove_if(values.begin(), values.end(), [&](float v){
                const size_t cur = idx++;
                if(keepBass && cur == 0) return false;
                const float d = std::abs(pitchClass(v) - rpc);
                return d <= 0.0001f || std::abs(d - 12.0f) <= 0.0001f;
            }), values.end());
        }
        switch(c.voiceLeadingMode){
            case VL_CLASSIC: values = applyVoiceLeading(prev, values, c.minNote, c.maxNote); break;
            case VL_GRAVITY: values = applyGravity(prev, values, c.minNote, c.maxNote, c.gravityAnchorWeight); break;
            case VL_GRAVITY_FREE: values = applyGravityFree(prev, values, c.minNote, c.maxNote); break;
            default: break;
        }
        if(deferGrowth){
            bool keepBass = !scl && c.addBass && !values.empty();
            std::vector<float> body = values;
            float bassValue = 0.0f;
            if(keepBass){
                const float expected = pitchClass(root) + (float)(c.bassOct * 12) + noteOff + pitchOff;
                const float epc = pitchClass(expected);
                size_t bi = 0;
                float best = std::numeric_limits<float>::max();
                bool found = false;
                for(size_t i = 0; i < values.size(); i++){
                    float cd = std::abs(pitchClass(values[i]) - epc);
                    cd = std::min(cd, 12.0f - cd);
                    if(cd > 0.01f) continue;
                    const float d = std::abs(values[i] - expected);
                    if(d < best){ best = d; bi = i; found = true; }
                }
                if(found){
                    bassValue = values[bi];
                    body.clear();
                    for(size_t i = 0; i < values.size(); i++) if(i != bi) body.push_back(values[i]);
                }else keepBass = false;
            }
            const auto grown = adaptSize(body, bodySize, true, false);
            values.clear();
            if(keepBass) values.push_back(bassValue);
            values.insert(values.end(), grown.begin(), grown.end());
        }
        values = applyRange(values, c.minNote, c.maxNote);
        if(c.perNoteDetune > 0.0f) for(auto& v : values) v += rng.range(-c.perNoteDetune, c.perNoteDetune);
        if(c.sortOutput) std::sort(values.begin(), values.end());
        return values;
    }
    // The root pitch class a step reports (Root output)
    float rootPitchClass(const Entry& e, const Mods& m) const { return pitchClass(entryRoot(e) + (float)m.transpose); }

    // ── The variations, from counts of their triggers ───────────────────────
    int randomModifier(int range, int quant, uint64_t key) const {
        range = std::max(0, range); quant = std::max(1, quant);
        if(range <= 0) return 0;
        const int maxStep = range / quant;
        if(maxStep <= 0) return 0;
        return std::min(maxStep, (int)(unit(key) * (maxStep + 1))) * quant;
    }
    // stepEvents / lapEvents: how many steps / laps have started, counting this one
    // seqTriggers: the transposition sequencer's triggers so far
    Mods modsFor(int64_t stepEvents, int64_t lapEvents, int64_t seqTriggers) const {
        Mods m;
        auto drawn = [&](bool perStep, int cycles, int range, int quant, uint64_t tag){
            const int64_t events = perStep ? stepEvents : lapEvents;
            const int64_t r = events / std::max(1, cycles);
            return r <= 0 ? 0 : randomModifier(range, quant, H(seed, (uint64_t)r, tag, (uint64_t)perStep));
        };
        m.randomTranspose = drawn(transposeRandomStep, transposeRandomCycles, transposeRandomRange, transposeRandomQuantization, 0x7A);
        m.randomInvert = drawn(inversionRandomStep, inversionRandomCycles, inversionRandomRange, inversionRandomQuantization, 0x1B);
        // the sequencer: every Cycles triggers, the next value of its list
        const int64_t advances = seqTriggers / std::max(1, transposeSequencerCycles);
        int64_t sum = 0;
        const int64_t n = (int64_t)transposeSequencerSteps.size();
        if(n > 0 && advances > 0){
            int64_t full = 0;
            for(int v : transposeSequencerSteps) full += v;
            sum = (advances / n) * full;
            for(int64_t i = 0; i < advances % n; i++) sum += transposeSequencerSteps[i];
        }
        if(transposeSequencerMod > 0){ sum %= transposeSequencerMod; if(sum < 0) sum += transposeSequencerMod; }
        m.sequencer = (int)sum;
        m.transpose = globalTranspose + m.randomTranspose + m.sequencer;
        m.invert = globalInvert + m.randomInvert;
        return m;
    }

    // ── The timed progression ───────────────────────────────────────────────
    struct Event {
        int64_t k = 0;              // the step (0: at the origin)
        int entry = 0;
        double start = 0.0, dur = 1.0;     // beats
        double lapStart = 0.0, lapLen = 1.0;
        Mods mods;
        std::vector<std::vector<float>> outs;
        float root = 0.0f;
        int64_t stepEvents = 0, lapEvents = 0, seqTriggers = 0;
    };
    // Forget the timeline (anything changed)
    void invalidate(){ events.clear(); }
    double lapBeats() const {
        double t = 0.0;
        for(const auto& e : progression) t += std::max(0.001f, e.beatDuration);
        return std::max(0.001, t);
    }
    // The step playing at a beat (before the origin: step 0)
    const Event& eventAt(double beat){
        if(beat < originBeat) return event(0);
        ensureUntil(beat);
        // the last event starting at or before beat
        size_t lo = 0, hi = events.size();
        while(hi - lo > 1){
            const size_t mid = (lo + hi) / 2;
            if(events[mid].start <= beat + 1e-9) lo = mid; else hi = mid;
        }
        return events[lo];
    }
    const Event& event(int64_t k){
        while((int64_t)events.size() <= k) append();
        return events[(size_t)std::max<int64_t>(0, k)];
    }
    // Events starting in [from, to)
    std::vector<const Event*> eventsIn(double from, double to){
        std::vector<const Event*> out;
        if(to <= originBeat) return out;
        ensureUntil(to);
        for(const auto& e : events) if(e.start >= from && e.start < to) out.push_back(&e);
        return out;
    }
    size_t cachedEvents() const { return events.size(); }

    // ── The Index order: computed as it plays (voice leading from the last) ─
    struct Live {
        int lastIndex = -1;
        int64_t stepEvents = 0, lapEvents = 0, seqTriggers = 0, changes = 0;
        std::vector<std::vector<float>> outs;
    };
    // An index arrives: the notes (prev = what was playing), as the old node
    std::vector<std::vector<float>> playIndex(Live& live, int index, bool force){
        const int n = (int)progression.size();
        const int e = wrapIndex(index, n);
        const bool changed = live.lastIndex < 0 || e != live.lastIndex;
        if(changed || force){
            live.stepEvents++;
            if(live.lastIndex < 0 || e == 0) live.lapEvents++;
            if(live.lastIndex >= 0 && e != live.lastIndex){
                if(transposeSequencerStepMode || e == 0) live.seqTriggers++;
            }
            live.changes++;
        }
        const Mods m = modsFor(live.stepEvents, live.lapEvents, live.seqTriggers);
        std::vector<std::vector<float>> outs(outputs.size());
        for(size_t o = 0; o < outputs.size(); o++){
            const std::vector<float> prev = (force || live.outs.size() != outputs.size()) ? std::vector<float>{} : live.outs[o];
            outs[o] = buildOutput(progression[e], outputs[o], prev, m, H(seed, (uint64_t)live.changes, o + 1, 0x0B));
        }
        live.lastIndex = e;
        live.outs = outs;
        lastIndexMods = m;
        return outs;
    }
    Mods lastIndexMods;

    // Each entry's notes in order (the Index cable's table: voice leading from the row before)
    std::vector<std::vector<std::vector<float>>> indexTable(){
        const int n = (int)progression.size();
        const Mods m = modsFor(1, 1, 0);
        std::vector<std::vector<std::vector<float>>> rows(n, std::vector<std::vector<float>>(outputs.size()));
        std::vector<std::vector<float>> prev(outputs.size());
        for(int pass = 0; pass < 2; pass++)   // twice: row 0 follows the last one
            for(int i = 0; i < n; i++)
                for(size_t o = 0; o < outputs.size(); o++){
                    rows[i][o] = buildOutput(progression[i], outputs[o], prev[o], m, H(seed, (uint64_t)i, o + 1, 0x7AB));
                    prev[o] = rows[i][o];
                }
        return rows;
    }

private:
    std::vector<Event> events;

    void ensureUntil(double beat){
        int guard = 0;
        while((events.empty() || events.back().start + events.back().dur <= beat) && guard++ < 200000) append();
    }
    int nextEntry(int current, int64_t k) const {
        const int n = (int)progression.size();
        if(n <= 0) return 0;
        switch(progressionOrder){
            case ORDER_DESC: return wrapIndex(current - 1, n);
            case ORDER_RANDOM: return std::min(n - 1, (int)(unit(H(seed, (uint64_t)k, 0x9A)) * n));
            case ORDER_MARKOV: {
                const auto& w = progression[current].markovWeights;
                float sum = 0.0f;
                for(int i = 0; i < n && i < (int)w.size(); i++) sum += std::max(0.0f, w[i]);
                if(sum <= 0.0001f) return (current + 1) % n;
                const float threshold = (float)unit(H(seed, (uint64_t)k, 0x3C)) * sum;
                float acc = 0.0f;
                for(int i = 0; i < n && i < (int)w.size(); i++){ acc += std::max(0.0f, w[i]); if(threshold <= acc) return i; }
                return (current + 1) % n;
            }
            default: return (current + 1) % n;
        }
    }
    void append(){
        Event ev;
        if(events.empty()){
            ev.k = 0;
            ev.entry = 0;
            ev.start = originBeat;
            ev.stepEvents = 1;
            ev.lapEvents = 1;
            ev.seqTriggers = 0;
            ev.lapStart = originBeat;
        }else{
            const Event& p = events.back();
            ev.k = p.k + 1;
            ev.entry = nextEntry(p.entry, ev.k);
            ev.start = p.start + p.dur;
            ev.stepEvents = p.stepEvents + 1;
            const bool lap = ev.entry == 0 && (p.entry != 0 || progression.size() == 1);
            ev.lapEvents = p.lapEvents + (lap ? 1 : 0);
            const bool changed = ev.entry != p.entry || progression.size() == 1;
            ev.seqTriggers = p.seqTriggers + ((transposeSequencerStepMode ? changed : lap) ? 1 : 0);
            ev.lapStart = lap ? ev.start : p.lapStart;
        }
        ev.dur = std::max(0.001, (double)progression[std::clamp(ev.entry, 0, (int)progression.size() - 1)].beatDuration);
        ev.lapLen = progressionOrder == ORDER_ASC || progressionOrder == ORDER_DESC ? lapBeats() : 0.0;
        ev.mods = modsFor(ev.stepEvents, ev.lapEvents, ev.seqTriggers);
        const Entry& e = progression[std::clamp(ev.entry, 0, (int)progression.size() - 1)];
        ev.outs.resize(outputs.size());
        for(size_t o = 0; o < outputs.size(); o++){
            const std::vector<float> prev = events.empty() ? std::vector<float>{} : events.back().outs[o];
            ev.outs[o] = buildOutput(e, outputs[o], prev, ev.mods, H(seed, (uint64_t)ev.k, o + 1, 0x5E));
        }
        ev.root = rootPitchClass(e, ev.mods);
        events.push_back(std::move(ev));
    }
};

} // namespace chordbox
