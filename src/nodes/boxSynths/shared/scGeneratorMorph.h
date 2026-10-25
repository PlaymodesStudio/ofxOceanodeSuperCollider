#pragma once
#include "scModulationMorph.h"

// Effective generator settings travel with every Box's shared modulation state.
// Editor switches keep their types; DSP blends algorithms on one running clock.
struct scGeneratorMorph {
    struct LfoKeys { std::string noise, random, model, sync, hz, beats, sh, manual, region; std::string shared{}, centered{}, steps{}; };
    static ofJson read(const ofJson& sound, const std::string& key, const ofJson& fallback,
                       const scModulationMorph::Blocked& blocked) {
        return !key.empty() && !(blocked && blocked(key)) && sound.contains(key) ? sound[key] : fallback;
    }
    static float scalar(const ofJson& value, float fallback = 0) {
        const auto v = scModulationMorph::numbers(value);
        return v.empty() || !std::isfinite(v.front()) ? fallback : v.front();
    }
    static bool weightsValid(const ofJson& value) {
        if(!value.is_array() || value.size() != 6) return false;
        float sum = 0;
        for(const auto& v : value){
            if(!v.is_number()) return false;
            const float x = v.get<float>();
            if(!std::isfinite(x) || x < 0) return false;
            sum += x;
        }
        return std::isfinite(sum) && sum > 0;
    }
    static ofJson endpoint(const ofJson& sound, const std::string& metadata, const LfoKeys& keys,
                           const ofJson& defaults, const scModulationMorph::Blocked& blocked = {}) {
        auto field = [&](const std::string& key){ return read(sound, key, defaults.value(key, ofJson()), blocked); };
        const auto bank = sound.value(metadata, ofJson::object());
        const auto saved = bank.is_object() ? bank.value(keys.noise, ofJson::object()) : ofJson::object();
        const bool modeDriven = blocked && (blocked(keys.noise) || blocked(keys.random) || (!keys.model.empty() && blocked(keys.model)));
        ofJson state = ofJson::object();
        if(!modeDriven && saved.is_object() && saved.contains("weights") && weightsValid(saved["weights"])){
            auto weights = saved["weights"].get<std::vector<float>>();
            float sum = 0; for(float v : weights) sum += v;
            for(auto& v : weights) v /= sum;
            state["weights"] = weights;
            state["manual"] = std::clamp(scalar(!keys.manual.empty() && blocked && blocked(keys.manual) ? field(keys.manual) : saved.value("manual", ofJson(0))), 0.f, 1.f);
        }else{
            int mode = 0; // Classic, Noise, Stepped, Smooth, Jumps, Manual
            if(!keys.model.empty()){
                const int model = std::clamp(int(scalar(field(keys.model), 1)), 0, 3);
                mode = model == 0 ? 5 : model == 2 ? 1 : model == 3 ? 2 : 0;
            }else{
                const int random = std::clamp(int(scalar(field(keys.random))), 0, 3);
                mode = random > 0 ? random + 1 : scalar(field(keys.noise)) > .5f ? 1 : 0;
            }
            std::vector<float> weights(6, 0); weights[mode] = 1;
            state["weights"] = weights;
            state["manual"] = std::clamp(scalar(field(keys.manual)), 0.f, 1.f);
        }
        const bool rateDriven = blocked && (blocked(keys.sync) || blocked(keys.hz) || blocked(keys.beats) || blocked(keys.sh) || (!keys.region.empty() && blocked(keys.region)));
        bool savedRate = !rateDriven && saved.is_object();
        for(const char* key : {"hz", "beatRate", "regionRate", "shHz", "shBeatRate"}){
            if(!saved.contains(key) || !saved[key].is_array() || saved[key].empty()){ savedRate = false; break; }
            for(const auto& v : saved[key])
                if(!v.is_number() || !std::isfinite(v.get<float>()) || v.get<float>() < 0) savedRate = false;
        }
        if(savedRate){
            for(const char* key : {"hz", "beatRate", "regionRate", "shHz", "shBeatRate"}) state[key] = saved[key];
        }else{
            const bool sync = scalar(field(keys.sync), 1) > .5f;
            const bool region = !keys.region.empty() && scalar(field(keys.region)) > .5f;
            auto hz = scModulationMorph::numbers(field(sync || region ? keys.beats : keys.hz));
            if(hz.empty()) hz = {1};
            for(auto& v : hz) v = sync || region ? 1.f / std::max(.001f, v) : std::max(0.f, v);
            auto sh = scModulationMorph::numbers(field(keys.sh)); if(sh.empty()) sh = {0};
            for(auto& v : sh) v = std::max(0.f, v);
            state["hz"] = sync || region ? std::vector<float>(hz.size(), 0) : hz;
            state["beatRate"] = sync && !region ? hz : std::vector<float>(hz.size(), 0);
            state["regionRate"] = region ? hz : std::vector<float>(hz.size(), 0);
            state["shHz"] = sync ? std::vector<float>(sh.size(), 0) : sh;
            state["shBeatRate"] = sync ? sh : std::vector<float>(sh.size(), 0);
        }
        for(const auto& item : {std::pair<const char*, std::string>{"shared", keys.shared}, {"centered", keys.centered}, {"steps", keys.steps}}){
            const bool driven = !item.second.empty() && blocked && blocked(item.second);
            const float fallback = std::string(item.first) == "centered" ? 1.f : 0.f;
            state[item.first] = std::clamp(scalar(!driven && saved.is_object() && saved.contains(item.first)
                ? saved[item.first] : field(item.second), fallback), 0.f, std::string(item.first) == "steps" ? 32.f : 1.f);
        }
        return state;
    }
    static ofJson blend(const ofJson& a, const ofJson& b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        ofJson out = ofJson::object();
        for(const char* key : {"weights", "hz", "beatRate", "regionRate", "shHz", "shBeatRate"})
            out[key] = scModulationMorph::interpolate(scModulationMorph::numbers(a.at(key)), scModulationMorph::numbers(b.at(key)), t);
        for(const char* key : {"shared", "centered", "steps"}) out[key] = (1-t)*a.value(key, 0.f)+t*b.value(key, 0.f);
        const float manualWeight = out["weights"][5].get<float>();
        const float contribution = (1-t)*a["weights"][5].get<float>()*a["manual"].get<float>()
                                 + t*b["weights"][5].get<float>()*b["manual"].get<float>();
        out["manual"] = manualWeight > 0 ? contribution / manualWeight : 0.f;
        return out;
    }
};
