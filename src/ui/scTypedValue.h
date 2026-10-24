//
//  scTypedValue.h
//  ofxOceanodeSuperCollider
//
//  A control's value typed by hand in its right-click menu (PolyComb,
//  EchoBox, SampleBox; GrainBox / AdditiveBox / SlicerBox have their own).
//  Numbers as they are, vectors "a, b, c" (or one value for all), switches
//  0 / 1 / on / off. Enter applies and closes; leaving the field applies.
//

#pragma once

#include "ofMain.h"
#include "imgui.h"
#include <array>
#include <functional>
#include <map>
#include <string>

namespace scTyped {

// A JSON value as text
inline std::string text(const ofJson& v){
    if(v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if(v.is_number_integer()) return ofToString(v.get<int>());
    if(v.is_number()) return ofToString(v.get<float>(), 4);
    if(v.is_array()){
        std::string s;
        for(size_t i = 0; i < v.size(); i++){
            if(i) s += ", ";
            s += v[i].is_number() ? ofToString(v[i].get<float>(), 4) : v[i].dump();
        }
        return s;
    }
    if(v.is_string()) return v.get<std::string>();
    return v.dump();
}

// Text in the type of `like` (the control's current value)
inline bool parse(const std::string& raw, const ofJson& like, ofJson& out){
    std::string t = ofTrim(raw);
    if(t.empty()) return false;
    t.erase(std::remove(t.begin(), t.end(), '['), t.end());
    t.erase(std::remove(t.begin(), t.end(), ']'), t.end());
    if(like.is_boolean()){
        const std::string l = ofToLower(t);
        out = (l == "1" || l == "true" || l == "on" || l == "yes");
        return true;
    }
    if(like.is_array()){
        ofJson a = ofJson::array();
        for(auto& part : ofSplitString(t, ",", true, true)) a.push_back(ofToFloat(part));
        if(a.empty()) return false;
        out = a;
        return true;
    }
    if(like.is_number_integer()){ out = (int)std::lround(ofToFloat(t)); return true; }
    if(like.is_number()){ out = ofToFloat(t); return true; }
    out = t;
    return true;
}

// The field at the top of a control's menu: the value now, typed anew.
// getText / applyText: the control's value as text and back.
inline void field(const std::string& key, const std::function<std::string()>& getText,
                  const std::function<void(const std::string&)>& applyText, bool isVector){
    static std::map<std::string, std::array<char, 1024>> buffers;   // one per control
    auto& buffer = buffers[key];
    const bool appearing = ImGui::IsWindowAppearing();
    if(appearing) std::snprintf(buffer.data(), buffer.size(), "%s", getText().c_str());
    ImGui::TextDisabled("%s", isVector ? "Value (one for all, or a, b, c...)" : "Value");
    ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
    if(appearing) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText(("##typed" + key).c_str(), buffer.data(), buffer.size(),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if(enter || ImGui::IsItemDeactivatedAfterEdit()){
        applyText(std::string(buffer.data()));
        if(enter) ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
}

// For JSON controls (save() / load()): the field with the value's own type
inline void jsonField(const std::string& key, const std::function<ofJson()>& save,
                      const std::function<void(const ofJson&)>& load){
    const ofJson now = save();
    field(key, [&](){ return text(now); }, [&](const std::string& t){
        ofJson v;
        if(parse(t, now, v)) load(v);
    }, now.is_array());
}

} // namespace scTyped
