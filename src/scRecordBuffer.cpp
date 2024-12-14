#include "scRecordBuffer.h"
#include "serverManager.h"
#include "ofxSCBuffer.h"
#include "ofxSCSynth.h"
#include "ofxSCServer.h"

scRecordBuffer::scRecordBuffer(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC Record Buffer") {
    servers = outputServers;
    recordBuffer = nullptr;
    recordSynth = nullptr;
    isBufferAllocated = false;
}

scRecordBuffer::~scRecordBuffer() {
    stopRecording();
}

void scRecordBuffer::setup() {
    // Input setup
    addParameter(input.set("Input", nodePort()));
    addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
    addParameter(numChannels.set("N Chan", 1, 1, 32));
    addParameter(isRecording.set("Record", false));
    addParameter(saveButton.set("Save", false));
    addParameter(lastSavedPath.set("Last Save", ""));
    
    // Output setup
    addOutputParameter(bufferIndex.set("Buffer", {-1}, {-1}, {INT_MAX}));
    
    // Server selection listener
    listeners.push(serverIndex.newListener([this](int &i){
        if(isRecording) {
            stopRecording();
        }
        if(recordBuffer != nullptr) {
            recordBuffer->free();
            delete recordBuffer;
            recordBuffer = nullptr;
            isBufferAllocated = false;
            bufferIndex = {-1};
        }
    }));
    
    // Recording state listener
    listeners.push(isRecording.newListener([this](bool &recording){
        if(recording) {
            startRecording();
        } else {
            stopRecording();
        }
    }));
    
    // Save button listener
    listeners.push(saveButton.newListener([this](bool &save){
        if(save && recordBuffer != nullptr) {
            ofFileDialogResult result = ofSystemSaveDialog("recording.wav", "Save buffer as wav");
            
            if(result.bSuccess) {
                string path = result.getPath();
                lastSavedPath = path;
                
                ofxOscMessage m;
                m.setAddress("/b_write");
                m.addIntArg(recordBuffer->index);
                m.addStringArg(path);
                m.addStringArg("wav");
                m.addStringArg("float");
                servers[serverIndex]->getServer()->sendMsg(m);
            }
            saveButton = false;
        }
    }));
    
    // Input bus change listener
    listeners.push(input.newListener([this](nodePort &port){
        if(isRecording) {
            stopRecording();
        }
    }));
    
    // Channel count listener
    listeners.push(numChannels.newListener([this](int &channels){
        if(isRecording) {
            stopRecording();
        }
        if(recordBuffer != nullptr) {
            recordBuffer->free();
            delete recordBuffer;
            recordBuffer = nullptr;
            isBufferAllocated = false;
            bufferIndex = {-1};
        }
    }));
}

void scRecordBuffer::startRecording() {
    stopRecording();
    if(input->getNodeRef() == nullptr) {
        isRecording = false;
        return;
    }

    int bufferSize = SAMPLE_RATE * RECORD_SECONDS;
    recordBuffer = new ofxSCBuffer(bufferSize, numChannels, servers[serverIndex]->getServer());
    recordBuffer->alloc();
    
    // Send buffer query message
    ofxOscMessage m;
    m.setAddress("/b_query");
    m.addIntArg(recordBuffer->index);
    servers[serverIndex]->getServer()->sendMsg(m);
    ofSleepMillis(100);
    
    bufferIndex = {recordBuffer->index};

    // Load SynthDefs after buffer allocation
    std::string d = ofToDataPath("", true);
    std::string synthDefDir = d + "/Supercollider/Synthdefs/Recorder/";
    m.clear();
    m.setAddress("/d_loadDir");
    m.addStringArg(synthDefDir);
    servers[serverIndex]->getServer()->sendMsg(m);
    ofSleepMillis(100);

    std::string synthName = "Recorder" + ofToString(numChannels);
    recordSynth = new ofxSCSynth(synthName, servers[serverIndex]->getServer());
    recordSynth->create();
    recordSynth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
    recordSynth->set("buf", recordBuffer->index);
}



void scRecordBuffer::stopRecording() {
    if(recordSynth != nullptr) {
        recordSynth->free();
        delete recordSynth;
        recordSynth = nullptr;
    }
}
