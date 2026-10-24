#pragma once
#include "ofJson.h"
#include <string>
#include <vector>

// Convert saved mode selectors to the boolean controls. Auto now means gated;
// only an explicit Drone (2) or Pulse (3) selection enables continuous playback.
namespace scDroneMode {
inline std::string renamed(const std::string& key) {
    for(const std::string prefix : {"Gate_Mode", "Env_Mode"}) {
        if(key == prefix) return "Drone";
        if(key.rfind(prefix + "_", 0) == 0) {
            const auto suffix = key.substr(prefix.size() + 1);
            if(!suffix.empty() && suffix.find_first_not_of("0123456789") == std::string::npos)
                return "Drone_" + suffix;
        }
    }
    return key;
}
inline int mode(const ofJson& value) {
    try {
        if(value.is_number()) return value.get<int>();
        if(value.is_string()) return std::stoi(value.get<std::string>());
    } catch(...) {}
    return 0;
}
inline void migrate(ofJson& json, bool hasPulse = false) {
    if(json.is_string()) {
        json = renamed(json.get<std::string>());
    } else if(json.is_array()) {
        for(auto& value : json) migrate(value, hasPulse);
    } else if(json.is_object()) {
        std::vector<std::string> oldKeys;
        for(auto it = json.begin(); it != json.end(); ++it) {
            if(renamed(it.key()) != it.key()) oldKeys.push_back(it.key());
            else migrate(it.value(), hasPulse);
        }
        for(const auto& oldKey : oldKeys) {
            const auto key = renamed(oldKey);
            const int selected = mode(json[oldKey]);
            if(!json.contains(key)) json[key] = selected == 2;
            if(hasPulse && !json.contains("Pulse")) json["Pulse"] = selected == 3;
            json.erase(oldKey);
        }
    }
}
inline ofJson migrated(ofJson json, bool hasPulse = false) {
    migrate(json, hasPulse);
    if(json.is_object()) {
        auto defaults = [hasPulse](ofJson& state) {
            if(!state.contains("Drone")) state["Drone"] = false;
            if(hasPulse && !state.contains("Pulse")) state["Pulse"] = false;
        };
        defaults(json);
        if(json.contains("controls") && json["controls"].is_object()) defaults(json["controls"]);
    }
    return json;
}
}
