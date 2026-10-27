//
//  scGlobalPitch.h
//  ofxOceanodeSuperCollider
//
//  Global Transpose, Global Scale and Global Detune: one transpose (semitones),
//  one scale (pitch classes 0-11, and a second one for the notes below a split
//  note) and one random spread per voice (cents) for
//  every pitched destination in the patch, set by the Global Pitch node
//  (globalPitch/scGlobalPitchNode.h). Each feature has its own list of
//  destinations.
//
//  Boxes read them from a block of 31 control buses per server (allocated by
//  serverManager): [transpose, scale on, the offset of each pitch class 0-11 to
//  its nearest scale degree, detune amount in cents, transpose wraps (modulo
//  12), the wrap's base pitch class, the split note (-1000 when off), the low
//  scale's 12 offsets, low scale on]. Scale on and low scale on are the two
//  bypasses: the scale at and above the split note, the low scale below it. There is one block for each combination
//  of the features a destination follows, so a destination sets one control
//  and the synth that makes its sounding pitch quantizes and transposes the
//  input note (gq), then adds its voices' detune (gt):
//
//    var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);   // Global Pitch
//    var gt = gg[14] * 0.01 * ({ Rand(-1, 1) } ! voices);               // one random per voice, drawn once
//    var gq = { |p| var r = p.round, on = ((r < gg[17]) * gg[30]) + ((r >= gg[17]) * gg[1]);   // the note's side of the split
//        var q = Select.kr(on > 0.5, [p, r + Select.kr(r.mod(12) + ((r < gg[17]) * 12), gg[2..13] ++ gg[18..29])]);
//        var w = gg[16] + (((q - gg[16]) / 12).floor * 12);              // the octave from the base holding the note
//        q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor) };    // transposed, wrapped into it when modulo 12
//    ... note = gq.(input pitch) ... + gt
//
//  so Oceanode values, Pitch cables and piano-roll notes all follow, and held
//  notes move with them.
//
//    setup():      scGlobalPitch::follow(this, true, [this]{ resend gtrbus to the live synths });
//    destructor:   scGlobalPitch::unfollow(this);
//    each synth:   s->set("gtrbus", scGlobalPitch::bus(this, server));
//
//  A generic SynthDef (scSynthdef) and FM7 cannot read the buses without being
//  compiled again, so they send applyInRange(value, channel, min, max) for their
//  "pitch" / "pitcharray" values (one per channel while spread() is on) and resend them
//  when a feature changes (valueChanged): Oceanode values and the timeline
//  follow, an audio cable into the pitch does not.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxSCServer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

class scGlobalPitch {
public:
    enum Feature { Transpose = 0, Scale = 1, Detune = 2, NumFeatures = 3 };
    static constexpr int BlockSize = 31;   // [transpose, scale on, 12 pitch-class offsets, detune cents, wrap on, wrap base,
                                           //  split note, 12 low-scale offsets, low scale on]
    static constexpr int NumBlocks = 8;    // by what a destination follows: bit 0 Transpose, bit 1 Scale, bit 2 Detune

    struct Follower {
        ofxOceanodeNodeModel* node = nullptr;
        bool byDefault = true;
        std::function<void()> resend;
        std::function<void()> valueChanged;   // a node that applies the values itself (scSynthdef), else none
    };

    // serverManager: the first of the server's NumBlocks * BlockSize buses (-1 when it has none)
    static void setServerBus(ofxSCServer* server, int index) {
        if(index < 0) { buses().erase(server); return; }
        buses()[server] = index;
        sendValues(server, index);
    }

    // ── the values ──
    static float transpose() { return state().transpose; }
    static void setTranspose(float v) {
        if(state().transpose == v) return;
        state().transpose = v;
        valuesChanged(Transpose);
    }
    static const std::vector<float>& scale() { return state().scale; }
    // the scale (at and above the split note) quantizes
    static bool scaleOn() { return state().scaleOn && !state().scale.empty(); }
    // degrees: pitch classes 0-11 (fractions allowed); on: false = bypassed
    static void setScale(const std::vector<float>& degrees, bool on) {
        if(state().scale == degrees && state().scaleOn == on) return;
        state().scale = degrees;
        state().scaleOn = on;
        state().offsets = offsetsFor(degrees);
        valuesChanged(Scale);
    }
    // the input notes below split (MIDI) quantize to the low scale instead; split 0 = off
    static const std::vector<float>& lowScale() { return state().lowScale; }
    static float split() { return state().split; }
    static bool splitOn() { return state().split > 0.0f && !state().lowScale.empty(); }
    // the low scale quantizes the notes below the split note (its own bypass)
    static bool lowScaleOn() { return state().lowOn && splitOn(); }
    static bool lowScaleEnabled() { return state().lowOn; }
    static void setLowScale(const std::vector<float>& degrees, float split, bool on) {
        if(state().lowScale == degrees && state().split == split && state().lowOn == on) return;
        state().lowScale = degrees;
        state().split = split;
        state().lowOn = on;
        state().lowOffsets = offsetsFor(degrees);
        valuesChanged(Scale);
    }
    // modulo 12: a transposed note wraps into its own octave, which starts at base (pitch class 0-11)
    static bool transposeWraps() { return state().wrap; }
    static int transposeBase() { return state().wrapBase; }
    static void setTransposeWrap(bool on, int base) {
        base = ((base % 12) + 12) % 12;
        if(state().wrap == on && state().wrapBase == base) return;
        state().wrap = on;
        state().wrapBase = base;
        valuesChanged(Transpose);
    }
    static float detune() { return state().detune; }
    // cents: each voice's offset is a fixed random within +-cents
    static void setDetune(float cents) {
        if(state().detune == cents) return;
        state().detune = cents;
        valuesChanged(Detune);
    }

    // ── the destinations ──
    static void follow(ofxOceanodeNodeModel* node, bool byDefault, std::function<void()> resend,
                       std::function<void()> valueChanged = {}) {
        unfollow(node);
        followers().push_back({node, byDefault, std::move(resend), std::move(valueChanged)});
    }
    static void unfollow(ofxOceanodeNodeModel* node) {
        auto& f = followers();
        f.erase(std::remove_if(f.begin(), f.end(), [node](const Follower& x){ return x.node == node; }), f.end());
        randoms().erase(node);
    }
    static const std::vector<Follower>& destinations() { return followers(); }

    // the block a destination's synths read, -1 when it follows neither feature
    static int bus(ofxOceanodeNodeModel* node, ofxSCServer* server) {
        const int block = (follows(node, Transpose) ? 1 : 0) | (follows(node, Scale) ? 2 : 0) | (follows(node, Detune) ? 4 : 0);
        auto it = buses().find(server);
        if(block == 0 || it == buses().end()) return -1;
        return it->second + block * BlockSize;
    }

    // what a destination that applies the values itself sends for a pitch v on a channel
    // detune: false for a destination that cannot play a fraction of a semitone (a MIDI note)
    static float apply(ofxOceanodeNodeModel* node, float v, int channel = 0, bool withDetune = true) {
        if(follows(node, Scale)) {
            const float r = std::round(v);
            const int pc = ((int(r) % 12) + 12) % 12;
            const bool low = splitOn() && r < split();
            if(low ? lowScaleOn() : scaleOn()) v = r + (low ? state().lowOffsets : state().offsets)[pc];
        }
        if(follows(node, Transpose)) v = transposed(v);
        if(withDetune && spread(node)) v += detune() * 0.01f * random(node, channel);
        return v;
    }
    // a note transposed: with modulo 12, wrapped into the octave from the base that holds it
    static float transposed(float v) {
        if(!state().wrap) return v + transpose();
        const float base = float(state().wrapBase);
        const float window = base + std::floor((v - base) / 12.0f) * 12.0f;
        return v + transpose() - 12.0f * std::floor((v + transpose() - window) / 12.0f);
    }
    // apply(), kept inside the control's declared range: a transpose must not push
    // a note (or a filter's cutoff, often a "pitch" too) past what the SynthDef is
    // built for, e.g. a band-pass bank's top bands up to Nyquist
    static float applyInRange(ofxOceanodeNodeModel* node, float v, int channel, float lo, float hi, bool withDetune = true) {
        const float out = apply(node, v, channel, withDetune);
        return lo <= hi ? std::clamp(out, lo, hi) : out;
    }
    // true while a destination's channels each need their own value (Global Detune)
    static bool spread(ofxOceanodeNodeModel* node) { return detune() != 0.0f && follows(node, Detune); }
    // a destination's fixed random in -1..1 for a voice / channel (drawn once, while it follows)
    static float random(ofxOceanodeNodeModel* node, int index) {
        auto& r = randoms()[node];
        if(index < 0) index = 0;
        if(index >= (int)r.size()) {
            static std::mt19937 engine{std::random_device{}()};
            std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
            while((int)r.size() <= index) r.push_back(uniform(engine));
        }
        return r[index];
    }

    // the controls of a generic / old-format SynthDef that carry its note (MIDI):
    // pitch, pitcharray, and carp (PMod's carrier; its modulator modp is timbre)
    static bool isPitchControl(const std::string& name) {
        return name == "pitch" || name == "pitcharray" || name == "carp";
    }

    // a destination's patch-unique name ("AnalogBox 2", "Macro 1/PolyComb 1")
    static std::string key(ofxOceanodeNodeModel* node) {
        const std::string parents = node->getParents();
        return (parents.empty() || parents == "Canvas" ? "" : parents + "/") + node->nodeName() + " " + ofToString(node->getNumIdentifier());
    }
    static bool follows(ofxOceanodeNodeModel* node, Feature feature) {
        for(auto& f : followers()) if(f.node == node) {
            auto& o = state().overrides[feature];
            auto it = o.find(key(node));
            return it == o.end() ? f.byDefault : it->second;
        }
        return false;
    }
    static void setFollows(ofxOceanodeNodeModel* node, Feature feature, bool on) {
        for(auto& f : followers()) if(f.node == node) {
            const auto k = key(node);
            auto& o = state().overrides[feature];
            if(on == f.byDefault) o.erase(k); else o[k] = on;
            if(f.resend) f.resend();
        }
    }

    // the destinations that differ from their default, as the feature's node saves them:
    // one "key=0" or "key=1" per line
    static std::string overridesText(Feature feature) {
        std::string text;
        for(auto& [k, on] : state().overrides[feature]) text += k + (on ? "=1\n" : "=0\n");
        return text;
    }
    static void setOverridesText(Feature feature, const std::string& text) {
        std::map<std::string, bool> parsed;
        for(auto& line : ofSplitString(text, "\n", true, true)) {
            const auto eq = line.rfind('=');
            if(eq == std::string::npos || eq == 0) continue;
            parsed[line.substr(0, eq)] = line.substr(eq + 1) != "0";
        }
        if(parsed == state().overrides[feature]) return;
        state().overrides[feature] = std::move(parsed);
        for(auto& f : followers()) if(f.resend) f.resend();
    }

    // each pitch class's offset to the nearest degree (the lower one on a tie)
    static std::array<float, 12> offsetsFor(const std::vector<float>& degrees) {
        std::array<float, 12> out{};
        if(degrees.empty()) return out;
        for(int pc = 0; pc < 12; pc++) {
            float best = 0, bestDistance = 1e9f;
            for(float d : degrees) {
                const float degree = std::fmod(std::fmod(d, 12.0f) + 12.0f, 12.0f);
                for(float candidate : {degree - 12.0f, degree, degree + 12.0f}) {
                    const float offset = candidate - pc, distance = std::abs(offset);
                    if(distance < bestDistance - 1e-6f || (std::abs(distance - bestDistance) <= 1e-6f && offset < best)) {
                        best = offset; bestDistance = distance;
                    }
                }
            }
            out[pc] = best;
        }
        return out;
    }

private:
    struct State {
        float transpose = 0.0f;
        std::vector<float> scale;
        bool scaleOn = false;
        std::array<float, 12> offsets{};
        std::vector<float> lowScale;
        float split = 0.0f;
        std::array<float, 12> lowOffsets{};
        bool lowOn = false;
        float detune = 0.0f;
        bool wrap = false;
        int wrapBase = 0;
        std::array<std::map<std::string, bool>, NumFeatures> overrides;
    };
    static State& state() { static State s; return s; }
    static std::map<ofxSCServer*, int>& buses() { static std::map<ofxSCServer*, int> b; return b; }
    static std::vector<Follower>& followers() { static std::vector<Follower> f; return f; }
    static std::map<ofxOceanodeNodeModel*, std::vector<float>>& randoms() { static std::map<ofxOceanodeNodeModel*, std::vector<float>> r; return r; }

    static void valuesChanged(Feature feature) {
        for(auto& [server, index] : buses()) sendValues(server, index);
        for(auto& f : followers()) if(f.valueChanged && follows(f.node, feature)) f.valueChanged();
    }
    static void sendValues(ofxSCServer* server, int index) {
        if(!server) return;
        ofxOscMessage m;
        m.setAddress("/c_setn");
        m.addIntArg(index);
        m.addIntArg(NumBlocks * BlockSize);
        for(int block = 0; block < NumBlocks; block++) {
            const bool t = block & 1, s = block & 2, q = s && scaleOn(), lq = s && lowScaleOn(), d = block & 4;
            m.addFloatArg(t ? state().transpose : 0.0f);
            m.addFloatArg(q ? 1.0f : 0.0f);
            for(float o : state().offsets) m.addFloatArg(q ? o : 0.0f);
            m.addFloatArg(d ? state().detune : 0.0f);
            m.addFloatArg(t && state().wrap ? 1.0f : 0.0f);
            m.addFloatArg(t ? float(state().wrapBase) : 0.0f);
            m.addFloatArg(s && splitOn() ? state().split : -1000.0f);   // where the low side starts, bypassed or not
            for(float o : state().lowOffsets) m.addFloatArg(lq ? o : 0.0f);
            m.addFloatArg(lq ? 1.0f : 0.0f);
        }
        server->sendMsg(m);
    }
};
