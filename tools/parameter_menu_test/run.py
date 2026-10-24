#!/usr/bin/env python3
"""Exercise the real shared menu/lifecycle with MIDI and timeline independently enabled.

Only the host/GUI boundaries are stubbed; scTimelineMenu.h/.cpp are compiled unchanged.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
'ofxOceanodeParameter.h': r'''
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <cstdio>
#include <typeinfo>
using ofJson = nlohmann::json;
inline float testTime = 0;
inline float ofGetElapsedTimef(){return testTime;}
struct ofAbstractParameter { virtual ~ofAbstractParameter() = default; virtual std::string getName() const = 0; };
constexpr int ofxOceanodeParameterFlags_NoGuiWidget = 128, ofxOceanodeParameterFlags_ReadOnly = 64;
struct ofxOceanodeAbstractParameter : ofAbstractParameter {
    std::string name, type = typeid(float).name(); int flags=0; float value=0;
    explicit ofxOceanodeAbstractParameter(std::string n):name(n){}
    std::string getName() const override{return name;}
    std::string getEscapedName() const{return name;}
    std::vector<std::string> getGroupHierarchyNames() const{return {"Synth_1"};}
    std::string valueType() const{return type;}
    int getFlags() const{return flags;} void setFlags(int f){flags=f;}
    std::string toString() const{return "0";} void fromString(std::string){}
};
struct Group {
    std::vector<std::shared_ptr<ofxOceanodeAbstractParameter>> values;
    bool contains(const std::string& key) const{for(auto& p:values)if(p->name==key)return true;return false;}
    ofAbstractParameter& get(const std::string& key){for(auto& p:values)if(p->name==key)return *p;throw 1;}
    ofAbstractParameter& get(int index){return *values.at(index);}
    int size() const{return int(values.size());}
};
struct ofColor { int r=0,g=0,b=0; };
inline std::string ofToString(int n){return std::to_string(n);}
''',
'ofxOceanodeNodeModel.h': r'''
#pragma once
#include "ofxOceanodeParameter.h"
struct ofxOceanodeContainer;
struct ofxOceanodeNodeModel {
    Group group; ofxOceanodeContainer* host=nullptr;
    Group& getParameterGroup(){return group;}
    ofxOceanodeContainer* getHostContainer(){return host;}
};
''',
'testTimeline.h': r'''
#pragma once
#include "ofxOceanodeParameter.h"
struct Timeline {
    struct Binding { std::string parameterPath,id; };
    struct Track { std::string id,name; bool isWaveTrack=false; std::vector<Binding> bindings; };
    bool bound=false; std::vector<Track> tracks;
    bool isParameterBound(ofxOceanodeAbstractParameter&){return bound;}
    bool isStepLaneCompatible(ofxOceanodeAbstractParameter&){return true;}
    const auto& getTracks(){return tracks;}
    std::string createTrack(){return "track";}
    std::string addBinding(std::string,ofxOceanodeAbstractParameter&){bound=true;return "binding";}
    void requestTrackRename(std::string,bool){} void removeTrack(std::string){}
    void requestNoteGroupSetup(ofxOceanodeAbstractParameter*){}
    void removeBinding(std::string,std::string){bound=false;}
    bool getParameterTrackColor(ofxOceanodeAbstractParameter&,ofColor&){return false;}
};
''',
'ofxOceanodeContainer.h': r'''
#pragma once
#include "testTimeline.h"
struct Binding { ofxOceanodeAbstractParameter* parameter; };
struct ofxOceanodeContainer {
    using Bindings = std::map<std::string,std::vector<std::shared_ptr<Binding>>>;
    Bindings midi,persistent; Timeline timeline;
    auto& getMidiBindings(){return midi;} auto& getPersistentMidiBindings(){return persistent;}
    static std::string key(ofxOceanodeAbstractParameter& p){return "Synth_1-|-"+p.name;}
    auto createMidiBinding(ofxOceanodeAbstractParameter& p){
        auto binding=std::make_shared<Binding>(Binding{&p});midi[key(p)].push_back(binding);return binding;
    }
    bool removeLastMidiBinding(ofxOceanodeAbstractParameter& p){
        auto& list=midi.at(key(p));list.pop_back();if(list.empty())midi.erase(key(p));return true;
    }
    void* getAutomationExtension(){return this;}
    auto& getTimelineManager(){return timeline;}
    std::string getTimelineParameterPath(ofxOceanodeAbstractParameter& p){return p.name;}
};
''',
'imgui.h': r'''
#pragma once
#include <string>
#include <vector>
#include <cstdarg>
struct ImVec2 {float x=0,y=0;ImVec2()=default;ImVec2(float a,float b):x(a),y(b){}};
constexpr int ImGuiInputTextFlags_EnterReturnsTrue=1, ImGuiInputTextFlags_AutoSelectAll=2, ImGuiSelectableFlags_Disabled=4;
#define IM_COL32(r,g,b,a) 0
namespace ImGui {
inline std::string click;
inline std::vector<std::string> seen;
inline bool Selectable(const char* label,bool=false,int flags=0){
    std::string s(label);s=s.substr(0,s.find("##"));seen.push_back(s);
    if(s==click && !(flags&ImGuiSelectableFlags_Disabled)){click.clear();return true;}return false;
}
inline bool BeginMenu(const char*){return true;} inline void EndMenu(){}
inline bool MenuItem(const char* s){return Selectable(s);}
inline bool BeginPopupContextItem(const char*){return false;}
inline void TextDisabled(const char*,...){} inline void Separator(){}
inline bool IsItemHovered(){return false;} inline void SetTooltip(const char*,...){}
inline bool IsWindowAppearing(){return false;} inline void SetNextItemWidth(float){}
inline void SetKeyboardFocusHere(){} inline bool InputText(const char*,char*,unsigned long,int){return false;}
inline bool IsItemDeactivatedAfterEdit(){return false;}inline void CloseCurrentPopup(){}inline void EndPopup(){}
inline ImVec2 GetItemRectMin(){return {};}inline ImVec2 GetItemRectMax(){return {};}
struct DrawList{void AddLine(ImVec2,ImVec2,unsigned int,float){}};
inline DrawList* GetWindowDrawList(){static DrawList d;return &d;}
}
'''
}
TEST = r'''
#include "ui/scTimelineMenu.h"
#include "ofxOceanodeContainer.h"
#include <cassert>
#include <iostream>
int main(){
    ofxOceanodeContainer host;ofxOceanodeNodeModel node;node.host=&host;
    scTimelineMenu menu;
    auto keys=[&](){std::vector<std::string> out;for(auto& p:node.group.values)out.push_back(p->name);return out;};
    menu.setup(&node,[&](const std::string& key){
        if(key=="missing")return;
        auto p=std::make_shared<ofxOceanodeAbstractParameter>(key);
        if(key=="string")p->type=typeid(std::string).name();
        if(key=="readonly")p->flags=ofxOceanodeParameterFlags_ReadOnly;
        node.group.values.push_back(p);
    },[&](const std::string& key){
        auto& v=node.group.values;v.erase(std::remove_if(v.begin(),v.end(),[&](auto& p){return p->name==key;}),v.end());
    },keys);
#ifdef OFXOCEANODE_USE_MIDI
    for(const auto& type:{std::string(typeid(float).name()),std::string(typeid(int).name()),
        std::string(typeid(bool).name()),std::string(typeid(void).name()),
        std::string(typeid(std::vector<float>).name()),std::string(typeid(std::vector<int>).name())}){
        auto p=std::make_shared<ofxOceanodeAbstractParameter>("visible");p->type=type;node.group.values.push_back(p);
        assert(menu.bindMidi("visible"));assert(!menu.isHidden("visible"));
        assert(host.midi.at("Synth_1-|-visible").back()->parameter==p.get());
        assert(menu.unbindLastMidi("visible"));assert(menu.parameter("visible")==p.get());
        node.group.values.clear();
    }
    for(auto key:{"string","readonly","missing"}){
        assert(!menu.bindMidi(key));assert(!menu.parameter(key));assert(!menu.isHidden(key));
    }
    node.host=nullptr;assert(!menu.bindMidi("cutoff"));assert(!menu.parameter("cutoff"));node.host=&host;
    // Test the actual shared popup entry point, including builds without timeline support.
    ImGui::click="Bind MIDI";menu.menuItems("cutoff");assert(ImGui::click.empty());
    auto* p=menu.parameter("cutoff");assert(p && menu.isHidden("cutoff"));
    assert(p->getFlags()&ofxOceanodeParameterFlags_NoGuiWidget);assert(menu.hasMidiBinding("cutoff"));
    menu.unpublish("cutoff");testTime=10;menu.update();assert(menu.parameter("cutoff")==p);
    assert(menu.bindMidi("cutoff"));assert(menu.unbindLastMidi("cutoff"));assert(menu.hasMidiBinding("cutoff"));
    menu.reveal("cutoff");assert(!menu.isHidden("cutoff"));assert(!(p->getFlags()&ofxOceanodeParameterFlags_NoGuiWidget));
    menu.unpublish("cutoff");assert(menu.isHidden("cutoff"));
    ofJson state;menu.save(state,"hidden");assert(state["hidden"]==ofJson::array({"cutoff"}));
    // Restoring hidden state allows bindings to load after the node.
    host.midi.clear();menu.load(state,"hidden");testTime=14;menu.update();assert(menu.parameter("cutoff"));
    host.createMidiBinding(*p);testTime=16;menu.update();assert(menu.parameter("cutoff"));
#if OFXOCEANODESC_HAS_TIMELINE
    host.timeline.bound=true;assert(menu.unbindLastMidi("cutoff"));assert(menu.parameter("cutoff"));
    host.timeline.bound=false;testTime=18;menu.update();assert(!menu.parameter("cutoff"));
#else
    ImGui::click="Unbind last MIDI";menu.menuItems("cutoff");assert(ImGui::click.empty());
    assert(!menu.parameter("cutoff"));
#endif
    assert(!menu.unbindLastMidi("cutoff"));
    assert(menu.bindMidi("persistent"));p=menu.parameter("persistent");
    host.persistent["Synth_1-|-persistent"].push_back(std::make_shared<Binding>(Binding{p}));
    assert(menu.unbindLastMidi("persistent"));assert(menu.parameter("persistent"));assert(menu.hasMidiBinding("persistent"));
    assert(!menu.unbindLastMidi("persistent"));
    testTime=30;menu.update();assert(menu.parameter("persistent"));
    host.persistent.clear();testTime=32;menu.update();assert(!menu.parameter("persistent"));
    // Empty native maps must not call removeLastMidiBinding (which assumes a nonempty list).
    assert(menu.bindMidi("external"));host.midi["Synth_1-|-external"].clear();
    assert(!menu.unbindLastMidi("external"));testTime=34;menu.update();assert(!menu.parameter("external"));
    ImGui::click="Bind MIDI";menu.menuItems("nested","Effect Mix");assert(menu.hasMidiBinding("nested"));
#else
    assert(!menu.bindMidi("cutoff"));assert(!menu.hasMidiBinding("cutoff"));assert(!menu.unbindLastMidi("cutoff"));
    menu.menuItems("cutoff");assert(!menu.parameter("cutoff"));
    assert(std::find(ImGui::seen.begin(),ImGui::seen.end(),"Bind MIDI")==ImGui::seen.end());
#endif
    std::cout<<"PASS: shared parameter MIDI menus, publication and binding lifecycle\n";
}
'''
with tempfile.TemporaryDirectory(prefix='parameter-menu-') as directory:
    out = Path(directory)
    for name, source in STUBS.items():
        (out / name).write_text(source)
    (out / 'test.cpp').write_text(TEST)
    for midi in (False, True):
        for timeline in (False, True):
            timeline_header = out / 'ofxOceanodeTimeline.h'
            if timeline:
                timeline_header.write_text('#include "testTimeline.h"\n')
            else:
                timeline_header.unlink(missing_ok=True)
            executable = out / f'test-{midi}-{timeline}'
            flags = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                     '-I' + str(out), '-I' + str(ROOT / 'src'),
                     '-I' + str(ROOT.parent.parent / 'libs/json/include'),
                     '-DOFXOCEANODESC_HAS_TIMELINE=' + str(int(timeline))]
            if midi:
                flags.append('-DOFXOCEANODE_USE_MIDI')
            subprocess.run(flags + [str(out / 'test.cpp'), str(ROOT / 'src/ui/scTimelineMenu.cpp'),
                           '-o', str(executable)], check=True)
            print(f'MIDI={midi}, timeline={timeline}', flush=True)
            subprocess.run([str(executable)], check=True)

# Every synth/editor with custom publish menus reaches the same shared actions.
for name in ('AdditiveBox', 'AliasingBox', 'AnalogBox', 'FMBox', 'GrainBoxPoly',
             'PhysicalBox', 'PulsarBox', 'SampleBox', 'SteamPipeBox', 'WavetableBox',
             'ArpeggiatorBox', 'ChordBox', 'EuclideanBox', 'MetroBox', 'RhythmBox',
             'SlicerBox', 'EchoBox'):
    directory = 'fmBox' if name == 'FMBox' else name[0].lower() + name[1:]
    stem = ROOT / 'src/nodes/boxSynths' / directory / ('sc' + name)
    text = '\n'.join(p.read_text() for p in (stem.with_suffix('.h'), stem.with_suffix('.cpp')) if p.exists())
    assert 'timeline.menuItems(' in text, name
assert 'timeline.menuItems(' in (ROOT / 'src/nodes/boxSynths/polyComb/scPolyComb.cpp').read_text()
print('PASS: all custom Box synth/editor menus and PolyComb use the shared actions')
