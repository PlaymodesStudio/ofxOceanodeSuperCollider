//
//  scPolyCombPitchMap.h
//  ofxOceanodeSuperCollider
//
//  PolyComb's internal pitch maps (the Pitch & Harmony tab), after the original
//  Reaktor PolyComb 1.7: one map for the oscillators, one for the resonators.
//
//  Each voice gets an integer index (geometries are rounded after shaping).
//  The Classic LFO morph waveform defines the layout (Skew/Roundness/PW).
//  Num Waves repeats it; Symmetry folds it. Final Pow normalizes the completed
//  range, reshapes amplitudes, and restores that range before integer rounding.
//  Legacy baseline: each voice gets an index: linear (First + i) or circular (up to the middle
//  voice and back down: N/2 - |v - N/2| + 1), bent by (index / N)^Bend * N.
//  Then a pitch:
//      60 + Transpose + Interval * index     (Off / Table / Tones; with a Span,
//                                             Interval * index wraps every Span st)
//      Base note * the index-th harmonic     (Harmonic: Interval * index is the
//                                             harmonic number; negative ones
//                                             are subharmonics, 0 the base)
//  quantized by the Mode:
//      User      degree modulo N chooses a note; each N degrees adds an octave
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
#include <cstdint>
#include "../../../audio/scMorphWave.h"

namespace pcmap {

enum Mode { ModeOff, ModeTable, ModeTones, ModeHarmonic, NumModes };
enum Quantize { QuantizeNearest, QuantizeDown, QuantizeUp };

struct Map {
    bool morphGeometry = false;   // legacy math retained only for old-state migration
    float skew=-1.f, roundness=0.f, pulseWidth=.5f, pow=0.f;
    int cycleShift = 0;            // whole-index offset per waveform cycle, wrapped to active voices
    float waves = 1.0f;            // repeats across the voice geometry
    int shape = 0;                 // 0 linear, 1 circular
    float first = 1.0f;            // the first voice's index (the original counts from 1)
    float interval = 0.0f;         // semitones (Harmonic: the harmonic step) per index
    float transpose = 0.0f;        // around 60
    float bend = 1.0f;             // legacy exponent, used only before morph migration
    float offset = 0.0f;           // added after the quantization
    int mode = ModeOff;
    int quantize = QuantizeNearest;
    float root = 0.0f;             // the tones' and table's key (0 C ... 11 B)
    std::vector<float> tones{0, 2, 4, 5, 7, 9, 11};
    std::vector<float> table{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    float harmonicBase = 36.0f;    // Harmonic: the note of harmonic 1
    float degreeOffset = 0.0f;
    float octaveJump = 1.0f;       // octave distance between repetitions of the small scale
    bool invert = false;
    int symmetry = 0, seed = 0;
    float comb = 0.0f, indexQuantize = 0.0f, indexRandom = 0.0f;
    float span = 0.0f;             // > 0: Interval * index wraps (a repeating pitch set); 0 off
};

inline float snapInteger(float x) {
    const float integer=std::round(x);
    return std::abs(x-integer)<1.e-4f?integer:x;
}
inline float floorMod(float x, float m) { float r = std::fmod(x, m); return r < 0.0f ? r + m : r; }

// voice i (0-based) of n
inline float rawVoiceIndex(const Map& m, int i, int n) {
    const float nf = (float)std::max(1, n);
    float v = m.first + (float)i;
    if(m.morphGeometry){
        // Voices sample a periodic waveform: N samples span [0, 1), never
        // including the next cycle's endpoint. Scale by N so a unit ramp
        // advances one integer index per voice, regardless of voice count.
        const float position=(v-1.f)*m.waves;
        const float phase=floorMod(position,nf)/nf;
        v=1.f+nf*scmorph::morph(phase,m.roundness,m.skew,m.pulseWidth,false);
        const float shift=floorMod(std::floor(position/nf)*m.cycleShift,nf);
        if(shift!=0.f){
            const float index=std::clamp(v-1.f,0.f,nf-1.f);
            v=1.f+floorMod(index+shift,nf);
        }
    }else{
    if(std::abs(m.waves-1.0f)>1e-6f) v=1.0f+floorMod((v-1.0f)*m.waves,nf);
    if(m.shape == 2) v=1.0f+(nf-1.0f)*(0.5f-0.5f*std::cos(6.28318530718f*(v-1.0f)/nf));
    if(m.shape == 1) v = nf * 0.5f - std::abs((v-1.0f) - nf * 0.5f) + 1.0f;
    if(std::abs(m.bend - 1.0f) > 1e-6f){
        const float width=std::max(1.0f,nf-1.0f),x=(v-1.0f)/width;
        v=1.0f+(x<0.0f?-std::pow(-x,m.bend):std::pow(x,m.bend))*width;
    }
    }
    if(m.invert) v = nf + 1.0f - v;
    if(m.symmetry > 0){
        const float width = std::max(1.0f, nf / (m.symmetry+1));
        v = width - std::abs(floorMod(v - 1.0f, width * 2.0f) - width) + 1.0f;
    }
    v=1.f+snapInteger(v-1.f); // avoid phase roundoff changing an integer lane
    if(m.comb > 0.0f){
        if(((int)std::floor(v-1.0f)%2)!=0) v += (nf+1.0f-2.0f*v)*m.comb;
    }
    if(m.indexQuantize > 1e-6f) v = 1.0f + std::floor(snapInteger((v-1.0f)/m.indexQuantize))*m.indexQuantize;
    if(m.indexRandom > 0.0f){
        // Stable per-voice scatter, independent of frame rate and voice count.
        uint32_t h = (uint32_t)i * 747796405u + (uint32_t)m.seed * 2891336453u + 277803737u;
        h = ((h >> ((h >> 28u)+4u)) ^ h) * 277803737u; h = (h >> 22u) ^ h;
        v += ((double)h/4294967295.0*2.0-1.0)*m.indexRandom;
    }
    return m.morphGeometry?std::clamp(v,1.f,nf):v;
}

// Pow is an amplitude transform after geometry, folds, comb, groups and scatter.
// Normalize the actual range so it preserves both extrema and mirror symmetry.
inline float shapedVoiceIndex(const Map& m,int i,int n) {
    const float v=rawVoiceIndex(m,i,n);
    if(!m.morphGeometry || std::abs(m.pow)<1.e-6f)return v;
    float lo=v,hi=v;
    for(int j=0;j<n;++j){const float x=rawVoiceIndex(m,j,n);lo=std::min(lo,x);hi=std::max(hi,x);}
    return hi-lo<1.e-6f?v:lo+(hi-lo)*scmorph::normalizedPow((v-lo)/(hi-lo),m.pow);
}

// Every downstream mapper sees integer positions, including bent/sine/random
// geometries. Keep the continuous helper for versioned legacy-state migration.
inline float voiceIndex(const Map& m,int i,int n) {
    float v=shapedVoiceIndex(m,i,n);
    const float half=std::round(v*2)*0.5f;
    if(std::abs(v-half)<1e-5f)v=half; // stable ties at exact sine quarter-cycles
    return std::round(v);
}

// User notes are an ordered scale: one degree per distinct integer pitch class.
inline std::vector<float> userScale(const std::vector<float>& input) {
    std::vector<float> out;
    for(float v : input){
        const float note = std::clamp(std::round(v),0.0f,11.0f);
        if(std::find(out.begin(),out.end(),note)==out.end()) out.push_back(note);
    }
    if(out.empty()) out.push_back(0);
    return out;
}
// Reaktor macro's sort V: quotient selects the octave, remainder the scale note.
inline float userDegree(float degree, const std::vector<float>& notes, float octaveJump = 1.0f) {
    const float size = (float)notes.size();
    const float index = std::floor(degree + 1e-5f);
    return 12.0f * octaveJump * std::floor(index/size) + notes[(size_t)floorMod(index,size)];
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
            if(ad < distance - 1e-4f || (std::abs(ad - distance) <= 1e-4f && c + root < best)){
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
    const auto scale = userScale(m.table);
    for(int i = 0; i < n; i++){
        const float s = shift.empty() ? 0.0f : shift[std::min((size_t)i, shift.size() - 1)];
        const float index = voiceIndex(m, i, n);
        float p;
        if(m.mode == ModeTable){
            float degree = m.interval * (index-1.0f) + m.degreeOffset;
            if(m.span > 1e-6f) degree = floorMod(degree,m.span);
            p = 60.0f + m.transpose + m.root + userDegree(degree,scale,m.octaveJump);
            if(std::abs(s)>1e-6f) p=quantizeTones(p+s,scale,m.root+m.transpose,m.quantize);
        }else if(m.mode == ModeHarmonic){
            const float h = m.interval * index;
            const float base = m.harmonicBase + m.transpose + s;
            p = h > 1e-6f ? base + 12.0f * std::log2(h) : h < -1e-6f ? base - 12.0f * std::log2(-h) : base;
            p = std::clamp(p, -127.0f, 127.0f);
        }else{
            float rel = m.interval * index;
            if(m.span > 1e-6f) rel = floorMod(rel, m.span);
            p = 60.0f + m.transpose + rel + s;
            if(m.mode == ModeTones) p = quantizeTones(p, m.tones, m.root, m.quantize);
        }
        out[(size_t)i] = p + m.offset;
    }
    return out;
}

// Complete a map with the live played root and optional voice permutation.
// Playback and endpoint morphing share this final stage so their pitches agree
// at both ends of a transition, including quantization and range clipping.
inline std::vector<float> playedPitches(const Map& map, int n, float root, bool inScale,
                                      const std::vector<int>& order = {}) {
    const std::vector<float> shift(std::max(0,n),root-60.0f);
    auto notes=generate(map,n,inScale ? shift : std::vector<float>{});
    if(!inScale) for(auto& note:notes) note+=root-60.0f;
    if(n>1 && int(order.size())>=n) {
        const auto original=notes;
        for(int i=0;i<n;++i) notes[i]=original[std::clamp(order[i],0,n-1)];
    }
    for(auto& note:notes) note=std::clamp(note,0.0f,135.0f);
    return notes;
}

} // namespace pcmap
