#pragma once
#include "scSnapshots.h"
#include "ofJson.h"
#include "ofParameter.h"
#include <typeinfo>

// MacroMixer: a Box blends a saved patch on its running synths through the
// same policy-aware path as its own snapshots, instead of MacroMixer replacing
// the node (a second instance, crossfaded) whenever its saved sound differs.
//
// The Box declares the saved keys holding its sound in morphKeys().blended,
// converts that saved state to its snapshot format once (snapshot), and
// forwards morphBegin / morphApply / morphEnd here. Counts that need other
// synths (Voices) are MorphHint::Switch: MacroMixer sets them once, half way.
// Output channels stay Structural.
struct scBoxMacroMorph {
    ofJson from, to;   // the sound at the start, the arrival (snapshot format)

    bool begin(scSnapshots& snaps, ofJson target, std::string& reason) {
        if(!target.is_object()) { reason = "no saved sound"; return false; }
        from = snaps.captureForExternalMorph();   // the current sound, including an interrupted blend
        to = std::move(target);
        return true;
    }
    // A saved value in the control's own format: Oceanode saves a published
    // control as a string, a one-value vector as a number
    static ofJson nativeValue(const ofAbstractParameter& p, const ofJson& v) {
        const std::string t = p.valueType();
        if(t == typeid(std::vector<float>).name() || t == typeid(std::vector<int>).name()) return v.is_array() ? v : ofJson::array({v});
        if(!v.is_string()) return v;
        const std::string s = v.get<std::string>();
        if(t == typeid(float).name()) return ofToFloat(s);
        if(t == typeid(int).name()) return ofToInt(s);
        if(t == typeid(bool).name()) return s == "1" || s == "true";
        return v;
    }
    void apply(scSnapshots& snaps, float t, bool switched) {
        if(from.is_object()) snaps.applyExternalMorph(from, to, t, switched);
    }
    // cancelled: back to the sound captured at the start
    void end(scSnapshots& snaps, bool completed) {
        if(!completed && from.is_object()) snaps.applyExternalMorph(from, from, 1.0f, true);
        from = to = ofJson();
    }
};
