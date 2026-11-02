//
//  scHarmonyBoxEngine.h
//  ofxOceanodeSuperCollider
//
//  HarmonyBox's engine, free of openFrameworks so tools/harmonybox_test can
//  build it alone. It makes a bank of chords that work together; the node
//  plays one of them per Index.
//
//  • Chords: a root, intervals above it (extensions keep their octave: 14 is
//    a ninth, so voicing knows what to keep) and an optional slash bass.
//    Symbols parse and print ("Dm9", "F/C", "G7alt", "D(2,4,5)").
//  • Styles propose candidate progressions (seeded):
//      Functional   Markov walk over the degrees, secondary dominants and
//                   borrowed chords with Color
//      Jazz         targets reached by ii-V, V, tritone subs, backdoor
//      Glass        a short cell (i-VI, i-bVII-bVI-V...) repeated with one
//                   change per pass, pedal bass and inversions
//      Reich        a modal field of seconds and fourths over a moving bass
//      Neo-Riemann  P / L / R walks (N, S, H with Color)
//    Each candidate is scored (smoothness of the whole loop, variety, the
//    tension level and shape asked for, the style's own likelihood) and the
//    best of a few hundred wins. Locked slots stay as they are.
//  • Voicing: the inversion and octave that move least from the chord
//    before, in range, with a separate bass, in close / drop 2 / open / wide.
//

#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace harmonybox {

constexpr int MAX_CHORDS = 16, MAX_VOICES = 8, CANDIDATES = 160;
constexpr float TEMPERATURE = 0.35f;   // how freely a good candidate beats the best one

enum Style { STYLE_FUNCTIONAL, STYLE_JAZZ, STYLE_GLASS, STYLE_REICH, STYLE_NEORIEMANN, NUM_STYLES };
enum Mode { MODE_MAJOR, MODE_DORIAN, MODE_PHRYGIAN, MODE_LYDIAN, MODE_MIXOLYDIAN, MODE_MINOR, MODE_LOCRIAN,
            MODE_HARMONIC, MODE_MELODIC, NUM_MODES };
enum Shape { SHAPE_FREE, SHAPE_FLAT, SHAPE_RISE, SHAPE_FALL, SHAPE_ARC, SHAPE_WAVE, NUM_SHAPES };
enum Spread { SPREAD_CLOSE, SPREAD_DROP2, SPREAD_OPEN, SPREAD_WIDE, NUM_SPREADS };

inline const std::vector<std::string>& styleNames(){
    static const std::vector<std::string> v{"Functional", "Jazz", "Glass", "Reich", "Neo-Riemann"}; return v;
}
inline const std::vector<std::string>& modeNames(){
    static const std::vector<std::string> v{"Major", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Minor", "Locrian",
                                            "Harmonic Minor", "Melodic Minor"}; return v;
}
inline const std::vector<std::string>& shapeNames(){
    static const std::vector<std::string> v{"Free", "Flat", "Rise", "Fall", "Arc", "Wave"}; return v;
}
inline const std::vector<std::string>& spreadNames(){
    static const std::vector<std::string> v{"Close", "Drop 2", "Open", "Wide"}; return v;
}
inline const std::vector<std::string>& keyNames(){
    static const std::vector<std::string> v{"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"}; return v;
}

inline const std::array<int, 7>& modeScale(int mode){
    static const std::array<std::array<int, 7>, NUM_MODES> s{{
        {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11},
        {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 10}, {0, 1, 3, 5, 6, 8, 10}, {0, 2, 3, 5, 7, 8, 11},
        {0, 2, 3, 5, 7, 9, 11}}};
    return s[std::clamp(mode, 0, NUM_MODES - 1)];
}
inline bool minorMode(int mode){ return modeScale(mode)[2] == 3; }
inline int pc(int v){ return ((v % 12) + 12) % 12; }

// Flats or sharps: the key signature of the mode's relative major
inline bool flatKey(int tonic, int mode){
    static const int rel[NUM_MODES] = {0, 2, 4, 5, 7, 9, 11, 9, 9};
    const int major = pc(tonic - rel[std::clamp(mode, 0, NUM_MODES - 1)]);
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1 || major == 6;
}
inline std::string pcName(int p, bool flats){
    static const char* sh[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* fl[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    return (flats ? fl : sh)[pc(p)];
}

// ── Random ──────────────────────────────────────────────────────────────────
struct Rng {
    uint64_t state;
    // seeds next to each other must not give the same stream one step apart
    explicit Rng(uint64_t s) : state(0x632BE59BD9B4E019ull) { state ^= s; state = next(); state ^= s * 0xD1B54A32D192ED03ull; next(); }
    uint64_t next(){
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float uniform(){ return (float)((next() >> 11) * (1.0 / 9007199254740992.0)); }
    int below(int n){ return n <= 1 ? 0 : (int)(next() % (uint64_t)n); }
    bool chance(float p){ return uniform() < p; }
    int weighted(const std::vector<float>& w){
        float sum = 0.0f;
        for(float x : w) sum += std::max(0.0f, x);
        if(sum <= 0.0f) return below((int)w.size());
        float r = uniform() * sum;
        for(size_t i = 0; i < w.size(); i++){ r -= std::max(0.0f, w[i]); if(r <= 0.0f) return (int)i; }
        return (int)w.size() - 1;
    }
};

// ── Chord qualities ─────────────────────────────────────────────────────────
struct Quality { std::string name; std::vector<int> iv; };

// Order matters: naming a set of notes takes the first match
inline const std::vector<Quality>& qualities(){
    static const std::vector<Quality> q{
        {"", {0, 4, 7}}, {"m", {0, 3, 7}}, {"dim", {0, 3, 6}}, {"aug", {0, 4, 8}}, {"sus4", {0, 5, 7}},
        {"sus2", {0, 2, 7}}, {"5", {0, 7}},
        {"maj7", {0, 4, 7, 11}}, {"m7", {0, 3, 7, 10}}, {"7", {0, 4, 7, 10}}, {"6", {0, 4, 7, 9}},
        {"m6", {0, 3, 7, 9}}, {"m7b5", {0, 3, 6, 10}}, {"dim7", {0, 3, 6, 9}}, {"mMaj7", {0, 3, 7, 11}},
        {"7sus4", {0, 5, 7, 10}}, {"maj7sus2", {0, 2, 7, 11}}, {"sus24", {0, 2, 5, 7}}, {"7#5", {0, 4, 8, 10}},
        {"maj7#5", {0, 4, 8, 11}}, {"7b5", {0, 4, 6, 10}}, {"add9", {0, 4, 7, 14}}, {"madd9", {0, 3, 7, 14}},
        {"add11", {0, 4, 7, 17}}, {"madd11", {0, 3, 7, 17}}, {"q", {0, 5, 10}}, {"q4", {0, 5, 10, 15}},
        {"maj9", {0, 4, 7, 11, 14}}, {"m9", {0, 3, 7, 10, 14}}, {"9", {0, 4, 7, 10, 14}}, {"6/9", {0, 4, 7, 9, 14}},
        {"m6/9", {0, 3, 7, 9, 14}}, {"7b9", {0, 4, 7, 10, 13}}, {"7#9", {0, 4, 7, 10, 15}}, {"9sus4", {0, 5, 7, 10, 14}},
        {"m9b5", {0, 3, 6, 10, 14}}, {"m7b9", {0, 3, 7, 10, 13}}, {"mMaj9", {0, 3, 7, 11, 14}},
        {"7#11", {0, 4, 7, 10, 18}}, {"maj7#11", {0, 4, 7, 11, 18}}, {"7b13", {0, 4, 7, 10, 20}},
        {"m11", {0, 3, 7, 10, 14, 17}}, {"11", {0, 7, 10, 14, 17}}, {"maj9#11", {0, 4, 7, 11, 14, 18}},
        {"9#11", {0, 4, 7, 10, 14, 18}}, {"13", {0, 4, 7, 10, 14, 21}}, {"maj13", {0, 4, 7, 11, 14, 21}},
        {"m13", {0, 3, 7, 10, 14, 21}}, {"13b9", {0, 4, 10, 13, 21}}, {"7b9b13", {0, 4, 10, 13, 20}},
        {"7alt", {0, 4, 10, 13, 15, 20}}, {"m13", {0, 3, 7, 10, 14, 17, 21}}, {"maj13#11", {0, 4, 7, 11, 14, 18, 21}},
        {"13#11", {0, 4, 7, 10, 14, 18, 21}}};
    return q;
}
inline const Quality* findQuality(const std::string& name){
    for(const auto& q : qualities()) if(q.name == name) return &q;
    return nullptr;
}
inline const std::map<std::string, std::string>& qualityAliases(){
    static const std::map<std::string, std::string> a{
        {"M", ""}, {"maj", ""}, {"major", ""}, {"min", "m"}, {"mi", "m"}, {"-", "m"}, {"minor", "m"},
        {"M7", "maj7"}, {"Maj7", "maj7"}, {"ma7", "maj7"}, {"Δ", "maj7"}, {"Δ7", "maj7"}, {"^7", "maj7"},
        {"-7", "m7"}, {"min7", "m7"}, {"mi7", "m7"}, {"ø", "m7b5"}, {"ø7", "m7b5"}, {"m7(b5)", "m7b5"},
        {"-7b5", "m7b5"}, {"°", "dim"}, {"o", "dim"}, {"°7", "dim7"}, {"o7", "dim7"}, {"+", "aug"},
        {"+7", "7#5"}, {"aug7", "7#5"}, {"sus", "sus4"}, {"M9", "maj9"}, {"Maj9", "maj9"}, {"Δ9", "maj9"},
        {"-9", "m9"}, {"min9", "m9"}, {"add2", "add9"}, {"madd2", "madd9"}, {"-add9", "madd9"}, {"mM7", "mMaj7"},
        {"m(maj7)", "mMaj7"}, {"-maj7", "mMaj7"}, {"mmaj7", "mMaj7"}, {"69", "6/9"}, {"m69", "m6/9"}, {"alt", "7alt"},
        {"7(b9)", "7b9"}, {"7(#9)", "7#9"}, {"7(#11)", "7#11"}, {"maj7(#11)", "maj7#11"}, {"-11", "m11"},
        {"min11", "m11"}, {"-13", "m13"}, {"M13", "maj13"}, {"7sus", "7sus4"}, {"9sus", "9sus4"},
        {"sus2sus4", "sus24"}, {"susb9", "7b9"}, {"-6", "m6"}, {"min6", "m6"}, {"M7#11", "maj7#11"},
        {"Maj7#11", "maj7#11"}, {"quartal", "q"}, {"4ths", "q4"}};
    return a;
}

inline std::vector<int> pcSetOf(const std::vector<int>& iv){
    std::set<int> s;
    for(int v : iv) s.insert(pc(v));
    return {s.begin(), s.end()};
}

// Pitch-class bits of intervals from 0, and the table's, computed once
inline uint16_t ivMask(const std::vector<int>& iv){
    uint16_t m = 0;
    for(int v : iv) m |= (uint16_t)(1u << pc(v));
    return m;
}
inline const std::vector<uint16_t>& qualityMasks(){
    static const std::vector<uint16_t> m = [](){
        std::vector<uint16_t> v;
        for(const auto& q : qualities()) v.push_back(ivMask(q.iv));
        return v;
    }();
    return m;
}
// bits seen from another root (bit p -> bit p - r)
inline uint16_t rotateMask(uint16_t m, int r){
    r = pc(r);
    return (uint16_t)(((m >> r) | (m << (12 - r))) & 0xFFF);
}

// Fallback interval names (and what parses back): semitones above the root
inline std::string degreeLabel(int semis){
    static const char* l[12] = {"1", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7"};
    return l[pc(semis)];
}
inline int parseDegree(const std::string& s){
    static const std::map<std::string, int> d{
        {"1", 0}, {"b2", 1}, {"2", 2}, {"#2", 3}, {"b3", 3}, {"3", 4}, {"4", 5}, {"#4", 6}, {"b5", 6}, {"5", 7},
        {"#5", 8}, {"b6", 8}, {"6", 9}, {"bb7", 9}, {"b7", 10}, {"7", 11}, {"b9", 13}, {"9", 14}, {"#9", 15},
        {"11", 17}, {"#11", 18}, {"b13", 20}, {"13", 21}};
    auto it = d.find(s);
    return it == d.end() ? -1 : it->second;
}

// ── Chord ───────────────────────────────────────────────────────────────────
struct Chord {
    int root = 0;
    int bass = -1;                    // slash bass pitch class, -1 = the root
    std::vector<int> iv{0, 4, 7};     // semitones above the root, sorted, with 0
    std::string quality;              // its symbol after the root

    int bassPc() const { return bass < 0 ? root : bass; }
    std::vector<int> pcs() const {
        std::set<int> s;
        for(int v : iv) s.insert(pc(root + v));
        s.insert(bassPc());
        return {s.begin(), s.end()};
    }
    // the notes as 12 bits (bit p = pitch class p), bass included
    uint16_t mask() const {
        uint16_t m = (uint16_t)(1u << bassPc());
        for(int v : iv) m |= (uint16_t)(1u << pc(root + v));
        return m;
    }
    bool has(int semis) const { for(int v : iv) if(pc(v) == pc(semis)) return true; return false; }
    bool isMinor() const { return has(3) && !has(4); }
    bool isDiminished() const { return has(3) && has(6) && !has(7); }
    bool isDominant() const { return has(4) && has(10); }
    std::string name(bool flats) const {
        std::string n = pcName(root, flats) + quality;
        if(bass >= 0 && pc(bass) != pc(root)) n += "/" + pcName(bass, flats);
        return n;
    }
    bool same(const Chord& o) const { return mask() == o.mask() && bassPc() == o.bassPc(); }
};

inline Chord makeChord(int root, const std::string& quality, int bass = -1){
    Chord c;
    c.root = pc(root);
    const Quality* q = findQuality(quality);
    if(!q) q = &qualities()[0];
    c.iv = q->iv;
    c.quality = q->name;
    c.bass = bass < 0 || pc(bass) == c.root ? -1 : pc(bass);
    return c;
}

// A chord from intervals: the table's name for the same notes, else "(2,4,5)"
inline Chord fromIntervals(int root, std::vector<int> iv, int bass = -1){
    iv.push_back(0);
    const auto set = pcSetOf(iv);
    const uint16_t m = ivMask(iv);
    for(size_t i = 0; i < qualities().size(); i++)
        if(qualityMasks()[i] == m) return makeChord(root, qualities()[i].name, bass);
    Chord c;
    c.root = pc(root);
    std::sort(iv.begin(), iv.end());
    iv.erase(std::unique(iv.begin(), iv.end()), iv.end());
    c.iv = iv;
    std::string q = "(";
    bool first = true;
    for(int v : set) if(v != 0){ q += (first ? "" : ",") + degreeLabel(v); first = false; }
    c.quality = q + ")";
    c.bass = bass < 0 || pc(bass) == c.root ? -1 : pc(bass);
    return c;
}

// The chord that a set of pitch classes over a bass is: the bass as root
// first, then any other root with the bass as slash
inline Chord fromPcs(const std::vector<int>& pcsIn, int bassPc){
    uint16_t all = (uint16_t)(1u << pc(bassPc));
    for(int p : pcsIn) all |= (uint16_t)(1u << pc(p));
    const auto& qm = qualityMasks();
    const uint16_t fromBass = rotateMask(all, bassPc);
    for(size_t i = 0; i < qm.size(); i++) if(qm[i] == fromBass) return makeChord(bassPc, qualities()[i].name);
    for(size_t i = 0; i < qm.size(); i++){
        if(qualities()[i].iv.size() < 3) continue;
        for(int r = 0; r < 12; r++){
            if(r == pc(bassPc) || !(all >> r & 1)) continue;
            if(qm[i] == rotateMask(all, r)) return makeChord(r, qualities()[i].name, bassPc);
        }
    }
    std::vector<int> iv;
    for(int v = 1; v < 12; v++) if(fromBass >> v & 1) iv.push_back(v);
    return fromIntervals(bassPc, iv);
}

inline std::string trim(const std::string& s){
    size_t a = 0, b = s.size();
    while(a < b && std::isspace((unsigned char)s[a])) a++;
    while(b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
inline std::string lower(std::string s){ for(auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }

// A note name at the start: C, C#, Db, Bb... (UTF-8 ♯ ♭ too). -1 when none
inline int parseNote(const std::string& s, size_t& pos){
    if(pos >= s.size()) return -1;
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};
    const char ch = (char)std::toupper((unsigned char)s[pos]);
    if(ch < 'A' || ch > 'G') return -1;
    int v = base[ch - 'A'];
    pos++;
    while(pos < s.size()){
        if(s[pos] == '#'){ v++; pos++; }
        else if(s[pos] == 'b'){ v--; pos++; }
        else if(s.compare(pos, 3, "♯") == 0){ v++; pos += 3; }
        else if(s.compare(pos, 3, "♭") == 0){ v--; pos += 3; }
        else break;
    }
    return pc(v);
}

inline bool parseQuality(const std::string& qIn, std::string& out, std::vector<int>& custom){
    custom.clear();
    std::string q = qIn;
    if(findQuality(q)){ out = q; return true; }
    auto a = qualityAliases().find(q);
    if(a != qualityAliases().end()){ out = a->second; return true; }
    if(q.size() > 1){   // case-insensitive, never for the lone m / M
        const std::string lq = lower(q);
        for(const auto& x : qualities()) if(lower(x.name) == lq){ out = x.name; return true; }
        for(const auto& [k, v] : qualityAliases()) if(k.size() > 1 && lower(k) == lq){ out = v; return true; }
    }
    // "(2,4,5)": intervals by name
    if(q.size() >= 2 && q.front() == '(' && q.back() == ')'){
        std::string body = q.substr(1, q.size() - 2), tok;
        custom.push_back(0);
        for(size_t i = 0; i <= body.size(); i++){
            if(i == body.size() || body[i] == ',' || body[i] == ' '){
                if(!tok.empty()){ const int d = parseDegree(tok); if(d < 0) return false; custom.push_back(d); }
                tok.clear();
            }else tok += body[i];
        }
        return custom.size() > 1;
    }
    return false;
}

inline bool parseChord(const std::string& textIn, Chord& out){
    std::string text;
    for(char ch : trim(textIn)) if(ch != ' ') text += ch;
    size_t pos = 0;
    const int root = parseNote(text, pos);
    if(root < 0) return false;
    std::string rest = text.substr(pos);
    int bass = -1;
    const size_t slash = rest.rfind('/');
    if(slash != std::string::npos){
        size_t p = 0;
        const std::string after = rest.substr(slash + 1);
        const int b = parseNote(after, p);
        if(b >= 0 && p == after.size()){ bass = b; rest = rest.substr(0, slash); }
    }
    std::string q;
    std::vector<int> custom;
    if(!parseQuality(rest, q, custom)) return false;
    out = custom.empty() ? makeChord(root, q, bass) : fromIntervals(root, custom, bass);
    return true;
}

// A progression in text (one per line, or separated by spaces , | ;), as the
// AI or a paste gives it. An optional "Key: A minor" line sets key and mode.
inline std::vector<Chord> parseProgression(const std::string& text, int* keyOut = nullptr, int* modeOut = nullptr){
    std::vector<Chord> out;
    size_t start = 0;
    while(start <= text.size()){
        size_t end = text.find('\n', start);
        if(end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        if(line.empty()) continue;
        const std::string ll = lower(line);
        if(ll.rfind("key", 0) == 0){
            size_t c = line.find(':');
            std::string v = trim(c == std::string::npos ? line.substr(3) : line.substr(c + 1));
            size_t p = 0;
            const int k = parseNote(v, p);
            if(k >= 0 && keyOut) *keyOut = k;
            const std::string m = lower(trim(v.substr(p)));
            if(modeOut && !m.empty()){
                int best = -1;
                for(int i = 0; i < NUM_MODES; i++) if(lower(modeNames()[i]) == m) best = i;
                if(best < 0){
                    if(m == "major" || m == "ionian") best = MODE_MAJOR;
                    else if(m == "minor" || m == "aeolian" || m == "natural minor" || m == "m") best = MODE_MINOR;
                    else for(int i = 0; i < NUM_MODES; i++) if(m.find(lower(modeNames()[i])) != std::string::npos) best = i;
                }
                if(best >= 0) *modeOut = best;
            }
            continue;
        }
        std::string tok;
        auto flush = [&](){
            std::string t = tok;
            tok.clear();
            while(!t.empty() && (t.back() == '.' || t.back() == '"' || t.back() == '\'' || t.back() == ']' || t.back() == '*')) t.pop_back();
            while(!t.empty() && (t.front() == '"' || t.front() == '\'' || t.front() == '[' || t.front() == '*')) t.erase(0, 1);
            Chord c;
            if(!t.empty() && parseChord(t, c) && (int)out.size() < MAX_CHORDS) out.push_back(c);
        };
        for(char ch : line){
            if(std::isspace((unsigned char)ch) || ch == ',' || ch == '|' || ch == ';') flush();
            else tok += ch;
        }
        flush();
    }
    return out;
}

// ── Key ─────────────────────────────────────────────────────────────────────
struct Key {
    int tonic = 0, mode = MODE_MAJOR;
    std::array<int, 7> scale() const { auto s = modeScale(mode); for(auto& v : s) v = pc(v + tonic); return s; }
    bool contains(int p) const { for(int v : scale()) if(v == pc(p)) return true; return false; }
    bool flats() const { return flatKey(tonic, mode); }
};

// The chord built in thirds on a degree of a scale (pitch classes from the
// tonic), `size` notes, leaving out the avoid notes (b9 and b13 off
// dominants, the 11th over a major third)
inline Chord diatonic(int tonic, const std::array<int, 7>& rel, int degree, int size){
    std::vector<int> iv;
    const int base = rel[degree % 7];
    const bool major3 = pc(rel[(degree + 2) % 7] - base) == 4;
    const bool dom = major3 && size >= 4 && pc(rel[(degree + 6) % 7] - base) == 10;
    for(int k = 0; k < size; k++){
        const int step = degree + 2 * k;
        int v = rel[step % 7] + 12 * (step / 7) - base;
        while(v < 0) v += 12;
        if(k >= 4) while(v < 12) v += 12;
        if(k == 4 && v == 13 && !dom) continue;
        if(k == 5 && v == 17 && major3) continue;
        if(k == 6 && v == 20 && !dom) continue;
        iv.push_back(v);
    }
    return fromIntervals(tonic + base, iv);
}

// Roman numeral of a chord in a key (degree of the mode, b / # when outside)
inline std::string roman(const Chord& c, const Key& k){
    static const char* up[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    const auto rel = modeScale(k.mode);
    const int r = pc(c.root - k.tonic);
    std::string acc, num;
    int deg = -1;
    for(int i = 0; i < 7; i++) if(rel[i] == r) deg = i;
    if(deg < 0){
        // flat, except a flat tonic: the raised seventh is #VII
        int fl = -1, sh = -1;
        for(int i = 0; i < 7; i++){ if(pc(rel[i] - 1) == r) fl = i; if(pc(rel[i] + 1) == r) sh = i; }
        if(fl > 0 || (fl == 0 && sh < 0)){ deg = fl; acc = "b"; }
        else if(sh >= 0){ deg = sh; acc = "#"; }
        else deg = 0;
    }
    num = up[deg];
    if(c.isMinor()) for(auto& ch : num) ch = (char)std::tolower((unsigned char)ch);
    std::string suf;
    if(c.quality == "m7b5" || c.quality == "m9b5") suf = "ø";
    else if(c.isDiminished()) suf = c.has(9) ? "°7" : "°";
    else if(c.quality.rfind("aug", 0) == 0 || c.quality == "7#5" || c.quality == "maj7#5") suf = "+";
    if(c.has(10) && suf != "ø") suf += "7";
    else if(c.has(11)) suf += "Δ";
    else if(c.quality.find("sus") != std::string::npos) suf += "sus";
    std::string out = acc + num + suf;
    if(c.bass >= 0){
        // inversion figures for chord tones, else the bass as a degree
        const int b = pc(c.bass - c.root);
        if(b == 3 || b == 4) out += "6";
        else if(b == 7) out += "64";
        else out += "/" + pcName(c.bass, k.flats());
    }
    return out;
}

// Flats or sharps for a chord in a key: the key's for its own notes, a
// chromatic root by how its numeral reads (bVI flat, #vii° sharp)
inline bool spellFlats(const Chord& c, const Key& k){
    if(k.contains(c.root)) return k.flats();
    return roman(c, k).find('#') == std::string::npos;
}

// ── Measures ────────────────────────────────────────────────────────────────

// How far two chords' notes are, on average (0 = same notes)
inline int maskCount(uint16_t m){ int n = 0; for(; m; m &= (uint16_t)(m - 1)) n++; return n; }
inline float pcDistance(const Chord& a, const Chord& b){
    const uint16_t A = a.mask(), B = b.mask();
    // nearest note of the other set, by rings of 0..6 semitones
    auto nearest = [](int p, uint16_t S){
        for(int d = 0; d <= 6; d++) if((S >> pc(p + d) & 1) || (S >> pc(p - d) & 1)) return d;
        return 6;
    };
    int sum = 0;
    for(int p = 0; p < 12; p++){
        if(A >> p & 1) sum += nearest(p, B);
        if(B >> p & 1) sum += nearest(p, A);
    }
    const int bassMove = pc(a.bassPc() - b.bassPc());
    return (float)sum / (float)(maskCount(A) + maskCount(B)) + 0.05f * (float)std::min(bassMove, 12 - bassMove);
}

// 0..1: notes outside the key, semitone clashes, tritones, dominant function
inline float tensionOf(const Chord& c, const Key& k){
    const uint16_t m = c.mask();
    uint16_t keyMask = 0;
    for(int v : k.scale()) keyMask |= (uint16_t)(1u << v);
    const int outside = maskCount((uint16_t)(m & ~keyMask));
    int clashes = 0, tritones = 0;
    for(int p = 0; p < 12; p++){
        if(!(m >> p & 1)) continue;
        if(m >> pc(p + 1) & 1) clashes++;
        if(p < 6 && (m >> (p + 6) & 1)) tritones++;
    }
    const int size = maskCount(m);
    float t = 0.16f * outside + 0.07f * clashes + 0.09f * tritones + (c.isDominant() ? 0.18f : 0.0f)
            + 0.04f * std::max(0, size - 3) + (pc(c.root) != pc(k.tonic) ? 0.12f : 0.0f)
            + (c.bass >= 0 ? 0.05f : 0.0f) + (c.isDiminished() ? 0.12f : 0.0f);
    return std::clamp(t, 0.0f, 1.0f);
}

// The scale to play over a chord: the key's, bent to the chord's own notes.
// Pitch classes relative to the chord's root.
inline std::vector<float> chordScale(const Chord& c, const Key& k){
    std::set<int> s;
    for(int v : k.scale()) s.insert(v);
    const auto C = c.pcs();
    std::set<int> cs(C.begin(), C.end());
    // three chromatic notes in a row: how un-scale-like a set is
    auto clusters = [](const std::set<int>& x){
        int n = 0;
        for(int p : x) if(x.count(pc(p + 1)) && x.count(pc(p + 2))) n++;
        return n;
    };
    for(int p : C){
        if(s.count(p)) continue;
        s.insert(p);
        // drop the neighbour it replaces (F for F#): the choice with fewest clusters
        std::set<int> best = s;
        int bestScore = clusters(s) * 10 + 1;
        for(int n : {pc(p - 1), pc(p + 1)}){
            if(cs.count(n) || !s.count(n)) continue;
            std::set<int> t = s;
            t.erase(n);
            const int sc = clusters(t) * 10;
            if(sc < bestScore){ bestScore = sc; best = t; }
        }
        s = best;
    }
    std::vector<float> out;
    for(int p : s) out.push_back((float)pc(p - c.root));
    std::sort(out.begin(), out.end());
    return out;
}

// ── Settings ────────────────────────────────────────────────────────────────
struct Settings {
    int style = STYLE_FUNCTIONAL, key = 0, mode = MODE_MAJOR, numChords = 4;
    float color = 0.3f, extensions = 0.4f, tension = 0.4f;
    int shape = SHAPE_FREE;
    int voices = 4, low = 40, high = 84, spread = SPREAD_CLOSE;
    bool bass = true, voiceLeading = true;

    nlohmann::json toJson() const {
        return {{"style", style}, {"key", key}, {"mode", mode}, {"numChords", numChords}, {"color", color},
                {"extensions", extensions}, {"tension", tension}, {"shape", shape}, {"voices", voices}, {"low", low},
                {"high", high}, {"spread", spread}, {"bass", bass}, {"voiceLeading", voiceLeading}};
    }
    void fromJson(const nlohmann::json& j){
        style = std::clamp(j.value("style", style), 0, NUM_STYLES - 1);
        key = pc(j.value("key", key));
        mode = std::clamp(j.value("mode", mode), 0, NUM_MODES - 1);
        numChords = std::clamp(j.value("numChords", numChords), 1, MAX_CHORDS);
        color = std::clamp(j.value("color", color), 0.0f, 1.0f);
        extensions = std::clamp(j.value("extensions", extensions), 0.0f, 1.0f);
        tension = std::clamp(j.value("tension", tension), 0.0f, 1.0f);
        shape = std::clamp(j.value("shape", shape), 0, NUM_SHAPES - 1);
        voices = std::clamp(j.value("voices", voices), 1, MAX_VOICES);
        low = std::clamp(j.value("low", low), 12, 96);
        high = std::clamp(j.value("high", high), 24, 120);
        spread = std::clamp(j.value("spread", spread), 0, NUM_SPREADS - 1);
        bass = j.value("bass", bass);
        voiceLeading = j.value("voiceLeading", voiceLeading);
    }
    // Each style's usual extensions / color, applied when the style is picked
    static Settings styleDefaults(int style){
        Settings s;
        s.style = style;
        switch(style){
            case STYLE_JAZZ:       s.extensions = 0.7f; s.color = 0.4f; s.tension = 0.5f; s.voices = 5; s.spread = SPREAD_DROP2; break;
            case STYLE_GLASS:      s.extensions = 0.15f; s.color = 0.3f; s.tension = 0.3f; s.mode = MODE_MINOR; break;
            case STYLE_REICH:      s.extensions = 0.5f; s.color = 0.3f; s.tension = 0.3f; s.mode = MODE_DORIAN; s.voices = 5; s.spread = SPREAD_OPEN; break;
            case STYLE_NEORIEMANN: s.extensions = 0.2f; s.color = 0.3f; s.tension = 0.5f; break;
            default: break;
        }
        return s;
    }
};

// ── Generation ──────────────────────────────────────────────────────────────
struct Candidate { std::vector<Chord> chords; float styleScore = 0.0f; };

class Generator {
public:
    Generator(const Settings& s, Rng& r) : st(s), rng(r) {
        key.tonic = s.key;
        key.mode = s.mode;
        rel = modeScale(s.mode);
        minor = minorMode(s.mode);
    }

    Candidate make(int n){
        switch(st.style){
            case STYLE_JAZZ: return jazz(n);
            case STYLE_GLASS: return glass(n);
            case STYLE_REICH: return reich(n);
            case STYLE_NEORIEMANN: return neoRiemann(n);
            default: return functional(n);
        }
    }

    // 0 triads .. 3 elevenths / thirteenths, drawn from Extensions
    int extLevel(){
        const float e = st.extensions;
        int l = 0;
        if(rng.chance(e)){ l = 1;
            if(rng.chance((e - 0.35f) / 0.65f)){ l = 2;
                if(rng.chance((e - 0.7f) / 0.3f)) l = 3; } }
        return l;
    }

    // A triad (or sus) made richer, keeping to the key when the chord is in it
    Chord extend(const Chord& c, int level){
        if(level <= 0) return c;
        static const std::map<std::string, std::array<std::vector<std::string>, 3>> opts{
            {"",     {{{"maj7", "6", "7"}, {"maj9", "6/9", "9", "add9"}, {"maj7#11", "13", "maj13", "maj9#11"}}}},
            {"m",    {{{"m7", "m6"}, {"m9", "madd9", "m6/9"}, {"m11", "m13"}}}},
            {"dim",  {{{"m7b5", "dim7"}, {"m9b5"}, {"m9b5"}}}},
            {"sus4", {{{"7sus4"}, {"9sus4"}, {"9sus4"}}}},
            {"sus2", {{{"maj7sus2", "sus24"}, {"maj7sus2"}, {"maj7sus2"}}}}};
        auto it = opts.find(c.quality);
        if(it == opts.end()) return c;
        bool inKey = true;
        for(int p : c.pcs()) if(!key.contains(p)) inKey = false;
        for(int l = std::min(level, 3); l >= 1; l--){
            std::vector<Chord> ok;
            for(const auto& q : it->second[l - 1]){
                Chord x = makeChord(c.root, q, c.bass);
                bool fits = true;
                if(inKey) for(int p : x.pcs()) if(!key.contains(p)) fits = false;
                if(fits) ok.push_back(x);
            }
            if(!ok.empty()) return ok[rng.below((int)ok.size())];
        }
        return c;
    }

    Key key;
    std::array<int, 7> rel;

private:
    const Settings& st;
    Rng& rng;
    bool minor = false;

    int sizeFor(int level){ return level <= 0 ? 3 : level == 1 ? 4 : level == 2 ? 5 : 7; }

    // ── Functional: Markov over degrees, then Color's chromatic chords
    Candidate functional(int n){
        static const float W[7][7] = {
            {0.0f, 2.0f, 1.0f, 3.0f, 3.0f, 3.0f, 0.5f},
            {0.5f, 0.0f, 0.0f, 0.7f, 5.0f, 0.0f, 1.5f},
            {0.3f, 1.0f, 0.0f, 2.0f, 0.5f, 4.0f, 0.0f},
            {2.0f, 1.5f, 0.3f, 0.0f, 4.0f, 0.5f, 1.0f},
            {5.0f, 0.0f, 0.3f, 0.7f, 0.0f, 2.5f, 0.0f},
            {0.5f, 3.0f, 0.5f, 3.0f, 1.5f, 0.0f, 0.3f},
            {5.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.3f, 0.0f}};
        auto rowSum = [&](int r){ float s = 0; for(float x : W[r]) s += x; return s; };
        std::vector<int> deg(n, 0);
        float logp = 0.0f;
        for(int i = 1; i < n; i++){
            const int d = rng.weighted(std::vector<float>(W[deg[i - 1]], W[deg[i - 1]] + 7));
            logp += std::log(std::max(1e-3f, W[deg[i - 1]][d] / rowSum(deg[i - 1])));
            deg[i] = d;
        }
        if(n > 1) logp += std::log(std::max(0.02f, W[deg[n - 1]][0] / rowSum(deg[n - 1])));
        const bool tonalMinor = st.mode == MODE_MINOR || st.mode == MODE_HARMONIC || st.mode == MODE_MELODIC;
        const auto harm = modeScale(MODE_HARMONIC);
        Candidate c;
        for(int i = 0; i < n; i++){
            int level = extLevel();
            const int d = deg[i];
            if(d == 4 && level == 0 && rng.chance(0.5f)) level = 1;     // V7 is common enough
            auto scale = rel;
            if(tonalMinor && (d == 4 || (d == 6 && rng.chance(0.5f)))) scale = harm;
            c.chords.push_back(diatonic(st.key, scale, d, sizeFor(level)));
        }
        const float col = st.color;
        for(int i = 1; i < n; i++){
            const int t = (i + 1) % n;
            // a secondary dominant onto the next chord
            if(t != 0 && deg[t] >= 1 && deg[t] <= 5 && !(deg[t] == 4 && rng.chance(0.5f)) && rng.chance(col * 0.45f)){
                const Chord& target = c.chords[t];
                const std::string q = target.isMinor() && rng.chance(0.5f) ? "7b9" : (st.extensions > 0.6f ? "9" : "7");
                c.chords[i] = makeChord(target.root + 7, q);
                continue;
            }
            // borrowed from the parallel minor
            if(!minor && rng.chance(col * 0.3f)){
                const int r = st.key;
                switch(deg[i]){
                    case 3: c.chords[i] = makeChord(r + 5, st.extensions > 0.4f ? "m6" : "m"); break;
                    case 5: c.chords[i] = makeChord(r + 8, st.extensions > 0.4f ? "maj7" : ""); break;
                    case 2: c.chords[i] = makeChord(r + 3, ""); break;
                    case 4: c.chords[i] = makeChord(r + 10, ""); break;
                    case 1: c.chords[i] = makeChord(r + 2, "m7b5"); break;
                    default: break;
                }
            }
            // the Neapolitan in minor, in first inversion
            else if(minor && deg[i] == 1 && rng.chance(col * 0.35f)) c.chords[i] = makeChord(st.key + 1, "", st.key + 5);
        }
        c.styleScore = 0.35f * logp / (float)std::max(1, n);
        return c;
    }

    // ── Jazz: tonic, then targets approached by ii-V / V / subV / backdoor
    Chord jazzColor(Chord c){
        const int l = std::max(1, extLevel());
        const float col = st.color;
        auto pick = [&](std::initializer_list<const char*> q){ std::vector<const char*> v(q); return std::string(v[rng.below((int)v.size())]); };
        std::string q = c.quality;
        if(q == "maj7")      q = l >= 3 ? pick({"maj9", "maj7#11", "maj13", "6/9"}) : l == 2 ? pick({"maj9", "6/9"}) : pick({"maj7", "6"});
        else if(q == "m7")   q = l >= 3 ? pick({"m11", "m9"}) : l == 2 ? "m9" : "m7";
        else if(q == "7")    q = rng.chance(col * 0.5f) ? pick({"7b9", "7#9", "7alt", "13b9", "7#11"})
                                : l >= 3 ? pick({"13", "9"}) : l == 2 ? "9" : "7";
        else if(q == "7b9")  q = rng.chance(col * 0.5f) ? pick({"7alt", "7b9b13"}) : "7b9";
        else if(q == "m7b5") q = l >= 2 && rng.chance(0.5f) ? "m9b5" : "m7b5";
        return makeChord(c.root, q, c.bass);
    }
    Candidate jazz(int n){
        const int T = st.key;
        auto tonic = [&](){ return minor ? makeChord(T, rng.chance(0.5f) ? "m6" : "m7") : makeChord(T, "maj7"); };
        // diatonic targets (sevenths of the mode), weights; Color adds far keys
        std::vector<std::pair<Chord, float>> targets;
        const float tw[7] = {1.0f, 3.0f, 1.5f, 2.5f, 0.6f, 3.0f, 0.4f};
        for(int d = 1; d < 7; d++){
            Chord ch = diatonic(T, rel, d, 4);
            if(ch.isDiminished() || ch.isDominant()) continue;
            targets.push_back({ch, tw[d]});
        }
        targets.push_back({tonic(), 1.0f});
        targets.push_back({makeChord(T + 3, "maj7"), st.color * 1.5f});
        targets.push_back({makeChord(T + 8, "maj7"), st.color * 1.5f});
        targets.push_back({makeChord(T + 5, "m7"), minor ? 0.0f : st.color});
        targets.push_back({makeChord(T + 1, "maj7"), st.color * 0.6f});
        std::vector<float> w;
        for(auto& t : targets) w.push_back(t.second);

        auto approach = [&](const Chord t, int len, std::vector<Chord>& out, bool toTonic){
            const bool tm = t.isMinor();
            const bool sub = rng.chance(st.color * 0.5f);
            if(len >= 2 && toTonic && !minor && rng.chance(st.color * 0.35f)){   // backdoor
                out.push_back(makeChord(t.root + 5, "m7"));
                out.push_back(makeChord(t.root + 10, "7"));
                return;
            }
            if(len >= 2) out.push_back(makeChord(t.root + 2, tm ? "m7b5" : "m7"));
            if(len >= 1) out.push_back(sub ? makeChord(t.root + 1, "7") : makeChord(t.root + 7, tm ? "7b9" : "7"));
        };
        Candidate c;
        c.chords.push_back(tonic());
        int remaining = n - 1;
        const int reserve = n >= 3 ? 2 : n == 2 ? 1 : 0;
        int lastTarget = -1;
        while(remaining - reserve > 0){
            const int room = remaining - reserve;
            int ti = rng.weighted(w);
            if(ti == lastTarget) ti = rng.weighted(w);
            lastTarget = ti;
            int a = room == 1 ? 0 : rng.weighted({0.6f, 1.2f, 2.0f});
            a = std::min(a, room - 1);
            approach(targets[ti].first, a, c.chords, false);
            c.chords.push_back(targets[ti].first);
            remaining -= a + 1;
        }
        approach(c.chords[0], reserve, c.chords, true);
        while((int)c.chords.size() < n) c.chords.push_back(tonic());
        c.chords.resize(n);
        for(auto& ch : c.chords) ch = jazzColor(ch);
        return c;
    }

    // ── Glass: a cell repeated, one change per pass (additive), pedal / inversions
    Candidate glass(int n){
        struct E { int off; const char* q; };
        static const std::vector<std::vector<E>> minorCells{
            {{0, "m"}, {8, ""}}, {{0, "m"}, {10, ""}, {8, ""}, {7, ""}}, {{0, "m"}, {5, "m"}, {8, ""}, {7, ""}},
            {{0, "m"}, {10, ""}, {3, ""}}, {{0, "m"}, {3, ""}, {5, "m"}, {7, ""}}, {{0, "m"}, {8, ""}, {5, "m"}, {7, "7"}},
            {{0, "m"}, {1, ""}}, {{0, "m"}, {8, ""}, {3, ""}, {10, ""}}, {{0, "m"}, {7, ""}, {8, ""}, {5, "m"}}};
        static const std::vector<std::vector<E>> majorCells{
            {{0, ""}, {9, "m"}}, {{0, ""}, {7, ""}, {9, "m"}, {5, ""}}, {{0, ""}, {5, ""}, {0, ""}, {7, ""}},
            {{0, ""}, {10, ""}, {5, ""}}, {{0, ""}, {4, "m"}, {9, "m"}, {5, ""}}, {{0, ""}, {2, "m"}, {5, ""}, {7, ""}},
            {{0, ""}, {8, ""}}, {{0, ""}, {9, "m"}, {4, "m"}, {5, ""}}};
        const auto& cells = minor ? minorCells : majorCells;
        std::vector<float> w;
        for(const auto& cell : cells){
            const int len = (int)cell.size();
            w.push_back(len > n ? 0.1f : 1.0f + (n % len == 0 ? 1.0f : 0.0f));
        }
        const auto& cell = cells[rng.weighted(w)];
        std::vector<Chord> pattern;
        const bool pedal = rng.chance(0.2f + 0.3f * st.color);
        for(const auto& e : cell){
            Chord ch = makeChord(st.key + e.off, e.q);
            ch = extend(ch, extLevel());
            if(pedal && pc(ch.root) != pc(st.key)) ch.bass = pc(st.key);
            else if(rng.chance(st.color * 0.35f) && ch.iv.size() >= 2) ch.bass = pc(ch.root + ch.iv[1]);
            pattern.push_back(ch);
        }
        Candidate c;
        const int len = (int)pattern.size();
        for(int i = 0; i < n; i++){
            if(i > 0 && i % len == 0 && rng.chance(0.6f + 0.3f * st.color)){
                // one change per pass, kept from then on
                const int p = rng.below(len);
                Chord& m = pattern[p];
                switch(rng.below(st.color > 0.4f ? 5 : 4)){
                    case 0: m = extend(makeChord(m.root, m.quality.substr(0, m.isMinor() ? 1 : 0), m.bass), 1); break;
                    case 1: if(!m.isMinor()) m = makeChord(m.root, "sus4", m.bass); else m = makeChord(m.root, "madd9", m.bass); break;
                    case 2: m = makeChord(m.root, m.isMinor() ? "madd9" : "add9", m.bass); break;
                    case 3: m.bass = m.bass >= 0 ? -1 : (m.iv.size() >= 2 ? pc(m.root + m.iv[1]) : -1); break;
                    default: m = m.isMinor() ? makeChord(m.root + 3, "") : makeChord(m.root + 9, "m"); break;
                }
            }
            c.chords.push_back(pattern[i % len]);
        }
        c.styleScore = n % len == 0 ? 0.3f : 0.0f;
        return c;
    }

    // ── Reich: a field of seconds and fourths from the mode over a moving bass
    Candidate reich(int n){
        const int f = rng.below(7);
        std::vector<int> field;
        if(rng.chance(0.55f)) field = {f, f + 1, f + 3, f + 4};   // two seconds a fourth apart
        else field = {f, f + 3, f + 6};                           // fourths
        const int extra = st.extensions > 0.75f ? 2 : st.extensions > 0.4f ? 1 : 0;
        for(int e = 0; e < extra; e++) field.push_back(field.back() + (e % 2 ? 3 : 2));
        Candidate c;
        int b = 0;
        static const int moves[] = {0, 1, -1, 2, -2, 3, -3, 4, -4};
        static const float mw[] = {0.8f, 2.0f, 2.0f, 1.0f, 1.0f, 1.5f, 1.5f, 0.8f, 0.8f};
        for(int i = 0; i < n; i++){
            if(i > 0) b += moves[rng.weighted(std::vector<float>(mw, mw + 9))];
            std::vector<int> deg = field;
            if(rng.chance(0.35f + 0.4f * st.color)){   // one note of the field steps aside
                const int k = rng.below((int)deg.size());
                deg[k] += rng.chance(0.5f) ? 1 : -1;
            }
            const int bassPc = pc(st.key + rel[((b % 7) + 7) % 7]);
            std::vector<int> pcs;
            for(int d : deg){
                int p = pc(st.key + rel[((d % 7) + 7) % 7]);
                if(pc(p - bassPc) == 1) p = pc(st.key + rel[(((d + 1) % 7) + 7) % 7]);   // no b9 over the bass
                pcs.push_back(p);
            }
            c.chords.push_back(fromPcs(pcs, bassPc));
        }
        return c;
    }

    // ── Neo-Riemannian walk on triads
    Candidate neoRiemann(int n){
        int root = st.key;
        bool isMinor = minor;
        int last = -1;
        Candidate c;
        auto add = [&](){ c.chords.push_back(extend(makeChord(root, isMinor ? "m" : ""), extLevel())); };
        add();
        for(int i = 1; i < n; i++){
            const float cw = st.color * 0.8f;
            std::vector<float> w{1.0f, 1.0f, 1.0f, cw, cw, cw * 0.6f};
            if(last >= 0 && last <= 2) w[last] = 0.0f;   // P, L, R undo themselves
            const int op = rng.weighted(w);
            last = op;
            switch(op){
                case 0: isMinor = !isMinor; break;                                                 // P
                case 1: root = isMinor ? root + 8 : root + 4; isMinor = !isMinor; break;           // L
                case 2: root = isMinor ? root + 3 : root + 9; isMinor = !isMinor; break;           // R
                case 3: root = isMinor ? root + 7 : root + 5; isMinor = !isMinor; break;           // N
                case 4: root = isMinor ? root + 11 : root + 1; isMinor = !isMinor; break;          // S
                default: root = isMinor ? root + 4 : root + 8; isMinor = !isMinor; break;          // H
            }
            root = pc(root);
            add();
        }
        return c;
    }
};

// ── Scoring ─────────────────────────────────────────────────────────────────
inline float shapeAt(int shape, float x){
    switch(shape){
        case SHAPE_RISE: return x;
        case SHAPE_FALL: return 1.0f - x;
        case SHAPE_ARC: return std::sin(x * 3.14159265f);
        case SHAPE_WAVE: return 0.5f - 0.5f * std::cos(x * 2.0f * 6.2831853f);
        default: return 0.5f;
    }
}

inline float scoreProgression(const std::vector<Chord>& p, const Settings& s, float styleScore){
    const int n = (int)p.size();
    if(n == 0) return 0.0f;
    Key k{s.key, s.mode};
    static const float smoothW[NUM_STYLES] = {1.0f, 0.6f, 1.5f, 1.5f, 2.0f};
    static const float dupW[NUM_STYLES] = {3.0f, 3.0f, 2.0f, 0.7f, 3.0f};
    float smooth = 0.0f, dups = 0.0f;
    for(int i = 0; i < n && n > 1; i++){
        const Chord& a = p[i];
        const Chord& b = p[(i + 1) % n];
        smooth += pcDistance(a, b);
        if(n == 2 && i == 1) continue;
        if(a.same(b)) dups += 1.0f;
        else if(a.root == b.root && a.bassPc() == b.bassPc() && s.style != STYLE_REICH) dups += 0.35f;   // Am9 then Am7
    }
    smooth /= (float)n;
    std::vector<Chord> distinct;
    for(const auto& c : p){
        bool seen = false;
        for(const auto& d : distinct) if(d.same(c)) seen = true;
        if(!seen) distinct.push_back(c);
    }
    const int wantDistinct = std::min(n, s.style == STYLE_REICH ? 2 : s.style == STYLE_GLASS ? 2 : 4);
    int unnamed = 0;   // chords only nameable as "(2,b5,7)" are usually muddy
    for(const auto& c : p) if(!c.quality.empty() && c.quality.front() == '(') unnamed++;
    float score = -smoothW[s.style] * smooth - dupW[s.style] * dups
                - 1.5f * (float)std::max(0, wantDistinct - (int)distinct.size()) - 0.4f * (float)unnamed;
    std::vector<float> t(n);
    float mean = 0.0f;
    for(int i = 0; i < n; i++){ t[i] = tensionOf(p[i], k); mean += t[i]; }
    mean /= (float)n;
    score -= 4.0f * (mean - s.tension) * (mean - s.tension);
    if(n >= 3 && s.shape != SHAPE_FREE){
        if(s.shape == SHAPE_FLAT){
            float var = 0.0f;
            for(float x : t) var += (x - mean) * (x - mean);
            score -= 8.0f * var / (float)n;
        }else{
            std::vector<float> g(n);
            float gm = 0.0f;
            for(int i = 0; i < n; i++){ g[i] = shapeAt(s.shape, (float)i / (float)(n - 1)); gm += g[i]; }
            gm /= (float)n;
            float sxy = 0.0f, sxx = 0.0f, syy = 0.0f;
            for(int i = 0; i < n; i++){ sxy += (t[i] - mean) * (g[i] - gm); sxx += (t[i] - mean) * (t[i] - mean); syy += (g[i] - gm) * (g[i] - gm); }
            const float corr = sxx > 1e-6f && syy > 1e-6f ? sxy / std::sqrt(sxx * syy) : 0.0f;
            score -= 1.5f * (1.0f - corr);
        }
    }
    return score + styleScore;
}

// ── Voicing ─────────────────────────────────────────────────────────────────
struct Voicing { int bass = 48; std::vector<int> upper; };

// The notes to keep when there are fewer voices than notes, most important first
inline int notePriority(const Chord& c, int v, bool bassHasRoot){
    if(v == 0) return bassHasRoot ? 1 : 4;
    const bool hasThird = c.has(3) || c.has(4);
    switch(v){
        case 3: case 4: return 10;
        case 10: case 11: return 9;
        case 9: return 8;
        case 6: case 8: return 8;
        case 2: return hasThird ? 6 : 9;
        case 5: return hasThird ? 4 : 9;
        case 7: return 2;
        case 13: case 14: case 15: return 7;
        case 17: return 5;
        case 18: case 20: case 21: return 6;
        default: return 3;
    }
}

inline std::vector<int> upperPcs(const Chord& c, int count, bool separateBass){
    std::vector<std::pair<int, int>> pr;   // priority, interval
    const bool bassHasRoot = separateBass && c.bass < 0;
    const int slash = !separateBass && c.bass >= 0 ? pc(c.bass - c.root) : -1;
    for(int v : c.iv) pr.push_back({pc(v) == slash ? 11 : notePriority(c, v, bassHasRoot), v});
    if(slash >= 0 && !c.has(slash)) pr.push_back({11, slash});
    std::stable_sort(pr.begin(), pr.end(), [](auto& a, auto& b){ return a.first > b.first; });
    std::vector<int> out;
    for(int i = 0; i < count && i < (int)pr.size(); i++) out.push_back(pc(c.root + pr[i].second));
    // more voices than notes: double root, fifth, then by priority
    std::vector<int> dbl;
    for(int v : {0, 7}) if(c.has(v)) dbl.push_back(pc(c.root + v));
    for(auto& p : pr) dbl.push_back(pc(c.root + p.second));
    for(size_t i = 0; (int)out.size() < count && !dbl.empty(); i++) out.push_back(dbl[i % dbl.size()]);
    return out;
}

inline std::vector<std::vector<int>> voicingCandidates(std::vector<int> pcs, int spread, int lo, int hi){
    std::vector<std::vector<int>> out;
    const int k = (int)pcs.size();
    if(k == 0) return out;
    std::sort(pcs.begin(), pcs.end());
    for(int r = 0; r < k; r++){
        for(int oct = (lo / 12) - 2; oct <= (hi / 12) + 1; oct++){
            std::vector<int> v;
            int prev = -1000;
            for(int j = 0; j < k; j++){
                const int p = pcs[(r + j) % k];
                int note = oct * 12 + p;
                while(note <= prev) note += 12;
                if(j > 0 && note == prev) note += 12;
                v.push_back(note);
                prev = note;
            }
            if(spread == SPREAD_DROP2 && k >= 3) v[k - 2] -= 12;
            else if(spread == SPREAD_OPEN && k >= 3){ for(int j = k - 2; j >= 0; j -= 2) v[j] -= 12; }
            else if(spread == SPREAD_WIDE && k >= 2){ for(int j = 0; j < k; j++) v[j] += 12 * ((j * 2) / k); }
            std::sort(v.begin(), v.end());
            out.push_back(v);
        }
    }
    return out;
}

inline Voicing voiceChord(const Chord& c, const Settings& s, const Voicing* prev){
    Voicing out;
    const int lo = std::min(s.low, s.high - 12), hi = std::max(s.high, s.low + 12);
    const int count = std::max(1, s.bass ? s.voices - 1 : s.voices);
    // bass
    {
        const int bp = c.bassPc();
        int best = lo, bestCost = std::numeric_limits<int>::max();
        for(int note = lo; note < lo + 19 && note <= hi; note++){
            if(pc(note) != bp) continue;
            int cost = note - lo;
            if(prev && s.voiceLeading) cost = std::abs(note - prev->bass) * 2 + (note >= lo + 12 ? 3 : 0);
            if(cost < bestCost){ bestCost = cost; best = note; }
        }
        out.bass = best;
    }
    const int upLo = s.bass ? std::max(lo + 7, out.bass + 3) : lo;
    const auto pcs = upperPcs(c, count, s.bass);
    const auto cands = voicingCandidates(pcs, s.spread, upLo, hi);
    const float center = 0.5f * (float)(upLo + hi) - 2.0f;
    float bestCost = std::numeric_limits<float>::max();
    for(const auto& v : cands){
        float cost = 0.0f;
        // range
        for(int x : v){ if(x < upLo) cost += 4.0f * (float)(upLo - x); if(x > hi) cost += 4.0f * (float)(x - hi); }
        // spacing: wide gaps inside, mud low down
        for(size_t j = 1; j < v.size(); j++){
            const int gap = v[j] - v[j - 1];
            if(gap > 12 && s.spread != SPREAD_WIDE) cost += 2.0f;
            if(v[j - 1] < 52 && gap < 5) cost += 3.0f;
            else if(v[j - 1] < 60 && gap < 3) cost += 1.5f;
        }
        if(s.bass && !v.empty() && v[0] - out.bass < 7 && v[0] < 60) cost += 1.5f;
        if(!s.bass && c.bass >= 0 && pc(v[0]) != c.bass) cost += 100.0f;
        const bool lead = prev && s.voiceLeading && prev->upper.size() == v.size();
        if(lead){
            for(size_t j = 0; j < v.size(); j++) cost += (float)std::abs(v[j] - prev->upper[j]);
            cost += 0.5f * (float)std::abs(v.back() - prev->upper.back());
        }else{
            float mean = 0.0f;
            for(int x : v) mean += (float)x;
            mean /= (float)v.size();
            cost += 0.3f * std::abs(mean - center);
            if(!s.bass && c.bass < 0 && pc(v[0]) != c.root) cost += s.voiceLeading ? 0.5f : 3.0f;
        }
        if(cost < bestCost){ bestCost = cost; out.upper = v; }
    }
    if(!s.bass && !out.upper.empty()) out.bass = out.upper.front();
    return out;
}

// ── Engine ──────────────────────────────────────────────────────────────────
struct Slot { Chord chord; bool locked = false; };

class Engine {
public:
    Settings s;
    std::vector<Slot> slots;
    std::vector<Voicing> voicings;
    std::vector<float> tensions;
    uint64_t revision = 1;

    Engine(){
        // I vi IV V until the first Generate
        for(auto [r, q] : std::vector<std::pair<int, const char*>>{{0, ""}, {9, "m"}, {5, ""}, {7, ""}})
            slots.push_back({makeChord(r, q), false});
        revoice();
    }

    Key key() const { return Key{s.key, s.mode}; }
    int size() const { return (int)slots.size(); }

    // New chords for the slots in `which` (empty: every unlocked one); the
    // others stay and the new ones are chosen to fit them
    void generate(uint64_t seed, std::vector<bool> which = {}){
        const int n = s.numChords;
        resizeSlots(n);
        if(which.empty()){ which.assign(n, false); for(int i = 0; i < n; i++) which[i] = !slots[i].locked; }
        which.resize(n, false);
        bool any = false;
        for(bool w : which) any = any || w;
        if(!any) return;
        const int single = std::count(which.begin(), which.end(), true) == 1 && n > 1
                         ? (int)(std::find(which.begin(), which.end(), true) - which.begin()) : -1;
        Rng rng(seed);
        Generator g(s, rng);
        // the good ones win, not always the same best one: each candidate is
        // drawn by exp(score / TEMPERATURE)
        std::vector<std::vector<Chord>> cands;
        std::vector<float> scores;
        float top = -std::numeric_limits<float>::max();
        for(int k = 0; k < CANDIDATES; k++){
            Candidate c = g.make(n);
            for(int i = 0; i < n; i++) if(!which[i]) c.chords[i] = slots[i].chord;
            float sc = scoreProgression(c.chords, s, c.styleScore);
            if(single >= 0 && c.chords[single].same(slots[single].chord)) sc -= 5.0f;   // a dice roll gives a new one
            top = std::max(top, sc);
            cands.push_back(std::move(c.chords));
            scores.push_back(sc);
        }
        std::vector<float> w(scores.size());
        for(size_t k = 0; k < scores.size(); k++) w[k] = std::exp((scores[k] - top) / TEMPERATURE);
        const auto& best = cands[rng.weighted(w)];
        for(int i = 0; i < n; i++) if(which[i]) slots[i].chord = best[i];
        changed();
    }

    void regenerateSlot(int i, uint64_t seed){
        if(i < 0 || i >= size()) return;
        std::vector<bool> w(size(), false);
        w[i] = true;
        generate(seed, w);
    }

    // NumChords: keeps the chords there are; new ones are generated to fit
    void setNumChords(int n, uint64_t seed){
        n = std::clamp(n, 1, MAX_CHORDS);
        const int old = size();
        s.numChords = n;
        resizeSlots(n);
        if(n > old){
            std::vector<bool> w(n, false);
            for(int i = old; i < n; i++) w[i] = true;
            generate(seed, w);
        }else changed();
    }

    bool setChordText(int i, const std::string& text){
        Chord c;
        if(i < 0 || i >= size() || !parseChord(text, c)) return false;
        slots[i].chord = c;
        slots[i].locked = true;
        changed();
        return true;
    }

    // Replace the bank with chords (from text or the AI)
    void setChords(const std::vector<Chord>& chords){
        if(chords.empty()) return;
        s.numChords = std::min((int)chords.size(), MAX_CHORDS);
        slots.clear();
        for(int i = 0; i < s.numChords; i++) slots.push_back({chords[i], false});
        changed();
    }

    // A new key moves the chords with it
    void setKey(int k){
        k = pc(k);
        const int d = k - s.key;
        s.key = k;
        if(d == 0) return;
        for(auto& sl : slots){
            sl.chord.root = pc(sl.chord.root + d);
            if(sl.chord.bass >= 0) sl.chord.bass = pc(sl.chord.bass + d);
        }
        changed();
    }

    void move(int from, int to){
        if(from < 0 || to < 0 || from >= size() || to >= size() || from == to) return;
        Slot sl = slots[from];
        slots.erase(slots.begin() + from);
        slots.insert(slots.begin() + to, sl);
        changed();
    }

    void erase(int i){
        if(i < 0 || i >= size() || size() <= 1) return;
        slots.erase(slots.begin() + i);
        s.numChords = size();
        changed();
    }
    void duplicate(int i){
        if(i < 0 || i >= size() || size() >= MAX_CHORDS) return;
        slots.insert(slots.begin() + i + 1, slots[i]);
        s.numChords = size();
        changed();
    }

    void revoice(){
        const int n = size();
        voicings.assign(n, Voicing{});
        tensions.assign(n, 0.0f);
        const Key k = key();
        for(int i = 0; i < n; i++) tensions[i] = tensionOf(slots[i].chord, k);
        if(n == 0) return;
        // twice around the loop, so the first chord follows the last
        Voicing prev;
        bool have = false;
        for(int pass = 0; pass < (s.voiceLeading && n > 1 ? 2 : 1); pass++)
            for(int i = 0; i < n; i++){
                voicings[i] = voiceChord(slots[i].chord, s, have ? &prev : nullptr);
                prev = voicings[i];
                have = true;
            }
    }

    void changed(){ revoice(); revision++; }

    // ── Outputs for slot i (Transpose added by the node)
    int wrap(int i) const { const int n = size(); return n == 0 ? 0 : ((i % n) + n) % n; }
    std::vector<float> pitch(int i) const {
        std::vector<float> v;
        if(voicings.empty()) return v;
        const auto& vo = voicings[wrap(i)];
        if(s.bass) v.push_back((float)vo.bass);
        for(int x : vo.upper) v.push_back((float)x);
        return v;
    }
    float bass(int i) const { return voicings.empty() ? 0.0f : (float)voicings[wrap(i)].bass; }
    float root(int i) const { return slots.empty() ? 0.0f : (float)slots[wrap(i)].chord.root; }
    std::vector<float> scale(int i) const { return slots.empty() ? std::vector<float>{} : chordScale(slots[wrap(i)].chord, key()); }
    float tension(int i) const { return tensions.empty() ? 0.0f : tensions[wrap(i)]; }
    std::string name(int i) const {
        if(slots.empty()) return "";
        const Chord& c = slots[wrap(i)].chord;
        return c.name(spellFlats(c, key()));
    }
    std::string romanAt(int i) const { return slots.empty() ? "" : roman(slots[wrap(i)].chord, key()); }

    std::string progressionText() const {
        std::string t;
        for(int i = 0; i < size(); i++) t += (i ? " " : "") + name(i);
        return t;
    }

    // ── State
    nlohmann::json toJson() const {
        nlohmann::json j;
        j["settings"] = s.toJson();
        j["slots"] = nlohmann::json::array();
        for(const auto& sl : slots)
            j["slots"].push_back({{"root", sl.chord.root}, {"bass", sl.chord.bass}, {"iv", sl.chord.iv},
                                  {"quality", sl.chord.quality}, {"locked", sl.locked}});
        return j;
    }
    void fromJson(const nlohmann::json& j){
        if(j.contains("settings") && j["settings"].is_object()) s.fromJson(j["settings"]);
        if(j.contains("slots") && j["slots"].is_array() && !j["slots"].empty()){
            slots.clear();
            for(const auto& o : j["slots"]){
                if((int)slots.size() >= MAX_CHORDS) break;
                Slot sl;
                sl.chord.root = pc(o.value("root", 0));
                sl.chord.bass = o.value("bass", -1);
                sl.chord.quality = o.value("quality", std::string());
                if(o.contains("iv") && o["iv"].is_array() && !o["iv"].empty()) sl.chord.iv = o["iv"].get<std::vector<int>>();
                sl.locked = o.value("locked", false);
                slots.push_back(sl);
            }
            s.numChords = (int)slots.size();
        }
        changed();
    }

private:
    void resizeSlots(int n){
        while((int)slots.size() < n) slots.push_back({slots.empty() ? makeChord(s.key, "") : slots.back().chord, false});
        if((int)slots.size() > n) slots.resize(n);
    }
};

} // namespace harmonybox
