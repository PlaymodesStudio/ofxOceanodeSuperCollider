//
//  scDropdowns.h
//  ofxOceanodeSuperCollider
//
//  The option names of a node's window combos, so a combo published to the
//  node is an Oceanode dropdown (an int, or a float switch, with names)
//  instead of a bare slider. The node names each combo's options once at
//  setup (a preset can publish before the window is ever drawn); a list that
//  changes with the sound (a filter's types, a pad's channels) is set again
//  where the combo is drawn, and reaches the published parameter at once.
//
//  Usage:
//      setup():    dropdowns.set(p.getEscapedName(), {"Auto", "Gated", "Drone"});
//      publish():  auto h = addParameter(p); dropdowns.apply(key, h);
//      combo():    const auto& items = dropdowns.get(key);   // what it draws
//                  dropdowns.set(key, liveItems, publishedHandle);  // when it changes
//
//  Header-only, so the projects need no new file.
//

#pragma once

#include "ofxOceanodeParameter.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class scDropdowns {
public:
    using Handle = std::shared_ptr<ofxOceanodeAbstractParameter>;

    // The options of a key; to its published parameter too when they change
    void set(const std::string& key, const std::vector<std::string>& items, const Handle& published = nullptr) {
        auto& current = options[key];
        if(current == items) return;
        current = items;
        apply(key, published);
    }
    void set(const std::string& key, const std::vector<const char*>& items, const Handle& published = nullptr) {
        set(key, std::vector<std::string>(items.begin(), items.end()), published);
    }
    // set(key, {"A", "B"}): the literal list (an exact match, not ambiguous)
    void set(const std::string& key, std::initializer_list<const char*> items, const Handle& published = nullptr) {
        set(key, std::vector<std::string>(items.begin(), items.end()), published);
    }
    bool has(const std::string& key) const { return options.count(key) > 0; }
    const std::vector<std::string>& get(const std::string& key) const {
        static const std::vector<std::string> none;
        const auto it = options.find(key);
        return it == options.end() ? none : it->second;
    }
    // ImGui::Combo wants const char*: valid while the key's list is unchanged
    std::vector<const char*> names(const std::string& key) const {
        std::vector<const char*> out;
        for(const auto& s : get(key)) out.push_back(s.c_str());
        return out;
    }
    // A freshly published parameter: its names (no-op for keys without a combo)
    void apply(const std::string& key, const Handle& h) const {
        const auto it = options.find(key);
        if(it != options.end()) setNames(h, it->second);
    }
    // Names on one parameter (int / float / their vectors), only when they differ
    static void setNames(const Handle& h, const std::vector<std::string>& items) {
        if(!h) return;
        const std::string type = h->valueType();
        auto put = [&](auto& p) { if(p.getDropdownOptions() != items) p.setDropdownOptions(items); };
        if(type == typeid(int).name()) put(h->cast<int>());
        else if(type == typeid(float).name()) put(h->cast<float>());
        else if(type == typeid(std::vector<int>).name()) put(h->cast<std::vector<int>>());
        else if(type == typeid(std::vector<float>).name()) put(h->cast<std::vector<float>>());
    }

private:
    std::map<std::string, std::vector<std::string>> options;
};
