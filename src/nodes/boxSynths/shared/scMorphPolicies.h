#pragma once

#include "ofMain.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <utility>

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
    // Numeric switches can have signed endpoints (e.g. feedback polarity).
    // Keep the editor's discrete behavior, interpolate its effective DSP value.
    template<class T> void continuous(ofParameter<T>& p, Read read = {}) {
        add(p, Kind::Value, 0, std::move(read));
    }
    // Engines can interpolate evaluated tables/coefficients instead of a
    // selector's editor value. Captures preserve the resulting array in metadata.
    template<class T> void resolved(ofParameter<T>& p,Read evaluate) {
        add(p,Kind::Value,0,std::move(evaluate));
    }
    template<class T> ofJson resolvedValue(const ofParameter<T>& p) const {
        auto it=entries.find(&p);
        return hasEffective(p) ? it->second.effective : ofJson();
    }
    // Modes whose DSP already runs parallel alternatives. weights() blends
    // endpoints directly (never visits unrelated intermediate enum values).
    template<class T> void mode(ofParameter<T>& p, int count, Read read = {}) {
        add(p, Kind::Mode, std::max(1, count), std::move(read));
    }
    // Stateful modes with a neutral audible path: fade their depth to zero,
    // switch while neutral, then restore it. Unlike a whole-output mute, the
    // carrier/dry signal keeps sounding. Hosts pin only the declared selectors.
    template<class M,class D> void neutralDepth(ofParameter<M>& mode,ofParameter<D>& depth,const std::string& container="") {
        auto& e=add(depth,Kind::NeutralDepth,0,field(depth.getEscapedName(),container));
        e.neutral.push_back({&mode,mode.getEscapedName(),container,field(mode.getEscapedName(),container),ofJson(mode.get())});
    }
    void pinSelectors(ofJson& a,ofJson& b,float t) const {
        for(const auto& item:entries)for(const auto& mode:item.second.neutral){
            if(isBlocked(mode.key))continue;
            auto x=mode.read(a),y=mode.read(b);
            if(x.is_null())x=mode.fallback;if(y.is_null())y=mode.fallback;
            const auto value=t<0.5f ? x : y;
            if(mode.container.empty()){a[mode.key]=value;b[mode.key]=value;}
            else {a[mode.container][mode.key]=value;b[mode.container][mode.key]=value;}
        }
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
    template<class T> bool hasEffective(const ofParameter<T>& p) const {
        auto it=entries.find(&p);
        return !isBlocked(p.getEscapedName()) && it!=entries.end() && !it->second.effective.is_null();
    }
    template<class T> float neutralGain(const ofParameter<T>& p) const {
        auto it=entries.find(&p);
        return hasEffective(p) && it!=entries.end() ? it->second.modulationGain : 1.0f;
    }
    template<class T> std::vector<float> weights(const ofParameter<T>& p,int voice=0) const {
        auto it=entries.find(&p);
        if(it==entries.end())return {};
        const auto& e=it->second;
        if(e.count<1)return {};
        // Effective mode tables can contain a row for every voice. Reading a
        // single row must not copy that entire JSON table on every DSP lookup.
        ofJson current;
        const ofJson* table=&e.effective;
        if(!hasEffective(p)){current=modeWeights(ofJson(p.get()),e.count);table=&current;}
        const auto& value=*table;
        if(!value.is_array() || value.empty())return oneHot(ofJson(0),e.count);
        const auto& row=value.front().is_array() ? value[size_t(std::max(0,voice))%value.size()] : value;
        if(!row.is_array() || int(row.size())!=e.count)return oneHot(ofJson(0),e.count);
        std::vector<float> out(e.count);float sum=0;
        for(int i=0;i<e.count;++i){
            if(!row[i].is_number())return oneHot(ofJson(0),e.count);
            out[i]=row[i].template get<float>();
            if(!std::isfinite(out[i]) || out[i]<0)return oneHot(ofJson(0),e.count);
            sum+=out[i];
        }
        if(!std::isfinite(sum) || sum<=0)return oneHot(ofJson(0),e.count);
        for(auto& v:out)v/=sum;
        return out;
    }
    template<class T,class Evaluate> float evaluate(const ofParameter<T>& p,int voice,int fallback,Evaluate evaluateMode) const {
        if(!hasEffective(p))return evaluateMode(fallback);
        return evaluateWeights(weights(p,voice),fallback,evaluateMode);
    }
    template<class Evaluate> static float evaluateWeights(const std::vector<float>& w,int fallback,Evaluate evaluateMode) {
        if(w.empty())return evaluateMode(fallback);
        float value=0;
        for(size_t i=0;i<w.size();++i)if(w[i]>0)value+=w[i]*evaluateMode(int(i));
        return value;
    }
    // One stateful algorithm with a dry neutral path. Bypass->mode starts at
    // the first nonzero weight; competing algorithms change only at zero wet.
    // This also works when an interrupted morph contains more than two modes.
    static std::pair<int,float> neutralMode(const std::vector<float>& weights,int off=0) {
        int mode=off;float total=0,largest=0;
        for(size_t i=0;i<weights.size();++i)if(int(i)!=off){
            total+=weights[i];if(weights[i]>largest){largest=weights[i];mode=int(i);}
        }
        return {mode,std::max(0.f,2*largest-total)};
    }
    bool applying() const { return inApply; }
    // Called before ordinary parameter listeners send audio values. Manual
    // edits invalidate overrides; snapshot application leaves them intact.
    void edited(const ofAbstractParameter& p) {
        if(inApply) return;
        auto it = entries.find(&p);
        if(it != entries.end()) it->second.effective = {};
        for(auto& item : entries) {
            if(item.second.selector==&p)item.second.effective={};
            for(const auto& mode:item.second.neutral)if(mode.parameter==&p)item.second.effective={};
        }
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
            bool changing=false;
            for(const auto& mode:e.neutral){
                if(isBlocked(mode.key))continue;
                auto ma=mode.read(a),mb=mode.read(b);
                if(ma.is_null())ma=mode.fallback;if(mb.is_null())mb=mode.fallback;
                changing|=ma!=mb;
            }
            e.modulationGain=changing ? std::abs(1-2*t) : 1.0f;
            e.effective=changing ? (t<0.5f ? scale(x,1-2*t) : scale(y,2*t-1)) : lerp(x,y,t);
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
    enum class Kind { Activation, Depth, Mode, Value, NeutralDepth };
    struct NeutralSelector {
        const ofAbstractParameter* parameter;
        std::string key,container;
        Read read;
        ofJson fallback;
    };
    struct Entry {
        std::vector<NeutralSelector> neutral;
        std::string id;
        Kind kind;
        int count = 0;
        float modulationGain=1.0f;
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
    static ofJson modeWeights(const ofJson& value,int count) {
        if(value.is_array()) {
            ofJson result=ofJson::array();
            for(const auto& v:value)result.push_back(oneHot(int(number(v)),count));
            return result;
        }
        return oneHot(int(number(value)),count);
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
        if(e.kind == Kind::Mode) return modeWeights(v,e.count);
        if(e.kind == Kind::Activation) return std::clamp(number(v), 0.0f, 1.0f);
        if(e.kind == Kind::NeutralDepth && v.is_boolean())return number(v);
        return v;
    }
};
