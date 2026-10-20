//
//  scPolyCombPitchMap.h
//  ofxOceanodeSuperCollider
//
//  PolyComb's internal pitch maps (the Pitch & Harmony tab), after the original
//  Reaktor PolyComb 1.7: one map for the oscillators, one for the resonators.
//
//  Each voice gets an index: linear (First + i) or circular (up to the middle
//  voice and back down: N/2 - |v - N/2| + 1), bent by (index / N)^Bend * N.
//  Then a pitch:
//      60 + Transpose + Interval * index     (Off / Table / Tones; with a Span,
//                                             Interval * index wraps every Span st)
//      Base note * the index-th harmonic     (Harmonic: Interval * index is the
//                                             harmonic number; negative ones
//                                             are subharmonics, 0 the base)
//  quantized by the Mode:
//      Table     each pitch class to the table's value (12, from Root)
//      Tones     to the nearest / lower / upper of the tones (pitch classes:
//                a scale or a chord), from Root
//  and Offset added. The node adds the played Pitch around 60 (61: +1), after
//  the map or, "in the scale", before the quantization.
//
//  Pure arithmetic: no Oceanode, for the node and its offline checks.
//
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace pcmap {

enum Mode { ModeOff, ModeTable, ModeTones, ModeHarmonic, NumModes };
enum Quantize { QuantizeNearest, QuantizeDown, QuantizeUp };

struct Map {
    int shape = 0;                 // 0 linear, 1 circular
    float first = 1.0f;            // the first voice's index (the original counts from 1)
    float interval = 0.0f;         // semitones (Harmonic: the harmonic step) per index
    float transpose = 0.0f;        // around 60
    float bend = 1.0f;             // 1 straight, more: indices pulled together at the start
    float offset = 0.0f;           // added after the quantization
    int mode = ModeOff;
    int quantize = QuantizeNearest;
    float root = 0.0f;             // the tones' and table's key (0 C ... 11 B)
    std::vector<float> tones{0, 2, 4, 5, 7, 9, 11};
    std::vector<float> table{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    float harmonicBase = 36.0f;    // Harmonic: the note of harmonic 1
    float span = 0.0f;             // > 0: Interval * index wraps (a repeating pitch set); 0 off
};

inline float floorMod(float x, float m) { float r = std::fmod(x, m); return r < 0.0f ? r + m : r; }

// voice i (0-based) of n
inline float voiceIndex(const Map& m, int i, int n) {
    const float nf = (float)std::max(1, n);
    float v = m.first + (float)i;
    if(m.shape == 1) v = nf * 0.5f - std::abs(v - nf * 0.5f) + 1.0f;
    if(std::abs(m.bend - 1.0f) > 1e-6f){
        const float x = v / nf;
        v = (x < 0.0f ? -std::pow(-x, m.bend) : std::pow(x, m.bend)) * nf;
    }
    return v;
}

inline float applyTable(float p, const std::vector<float>& table, float root) {
    if(table.empty()) return p;
    const float rel = p - root;
    const float octave = std::floor(rel / 12.0f);
    const int pc = std::clamp((int)std::floor(rel - octave * 12.0f), 0, 11);
    return octave * 12.0f + table[std::min((size_t)pc, table.size() - 1)] + root;
}

// the nearest (ties: the lower), lower or upper pitch of the tones (any octave)
inline float quantizeTones(float p, const std::vector<float>& tones, float root, int dir) {
    if(tones.empty()) return p;
    const float rel = p - root;
    const float octave = std::floor(rel / 12.0f);
    float best = p, distance = 1e9f;
    for(int o = -1; o <= 1; o++){
        for(float t : tones){
            const float c = (octave + (float)o) * 12.0f + t;
            const float d = c - rel;
            if(dir == QuantizeDown && d > 1e-4f) continue;
            if(dir == QuantizeUp && d < -1e-4f) continue;
            const float ad = std::abs(d);
            if(ad < distance - 1e-6f || (std::abs(ad - distance) <= 1e-6f && c + root < best)){
                distance = ad; best = c + root;
            }
        }
    }
    return distance < 1e8f ? best : p;
}

// the voices' pitches before the played Pitch is added after the map;
// shift: the played Pitch - 60 per voice when it goes "in the scale" (else 0)
inline std::vector<float> generate(const Map& m, int n, const std::vector<float>& shift = {}) {
    std::vector<float> out((size_t)std::max(0, n));
    for(int i = 0; i < n; i++){
        const float s = shift.empty() ? 0.0f : shift[std::min((size_t)i, shift.size() - 1)];
        const float index = voiceIndex(m, i, n);
        float p;
        if(m.mode == ModeHarmonic){
            const float h = m.interval * index;
            const float base = m.harmonicBase + s;
            p = h > 1e-6f ? base + 12.0f * std::log2(h) : h < -1e-6f ? base - 12.0f * std::log2(-h) : base;
            p = std::clamp(p, -127.0f, 127.0f);
        }else{
            float rel = m.interval * index;
            if(m.span > 1e-6f) rel = floorMod(rel, m.span);
            p = 60.0f + m.transpose + rel + s;
            if(m.mode == ModeTable) p = applyTable(p, m.table, m.root);
            else if(m.mode == ModeTones) p = quantizeTones(p, m.tones, m.root, m.quantize);
        }
        out[(size_t)i] = p + m.offset;
    }
    return out;
}

} // namespace pcmap
