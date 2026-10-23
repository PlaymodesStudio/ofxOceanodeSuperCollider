#!/usr/bin/env python3
"""Exercise production visibility leases, deduplication and capture resource cleanup."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'src/serverManager.cpp').read_text()
a=source.index('const std::vector<float>* serverManager::requestFoldedWaveform(')
b=source.index('void serverManager::releaseGraphBusses()',a)
methods=source[a:b]
code=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
constexpr int RATE_CONTROL=1;
uint64_t frameNumber=0;double clockNow=0;
uint64_t ofGetFrameNum(){return frameNumber;}double ofGetElapsedTimef(){return clockNow;}
struct ofxSCServer {bool nrt=false;bool isNRTCapturing(){return nrt;}};
struct scNode{int bus=256;};
struct nodePort {scNode* node=nullptr;scNode* getNodeRef()const{return node;}int getBusIndex(ofxSCServer*)const{return node?node->bus:-1;}};
struct ofxSCSynth {
    static inline int created=0,freed=0;
    ofxSCSynth(const char*,ofxSCServer*){}
    void set(const char*,int){}
    void createAndRun(int,int,bool){++created;}void free(){++freed;}
};
struct ofxSCBus {
    static inline int allocated=0,freed=0,requests=0;
    int index=128;std::vector<float> readValues;
    ofxSCBus(int,int size,ofxSCServer*):readValues(size,.25f){++allocated;}
    void free(){++freed;}void requestValues(){++requests;}
};
struct serverManager {
    static constexpr int foldedWaveformSamples=64;
    struct FoldedWaveform {std::unique_ptr<ofxSCSynth>synth;std::unique_ptr<ofxSCBus>bus;uint64_t lastDrawFrame=0;};
    std::map<int,FoldedWaveform> foldedWaveforms;
    double lastFoldedWaveformPoll=-1;
    bool initialized=true,loading=false,definitionAvailable=true;
    int mixerGraphSyncID=-1;
    ofxSCServer* server;std::vector<scNode*> nodesList;
    bool loadFoldedWaveformDef(){return definitionAvailable;}
    bool areRequiredSynthdefsLoading(){return loading;}
    const std::vector<float>* requestFoldedWaveform(const nodePort&);
    void clearFoldedWaveforms(bool serverReset=false);void updateFoldedWaveforms();
};
'''+methods+r'''
int main(){
    ofxSCServer server;scNode a,b;nodePort port{&a},alias{&b};
    serverManager m;m.server=&server;m.nodesList={&a,&b};
    m.updateFoldedWaveforms();assert(ofxSCSynth::created==0 && ofxSCBus::requests==0);
    auto* waiting=m.requestFoldedWaveform(port);assert(waiting && waiting->empty());
    m.requestFoldedWaveform(alias);assert(m.foldedWaveforms.size()==1); // Macro/Portal aliases
    m.loading=true;m.updateFoldedWaveforms();assert(ofxSCSynth::created==0);
    m.loading=false;frameNumber=1;m.updateFoldedWaveforms();
    assert(ofxSCSynth::created==1 && ofxSCBus::requests==1);
    auto* data=m.requestFoldedWaveform(alias);assert(data && data->size()==64);
    frameNumber=2;clockNow=.01;m.updateFoldedWaveforms();assert(ofxSCBus::requests==1);
    m.requestFoldedWaveform(port);
    frameNumber=3;clockNow=.06;m.updateFoldedWaveforms();assert(ofxSCBus::requests==2);
    // No further visible draws: expand or cull the node.
    frameNumber=5;clockNow=.1;m.updateFoldedWaveforms();
    assert(m.foldedWaveforms.empty() && ofxSCSynth::freed==1 && ofxSCBus::freed==1);
    frameNumber=6;clockNow=.2;m.updateFoldedWaveforms();assert(ofxSCBus::requests==2);
    m.requestFoldedWaveform(port);m.updateFoldedWaveforms();assert(ofxSCSynth::created==2);
    server.nrt=true;m.updateFoldedWaveforms();assert(m.foldedWaveforms.empty());
    assert(!m.requestFoldedWaveform(port));
    server.nrt=false;m.requestFoldedWaveform(port);m.updateFoldedWaveforms();
    int freed=ofxSCBus::freed;m.clearFoldedWaveforms(true);assert(ofxSCBus::freed==freed);
    m.requestFoldedWaveform(port);m.updateFoldedWaveforms();m.clearFoldedWaveforms();
    m.nodesList.clear();assert(!m.requestFoldedWaveform(port));
    m.nodesList={&a};m.definitionAvailable=false;assert(!m.requestFoldedWaveform(port));
    assert(m.foldedWaveforms.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='folded-waveform-life-') as folder:
    work=Path(folder);cpp=work/'test.cpp';cpp.write_text(code)
    subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',str(cpp),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
print('PASS: no hidden capture, shared bus capture, sync fence, 20 Hz limit, expand/cull cleanup, NRT and reboot.')
