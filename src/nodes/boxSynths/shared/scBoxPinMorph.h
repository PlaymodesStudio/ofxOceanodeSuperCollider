#pragma once
#include "ofJson.h"
#include <algorithm>
#include <string>
#include <vector>

// MacroMixer: a saved patch that publishes other controls as pins morphs on
// the running Box instead of replacing it. The Box lists its "...Published"
// key in morphKeys().ignored and forwards the node's pin hooks here:
//   pins / value  read a saved state: the published list, and a control's
//                 value in the saved sound (a cable leaving a pin the target
//                 does not publish glides to it);
//   begin         publishes the target's missing pins, before MacroMixer
//                 plans the cables into them;
//   end           once no morph cable feeds them: completed, unpublishes the
//                 pins the target lacks; cancelled, the ones begin added.
// A published control is the sound's own control, so its value keeps
// blending with the rest of the sound whichever side publishes it, and a
// cable into it (morphBlocked) takes over from that blend.
struct scBoxPinMorph {
    std::string listKey, soundKey;
    std::vector<std::string> target, added;

    scBoxPinMorph(std::string list, std::string sound) : listKey(std::move(list)), soundKey(std::move(sound)) {}

    std::vector<std::string> pins(const ofJson& saved) const {
        std::vector<std::string> keys;
        if(saved.is_object() && saved.contains(listKey) && saved.at(listKey).is_array())
            for(const auto& k : saved.at(listKey)) if(k.is_string()) keys.push_back(k.get<std::string>());
        return keys;
    }
    bool value(const ofJson& saved, const std::string& name, ofJson& out) const {
        if(!saved.is_object() || !saved.contains(soundKey)) return false;
        const auto& sound = saved.at(soundKey);
        if(!sound.is_object() || !sound.contains(name) || sound.at(name).is_null()) return false;
        out = sound.at(name);
        return true;
    }
    template<typename Publish>
    void begin(const ofJson& saved, std::vector<std::string> published, Publish publish) {
        target = pins(saved); added.clear();
        for(const auto& k : target) if(std::find(published.begin(), published.end(), k) == published.end()) {
            publish(k); added.push_back(k);
        }
    }
    template<typename Unpublish>
    void end(bool completed, std::vector<std::string> published, Unpublish unpublish) {
        std::vector<std::string> drop;
        if(completed) {
            for(const auto& k : published) if(std::find(target.begin(), target.end(), k) == target.end()) drop.push_back(k);
        } else drop = added;
        for(const auto& k : drop) unpublish(k);
        target.clear(); added.clear();
    }
};
