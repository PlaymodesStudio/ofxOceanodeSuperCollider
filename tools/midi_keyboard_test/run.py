#!/usr/bin/env python3
"""Exercise the shared production MIDI keyboard binding without MIDI hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
midi = root.parent / 'ofxOceanodeMidi/src'
binding = (midi / 'ofxOceanodeMidiKeyboardBinding.h').read_text()
for include in ['#pragma once', '#include "ofxOceanodeMidiBinding.h"', '#include "ofxOceanodeNodeModel.h"']:
    binding = binding.replace(include, '')
fixture = r'''
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
using ofJson = nlohmann::json;
using ofMutex = std::mutex;
template<class T> struct ofParameter {
    std::shared_ptr<T> value = std::make_shared<T>();
    ofParameter() = default;
    ofParameter(const T& v) : value(std::make_shared<T>(v)) {}
    const T& get() const { return *value; }
    void set(const T& v) { *value = v; }
    ofParameter& operator=(const T& v) { set(v); return *this; }
    operator const T&() const { return get(); }
};
struct ofxOceanodeAbstractParameter {
    ofParameter<std::vector<float>> parameter;
    ofxOceanodeAbstractParameter(std::vector<float> v) : parameter(v) {}
    std::string valueType() const { return typeid(std::vector<float>).name(); }
    template<class T> auto& cast() { return *this; }
    auto getParameter() { return parameter; }
};
struct ofxOceanodeTimelineInstrument {
    ofxOceanodeAbstractParameter *gate=nullptr, *pitch=nullptr, *velocity=nullptr;
    int voices=0; bool kit=false;
};
enum MidiStatus { MIDI_NOTE_ON, MIDI_NOTE_OFF, MIDI_CONTROL_CHANGE };
struct ofxMidiMessage {
    MidiStatus status=MIDI_NOTE_ON;
    int pitch=60, velocity=100, channel=1, control=0, value=0;
    std::string portName="Keyboard";
    static std::string getStatusString(MidiStatus) { return "Note On"; }
};
struct ofxOceanodeAbstractMidiBinding {
    virtual ~ofxOceanodeAbstractMidiBinding() = default;
    virtual void newMidiMessage(ofxMidiMessage&) {}
    virtual void update() {}
    virtual void savePreset(ofJson& j) { j={{"Port",portName},{"Channel",channel.get()},{"Type",messageType.get()}}; }
    virtual void loadPreset(ofJson& j) { portName=j["Port"]; channel=j["Channel"].get<int>(); isListening=false; }
    struct Event { void notify(void*, std::string&) {} } unregisterUnusedMidiIns;
    ofMutex mutex;
    std::vector<ofxMidiMessage> lastsMidiMessage;
    bool isListening=true;
    std::string name, portName;
    ofParameter<int> channel;
    ofParameter<std::string> messageType;
    MidiStatus status=MIDI_NOTE_ON;
};
'''
container_source = (root.parent / 'ofxOceanode/src/Managers/ofxOceanodeContainer.cpp').read_text()
start = container_source.index('void ofxOceanodeContainer::drawMidiKeyboardMenu(')
end = container_source.index('bool ofxOceanodeContainer::removeLastMidiBinding(', start)
menu = container_source[start:end]
menu_fixture = r'''
#include <map>
using namespace std;
namespace ImGui {
    std::string clicked, offered;
    void Separator() {}
    bool Selectable(const char* label) { offered=label; return clicked==label; }
    bool IsItemHovered() { return false; }
    void SetTooltip(const char*) {}
}
struct Group { std::string getEscapedName() { return "Synth_1"; } };
struct ofxOceanodeNodeModel {
    ofxOceanodeTimelineInstrument instrument;
    Group group;
    Group& getParameterGroup() { return group; }
    bool getMidiKeyboardInstrument(ofxOceanodeTimelineInstrument& out, bool = false) { out=instrument; return true; }
};
struct Node { bool getIsPersistent() { return false; } };
struct ofxOceanodeContainer {
    using Map=std::map<std::string,std::vector<std::shared_ptr<ofxOceanodeAbstractMidiBinding>>>;
    Map midiBindings,persistentMidiBindings;
    std::map<std::string,Node*> parameterGroupNodesMap;
    struct Input { int removed=0; void removeListener(ofxOceanodeAbstractMidiBinding*) { ++removed; } };
    std::map<std::string,Input> midiIns;
    int creates=0;
    void createMidiBinding(ofxOceanodeAbstractParameter&,bool,int,bool keyboard) { assert(keyboard); ++creates; }
    void drawMidiKeyboardMenu(ofxOceanodeNodeModel&);
};
'''
tests = r'''
int main() {
    using Binding=ofxOceanodeMidiKeyboardBinding;
    ofxOceanodeAbstractParameter p({60}),g({0}),v({1});
    ofxOceanodeTimelineInstrument i; i.pitch=&p; i.gate=&g; i.velocity=&v; i.voices=2;
    auto send=[](Binding& b, MidiStatus status,int note,int value=0,int ch=1,std::string port="Keyboard") {
        ofxMidiMessage m; m.status=status; m.pitch=note; m.velocity=value; m.channel=ch; m.portName=port;
        m.control=note; m.value=value; b.newMidiMessage(m);
    };
    auto active=[&](int pitch) {
        auto& ps=p.parameter.get(); auto& gs=g.parameter.get();
        for(size_t n=0;n<std::min(ps.size(),gs.size());++n) if(gs[n]>0&&ps[n]==pitch) return true;
        return false;
    };
    ofJson saved;
    {
        Binding b([&]{return i;},"Synth");
        send(b,MIDI_CONTROL_CHANGE,1,127); b.update(); // CC must not learn.
        b.savePreset(saved); assert(saved["Port"]=="");
        send(b,MIDI_NOTE_ON,60,100); assert(g.parameter.get()[0]==0); // callback only queues
        b.update(); assert(active(60)); assert(v.parameter.get()[0]==100.f/127.f);
        send(b,MIDI_NOTE_ON,64,100); b.update(); assert(active(60)&&active(64));
        send(b,MIDI_NOTE_ON,67,100); b.update(); assert(!active(60)&&active(64)&&active(67));
        send(b,MIDI_NOTE_OFF,67); b.update(); assert(active(60)&&active(64));
        send(b,MIDI_NOTE_ON,70,127,2); send(b,MIDI_NOTE_ON,71,127,1,"Other"); b.update();
        assert(!active(70)&&!active(71));
        send(b,MIDI_CONTROL_CHANGE,64,127); send(b,MIDI_NOTE_OFF,64); b.update(); assert(active(64));
        send(b,MIDI_CONTROL_CHANGE,64,0); b.update(); assert(!active(64)&&active(60));
        i.voices=1; b.update(); assert(g.parameter.get()[1]==0&&active(60));
        send(b,MIDI_NOTE_ON,62,100); b.update(); assert(active(62)&&!active(60));
        send(b,MIDI_NOTE_ON,62,0); b.update(); assert(active(60));
        b.savePreset(saved); assert(saved["Keyboard"]==true&&saved["Port"]=="Keyboard");
        b.release(); assert(!active(60));
        send(b,MIDI_NOTE_ON,60,100); send(b,MIDI_NOTE_OFF,60); b.update(); assert(!active(60));
        send(b,MIDI_NOTE_ON,63,100); b.update(); assert(active(63));
        send(b,MIDI_CONTROL_CHANGE,123,0); b.update(); assert(!active(63));
        send(b,MIDI_NOTE_ON,60,100); b.update(); assert(active(60));
    }
    assert(!active(60)); // destroying or unbinding closes owned gates
    {
        Binding b([&]{return i;},"Synth"); b.loadPreset(saved);
        send(b,MIDI_NOTE_ON,65,100,2); b.update(); assert(!active(65));
        send(b,MIDI_NOTE_ON,65,100); b.update(); assert(active(65));
        i.gate=nullptr; b.update(); assert(!active(65)); i.gate=&g;
        b.release();
        i.kit=true; i.voices=3; i.pitch=nullptr;
        send(b,MIDI_NOTE_ON,36,100); send(b,MIDI_NOTE_ON,38,100); b.update();
        assert(g.parameter.get()[0]==1&&g.parameter.get()[1]==0&&g.parameter.get()[2]==1);
        send(b,MIDI_NOTE_OFF,36); b.update(); assert(g.parameter.get()[0]==0&&g.parameter.get()[2]==1);
    }
    {
        i.kit=false; i.pitch=&p; i.voices=1;
        Binding b([&]{return i;},"Synth"); ofJson unlearned={{"Port",""}}; b.loadPreset(unlearned);
        send(b,MIDI_NOTE_ON,72,100,3,"Fresh"); b.update(); assert(active(72));
    }
    {
        ofxOceanodeContainer container;
        ofxOceanodeNodeModel model; model.instrument=i;
        ImGui::clicked="Bind MIDI Keyboard (Pitch / Gate)";
        container.drawMidiKeyboardMenu(model); assert(container.creates==1);
        container.midiIns["Keyboard"]={};
        auto regular=std::make_shared<ofxOceanodeAbstractMidiBinding>();
        container.midiBindings["Synth_1-|-Pitch"]={regular,std::make_shared<Binding>([&]{return i;},"Synth")};
        container.midiBindings["Other_1-|-Pitch"]={std::make_shared<Binding>([&]{return i;},"Other")};
        container.persistentMidiBindings["Synth_1-|-Gate"]={std::make_shared<Binding>([&]{return i;},"Synth")};
        ImGui::clicked="Unbind MIDI Keyboard (Pitch / Gate)";
        container.drawMidiKeyboardMenu(model);
        assert(container.midiBindings["Synth_1-|-Pitch"].size()==1);
        assert(container.midiBindings["Synth_1-|-Pitch"][0]==regular);
        assert(container.midiBindings["Other_1-|-Pitch"].size()==1);
        assert(container.persistentMidiBindings.empty());
        assert(container.midiIns["Keyboard"].removed==2);
    }
    std::cout<<"PASS: learning, queued writes, polyphony, voice stealing/return, voice resize, velocity, sustain, note-off, panic, filtering, preset restore, teardown, kits and bind/unbind menu isolation\n";
}
'''
with tempfile.TemporaryDirectory(prefix='midi-keyboard-test-') as directory:
    source=Path(directory)/'test.cpp'
    source.write_text(fixture+binding+menu_fixture+menu+tests)
    binary=Path(directory)/'test'
    json_include=root.parent.parent/'libs/json/include'
    subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-g', '-I'+str(midi),'-I'+str(json_include),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
