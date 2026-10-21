#include <nlohmann/json.hpp>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using ofJson = nlohmann::json;
int ofToInt(const std::string& s) { return std::stoi(s); }
template<class T> T ofClamp(T v,T lo,T hi) { return std::clamp(v,lo,hi); }
struct Handle {
    bool connected = false;
    bool hasInConnection() const { return connected; }
    bool hasOutConnections() const { return false; }
};
struct Timeline {
    std::set<std::string> hidden;
    bool isHidden(const std::string& key) { return hidden.count(key); }
    void reveal(const std::string& key) { hidden.erase(key); }
    void load(const ofJson& j,const std::string& key) {
        hidden.clear();
        if(j.contains(key)) for(const auto& k:j[key]) hidden.insert(k.get<std::string>());
    }
};
template<class T> struct ClockParameter {
    std::string name;
    T value;
    std::string getEscapedName() const { return name; }
    ClockParameter& operator=(T next) { value = next; return *this; }
};
struct scRhythmBox {
    static constexpr int MAX_TRACKS = 8;
    int numTracks = 0;
    bool legacyPublishing = false;
    ClockParameter<bool> syncToTransportP{"Sync To Transport", true};
    ClockParameter<float> beatOffsetP{"Beat Offset", 0.0f};
    template<class T> void deserializeParameter(ofJson& json, ClockParameter<T>& parameter) {
        parameter.value = json.at(parameter.name).template get<T>();
    }
    std::vector<std::function<void()>> pendingTrackEdits;
    std::vector<std::string> legacyControlKeys = {"Reset", "Play", "Tracks", "Slot", "Embed", "MasterVol", "Vol", "Prob", "StepProb", "Cut", "Res", "Pan", "Mute", "Solo", "Swing", "Transpose", "GTranspose"};
    std::map<std::string,std::shared_ptr<Handle>> snapPublished;
    Timeline timeline;
    void publishSnap(const std::string& k) { if(!snapPublished.count(k)) snapPublished[k] = std::make_shared<Handle>(); }
    void unpublishSnap(const std::string& k) { if(snapPublished.count(k) && !snapPublished[k]->connected) snapPublished.erase(k); }
    void refreshBrowse(const std::string&) {}
    void setNumTracks(int n) { numTracks = n; }
    void loadBeforeConnections(ofJson&);
    void presetHasLoaded();
};
#include "preset_methods.h"
int main() {
    scRhythmBox box;
    ofJson old = {{"Tracks", "2"}, {"Vol", {.2f,.3f}}, {"rhythmBoxSnapPublished", {"Morph"}}};
    box.loadBeforeConnections(old);
    assert(box.numTracks == 2);
    assert(!box.syncToTransportP.value && box.beatOffsetP.value == 0.0f);
    // Every old wire endpoint exists before the container reconnects it.
    for(const auto& key:box.legacyControlKeys) assert(box.snapPublished.count(key));
    box.snapPublished["Vol"]->connected = true;
    box.presetHasLoaded();
    assert(box.snapPublished.count("Vol") && box.snapPublished.count("Morph"));
    assert(!box.snapPublished.count("Tracks") && !box.snapPublished.count("MasterVol"));
    // New presets restore just their chosen ports and timeline visibility.
    box.snapPublished["Vol"]->connected = false;
    ofJson current = {{"numTracks", 3}, {"rhythmBoxPublished", {"MasterVol", "Morph"}}, {"rhythmBoxTimeline", {"MasterVol"}}};
    current["Sync To Transport"] = true; current["Beat Offset"] = 0.5f;
    box.loadBeforeConnections(current);
    assert(box.syncToTransportP.value && box.beatOffsetP.value == 0.5f);
    assert(box.numTracks == 3 && box.snapPublished.size() == 2);
    assert(box.timeline.isHidden("MasterVol"));
    current["rhythmBoxTimeline"] = ofJson::array();
    box.loadBeforeConnections(current);
    assert(!box.timeline.isHidden("MasterVol"));
    // Legacy structural track counts and explicit empty boxes are preserved.
    assert(inferRhythmBoxTrackCount(ofJson{{"trackConfigs", ofJson::array({{}, {}, {}, {}})}},0) == 4);
    assert(inferRhythmBoxTrackCount(ofJson{{"numTracks",0},{"Tracks",8}},8) == 0);
    assert(inferRhythmBoxTrackCount(ofJson{{"Tracks","8"}},0) == 8);
    std::cout << "PASS: legacy connection endpoints, connected-port retention, minimal publication, timeline visibility and track counts\n";
}
