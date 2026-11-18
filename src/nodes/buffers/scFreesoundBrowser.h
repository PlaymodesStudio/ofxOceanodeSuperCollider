//
//  scFreesoundBrowser.h
//  ofxOceanodeSuperCollider
//
//  Standalone freesound.org browser node (header-only). Its window is the
//  shared sample browser (scSampleBrowser) opened on its Freesound side:
//    • click a sound to hear it, drag it onto anything that takes a sample;
//    • double-click or Enter sends its file to the Path output, to connect
//      to the Path of an SC Buffer node.
//  The path is written like the SC Buffer file dialog writes it (relative to
//  data/Supercollider/Samples when inside it) and saved with the preset, so
//  a connected Buffer gets the same file back on load.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "../../serverManager.h"
#include "../../samples/scSampleBrowser.h"

class scFreesoundBrowser : public ofxOceanodeNodeModel {
public:
    scFreesoundBrowser(vector<serverManager*> servers)
        : ofxOceanodeNodeModel("SC Freesound") {
        for(auto* sm : servers) if(sm && sm->getServer()) { browser.setPreviewServer(sm->getServer()); break; }
    }

    void setup() override {
        description = "Searches freesound.org. Show opens the browser: click a sound to hear it, drag it onto a "
                      "sampler, or double-click it to send its file to Path (connect Path to an SC Buffer). "
                      "Needs a free Freesound API key, asked for the first time. Sounds are the site's preview "
                      "quality, kept as WAV in Supercollider/Samples/Freesound.";
        addParameter(showWindow.set("Show", false));
        addOutputParameter(path.set("Path", ""));

        std::string dir = ofToDataPath("Supercollider/Samples/Freesound", true);
        if(!std::filesystem::exists(dir)) dir = ofToDataPath("Supercollider/Samples", true);
        browser.refresh(dir);
        browser.setFreesoundMode(true);
        browser.onActivate = [this](const std::string& file) {
            std::string relative = file;
            ofStringReplace(relative, ofToDataPath("Supercollider/Samples/", true), "");
            path = relative;
        };
    }

    void draw(ofEventArgs&) override {
        if(!showWindow) { browser.stopPreview(); return; }
        const std::string title = "Freesound " + ofToString(getNumIdentifier());
        ImGui::SetNextWindowSize(ImVec2(360, 520), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(title.c_str(), (bool*)&showWindow.get())) {
            ImGui::BeginChild("##fsNodeBrowser", ImVec2(0, 0), false);
            browser.draw(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
            ImGui::EndChild();
        }
        ImGui::End();
    }

    void presetSave(ofJson& json) override { json["freesoundPath"] = path.get(); }
    void loadBeforeConnections(ofJson& json) override {
        if(json.contains("freesoundPath") && json["freesoundPath"].is_string())
            path = json["freesoundPath"].get<std::string>();
    }
    void macroSave(ofJson& json, string) override { presetSave(json); }
    void macroLoad(ofJson& json, string) override { loadBeforeConnections(json); }

private:
    ofParameter<bool>   showWindow;
    ofParameter<string> path;
    scSampleBrowser     browser{"scFreesoundBrowser", {".wav", ".aif", ".aiff", ".flac"}};
};
