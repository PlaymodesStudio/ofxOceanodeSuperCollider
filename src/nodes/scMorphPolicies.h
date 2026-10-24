#pragma once

#include "ofMain.h"
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Audio values are separate from editor parameters: a checkbox still stores a
// bool, but its effective activation can be fractional. No name/type guessing.
// Snapshot metadata preserves effective values when storing or interrupting a
// morph. Old snapshots need no migration. Engines opt into supported policies.
class scMorphPolicies {
public:
    using Read = std::function<ofJson(const ofJson&)>;
    static constexpr const char* metadata = "_morphAudio";

    template<class T>
    void bypass(ofParameter<T>& p, Read read = {}) {
        add(p, Kind::Activation, 0, std::move(read));
    }
    // Modes whose DSP already runs parallel alternatives. weights() blends
    // endpoints directly (never visits unrelated intermediate enum values).
    template<class T> void mode(ofParameter<T>& p, int count, Read read = {}) {
        add(p, Kind::Mode, std::max(1, count), std::move(read));
    }
    template<class T, class E>
    void enabledDepth(ofParameter<E>& enabled, ofParameter<T>& depth, Read readEnabled = {}, Read readDepth = {}) {
        auto& e = add(depth, Kind::Depth, 0, std::move(readDepth));
        e.enabled = readEnabled ? std::move(readEnabled) : field(enabled.getEscapedName());
        e.enabledDefault = ofJson(enabled.get());
        e.selector = &enabled;
        e.selectorKey = enabled.getEscapedName();
        e.enabledCurrent = [&enabled]() { return ofJson(enabled.get()); };
    }
    static Read field(const std::string& key, const std::string& container = "") {
        return [key, container](const ofJson& j) -> ofJson {
            const ofJson* s = &j;
            if(!container.empty()) {
                if(!j.contains(container) || !j[container].is_object()) return {};
                s = &j[container];
            }
            return s->contains(key) ? (*s)[key] : ofJson();
        };
    }
    template<class T> T value(const ofParameter<T>& p) const {
        auto it = entries.find(&p);
        if(!isBlocked(p.getEscapedName()) && it != entries.end() && !it->second.effective.is_null()) {
            try { return it->second.effective.template get<T>(); } catch(...) {}
        }
        return p.get();
    }
    template<class T> float activation(const ofParameter<T>& p) const {
        auto it = entries.find(&p);
        return isBlocked(p.getEscapedName()) || it == entries.end() || !it->second.effective.is_number()
            ? std::clamp(number(ofJson(p.get())), 0.0f, 1.0f) : it->second.effective.template get<float>();
    }
    template<class T> std::vector<float> weights(const ofParameter<T>& p) const {
        auto it = entries.find(&p);
        if(it == entries.end()) return {};
        const auto& e = it->second;
        return !isBlocked(p.getEscapedName()) && e.effective.is_array() ? e.effective.template get<std::vector<float>>() : oneHot(p.get(), e.count);
    }
    bool applying() const { return inApply; }
    // Called before ordinary parameter listeners send audio values. Manual
    // edits invalidate overrides; snapshot application leaves them intact.
    void edited(const ofAbstractParameter& p) {
        if(inApply) return;
        auto it = entries.find(&p);
        if(it != entries.end()) it->second.effective = {};
        for(auto& item : entries) if(item.second.selector == &p) item.second.effective = {};
    }
    void erase(const ofAbstractParameter& p) { entries.erase(&p); }
    void clear() {
        for(auto& item : entries) item.second.effective = {};
    }
    void begin(const ofJson& a, const ofJson& b, float t, const std::function<bool(const std::string&)>& blocked = {}) {
        inApply = true;
        blockedControls = blocked;
        t = std::clamp(t, 0.0f, 1.0f);
        for(auto& item : entries) {
            auto& e = item.second;
            if(blocked && blocked(e.id)) { e.effective = {}; continue; }
            ofJson x = endpoint(e, a), y = endpoint(e, b);
            if(e.kind == Kind::Depth) {
                const auto ax = e.enabled(a), by = e.enabled(b);
                const bool driven = blocked && blocked(e.selectorKey);
                const ofJson ex = driven ? e.enabledCurrent() : ax.is_null() ? e.enabledDefault : ax;
                const ofJson ey = driven ? e.enabledCurrent() : by.is_null() ? e.enabledDefault : by;
                if(!hasEffective(a, e.id)) x = scale(x, number(ex) > 0 ? 1 : 0);
                if(!hasEffective(b, e.id)) y = scale(y, number(ey) > 0 ? 1 : 0);
            }
            e.effective = lerp(x, y, t);
        }
    }
    void end() { inApply = false; }
    void capture(ofJson& j) const {
        ofJson state = ofJson::object();
        for(const auto& item : entries) {
            const auto& e = item.second;
            if(isBlocked(e.id)) continue;
            auto expected = endpoint(e, j);
            if(e.kind == Kind::Depth) {
                auto enabled = e.enabled(j);
                if(enabled.is_null()) enabled = e.enabledDefault;
                expected = scale(expected, number(enabled) > 0 ? 1 : 0);
            }
            if(!e.effective.is_null() && e.effective != expected) state[e.id] = e.effective;
        }
        if(!state.empty()) j[metadata] = std::move(state);
    }
    static ofJson lerp(const ofJson& a, const ofJson& b, float t) {
        if(t <= 0) return a;
        if(t >= 1) return b;
        if(a.is_array() && b.is_array() && !a.empty() && !b.empty()) {
            ofJson out = ofJson::array();
            for(size_t i = 0; i < std::max(a.size(), b.size()); ++i)
                out.push_back(lerp(a[std::min(i,a.size()-1)], b[std::min(i,b.size()-1)], t));
            return out;
        }
        return number(a) * (1-t) + number(b) * t;
    }
private:
    enum class Kind { Activation, Depth, Mode };
    struct Entry {
        std::string id;
        Kind kind;
        int count = 0;
        Read read, enabled;
        std::string selectorKey;
        std::function<ofJson()> enabledCurrent;
        ofJson fallback, effective, enabledDefault;
        const ofAbstractParameter* selector = nullptr;
    };
    std::map<const ofAbstractParameter*, Entry> entries;
    bool inApply = false;
    std::function<bool(const std::string&)> blockedControls;
    bool isBlocked(const std::string& key) const { return blockedControls && blockedControls(key); }
    template<class T> Entry& add(ofParameter<T>& p, Kind kind, int count, Read read) {
        auto& e = entries[&p];
        e.id = p.getEscapedName(); e.kind = kind; e.count = count;
        e.read = read ? std::move(read) : field(e.id);
        e.fallback = ofJson(p.get());
        return e;
    }
    static float number(const ofJson& j) {
        if(j.is_boolean()) return j.get<bool>() ? 1.0f : 0.0f;
        if(j.is_string()) {
            const auto str = j.get<std::string>();
            if(str == "true") return 1;
            try { return std::stof(str); } catch(...) { return 0; }
        }
        return j.is_number() ? j.get<float>() : 0.0f;
    }
    static std::vector<float> oneHot(int index, int count) {
        std::vector<float> w(count, 0);
        if(count > 0) w[std::clamp(index, 0, count-1)] = 1;
        return w;
    }
    static bool hasEffective(const ofJson& j, const std::string& id) {
        return j.contains(metadata) && j[metadata].is_object() && j[metadata].contains(id);
    }
    static ofJson scale(const ofJson& j, float amount) {
        if(j.is_array()) {
            ofJson out = ofJson::array();
            for(const auto& v : j) out.push_back(number(v) * amount);
            return out;
        }
        return number(j) * amount;
    }
    static ofJson endpoint(const Entry& e, const ofJson& j) {
        if(hasEffective(j, e.id)) return j[metadata][e.id];
        auto v = e.read(j);
        if(v.is_null()) v = e.fallback;
        if(e.kind == Kind::Mode) return oneHot(int(number(v)), e.count);
        if(e.kind == Kind::Activation) return std::clamp(number(v), 0.0f, 1.0f);
        return v;
    }
};
