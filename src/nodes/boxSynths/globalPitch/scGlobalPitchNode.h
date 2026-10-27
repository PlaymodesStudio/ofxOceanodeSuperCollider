//
//  scGlobalPitchNode.h
//  ofxOceanodeSuperCollider
//
//  Global Pitch: wireless pitch controls for every pitched destination in the
//  patch (pitched Boxes, generic SynthDefs, FM7), Pitch cables and piano-roll
//  notes included (scGlobalPitch.h):
//    Scale         the notes move to the nearest degree (pitch classes 0-11)
//    Scale Bypass  turns the scale off
//    Transpose     semitones, added after the scale (a transpose changes the key)
//    Mod 12        a transposed note wraps into its own octave, which starts at
//    Base Note     (C .. B): it never leaves that octave (cf. Fit Notes In Range)
//    Detune        each voice's own fixed random offset within +-cents, never quantized
//  The inspector lists the destinations with a tick per feature (SampleBox
//  and SlicerBox start unticked). Several Global Pitch nodes show one state.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "../shared/scGlobalPitch.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <functional>

class scGlobalPitchNode : public ofxOceanodeNodeModel {
public:
    scGlobalPitchNode() : ofxOceanodeNodeModel("Global Pitch") {
        description = "Scale, Transpose and Detune for every pitched Box and SynthDef in the patch, without cables: "
                      "Oceanode values, Pitch cables and piano-roll notes, held notes included. Notes move to the "
                      "nearest degree of Scale (pitch classes 0-11), then Transpose (semitones) is added (Mod 12: wrapped "
                      "into the note's own octave from Base Note), then each "
                      "voice's own random Detune (within +-cents). The inspector chooses which destinations follow "
                      "each one.";
    }
    ~scGlobalPitchNode() {
        auto& all = instances();
        all.erase(std::remove(all.begin(), all.end(), this), all.end());
        if(!all.empty()) return;   // the last one: the patch has no Global Pitch any more
        scGlobalPitch::setTranspose(0.0f);
        scGlobalPitch::setTransposeWrap(false, 0);
        scGlobalPitch::setScale({}, false);
        scGlobalPitch::setDetune(0.0f);
        for(int f = 0; f < scGlobalPitch::NumFeatures; f++) scGlobalPitch::setOverridesText(feature(f), "");
    }

    void setup() override {
        const auto& current = scGlobalPitch::scale();
        addParameter(scale.set("Scale", current.empty() ? std::vector<float>{0, 2, 4, 5, 7, 9, 11} : current,
                               std::vector<float>(1, 0.0f), std::vector<float>(1, 11.0f)));
        addParameter(bypass.set("Scale Bypass", !current.empty() && !scGlobalPitch::scaleOn()));
        addParameter(transpose.set("Transpose", scGlobalPitch::transpose(), -48.0f, 48.0f));
        addParameter(wrap.set("Mod 12", scGlobalPitch::transposeWraps()));
        addParameterDropdown(wrapBase, "Base Note", scGlobalPitch::transposeBase(),
                             {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"});
        addParameter(detune.set("Detune", scGlobalPitch::detune(), 0.0f, 100.0f));
        // the destinations that differ from their default, one string per feature (scGlobalPitch::overridesText)
        static const char* names[scGlobalPitch::NumFeatures] = {"Transpose Destinations", "Scale Destinations", "Detune Destinations"};
        for(int f = 0; f < scGlobalPitch::NumFeatures; f++) {
            addInspectorParameter(destinations[f].set(names[f], scGlobalPitch::overridesText(feature(f))));
            listeners.push(destinations[f].newListener([this, f](std::string& text){
                scGlobalPitch::setOverridesText(feature(f), text);
                share([f, text](scGlobalPitchNode* n){ n->destinations[f] = text; });
            }));
        }
        instances().push_back(this);

        auto sendScale = [this](){
            const std::vector<float> s = scale.get();
            const bool b = bypass.get();
            scGlobalPitch::setScale(s, !b);
            share([s, b](scGlobalPitchNode* n){ n->scale = s; n->bypass = b; });
        };
        listeners.push(scale.newListener([sendScale](std::vector<float>&){ sendScale(); }));
        listeners.push(bypass.newListener([sendScale](bool&){ sendScale(); }));
        listeners.push(transpose.newListener([this](float& v){
            scGlobalPitch::setTranspose(v);
            share([v](scGlobalPitchNode* n){ n->transpose = v; });
        }));
        auto sendWrap = [this](){
            const bool on = wrap.get();
            const int base = wrapBase.get();
            scGlobalPitch::setTransposeWrap(on, base);
            share([on, base](scGlobalPitchNode* n){ n->wrap = on; n->wrapBase = base; });
        };
        listeners.push(wrap.newListener([sendWrap](bool&){ sendWrap(); }));
        listeners.push(wrapBase.newListener([sendWrap](int&){ sendWrap(); }));
        listeners.push(detune.newListener([this](float& v){
            scGlobalPitch::setDetune(v);
            share([v](scGlobalPitchNode* n){ n->detune = v; });
        }));
        sendScale();
    }

    // one table for the three destination strings: a row per destination, a tick per feature
    bool drawInspectorParameter(ofAbstractParameter& parameter) override {
        if(parameter.isReferenceTo(destinations[1]) || parameter.isReferenceTo(destinations[2])) return true;
        if(!parameter.isReferenceTo(destinations[0])) return false;
        std::vector<scGlobalPitch::Follower> list = scGlobalPitch::destinations();
        std::sort(list.begin(), list.end(), [](auto& a, auto& b){
            return scGlobalPitch::key(a.node) < scGlobalPitch::key(b.node);
        });
        ImGui::TextUnformatted("Destinations");
        if(list.empty()) { ImGui::TextDisabled("Nothing pitched in the patch"); return true; }
        std::array<bool, scGlobalPitch::NumFeatures> changed{};
        if(ImGui::BeginTable("##globalPitchDestinations", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Scale");
            ImGui::TableSetupColumn("Transp.");
            ImGui::TableSetupColumn("Detune");
            ImGui::TableHeadersRow();
            static const int order[3] = {scGlobalPitch::Scale, scGlobalPitch::Transpose, scGlobalPitch::Detune};
            for(auto& d : list) {
                const auto k = scGlobalPitch::key(d.node);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k.c_str());
                if(!d.byDefault && ImGui::IsItemHovered()) ImGui::SetTooltip("Left out by default");
                for(int f : order) {
                    ImGui::TableNextColumn();
                    bool on = scGlobalPitch::follows(d.node, feature(f));
                    ImGui::PushID((k + "/" + ofToString(f)).c_str());
                    if(ImGui::Checkbox("##on", &on)) { scGlobalPitch::setFollows(d.node, feature(f), on); changed[f] = true; }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if(ImGui::SmallButton("Defaults")) {
            for(int f = 0; f < scGlobalPitch::NumFeatures; f++) { scGlobalPitch::setOverridesText(feature(f), ""); changed[f] = true; }
        }
        for(int f = 0; f < scGlobalPitch::NumFeatures; f++)
            if(changed[f]) destinations[f] = scGlobalPitch::overridesText(feature(f));
        return true;
    }

private:
    static scGlobalPitch::Feature feature(int f) { return static_cast<scGlobalPitch::Feature>(f); }
    static std::vector<scGlobalPitchNode*>& instances() { static std::vector<scGlobalPitchNode*> v; return v; }
    // the other Global Pitch nodes show the same state
    void share(std::function<void(scGlobalPitchNode*)> apply) {
        static bool sharing = false;
        if(sharing) return;
        sharing = true;
        for(auto* n : instances()) if(n != this) apply(n);
        sharing = false;
    }

    ofParameter<std::vector<float>> scale;
    ofParameter<bool> bypass;
    ofParameter<float> transpose, detune;
    ofParameter<bool> wrap;
    ofParameter<int> wrapBase;
    std::array<ofParameter<std::string>, scGlobalPitch::NumFeatures> destinations;
    ofEventListeners listeners;
};
