#ifndef scRecordBuffer_h
#define scRecordBuffer_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class serverManager;
class ofxSCBuffer;
class ofxSCSynth;

class scRecordBuffer : public ofxOceanodeNodeModel {
public:
    scRecordBuffer(vector<serverManager*> outputServers);
    ~scRecordBuffer();
    
    void setup();
    
private:
    void startRecording();
    void stopRecording();
    
    ofEventListeners listeners;
    
    // Input parameters
    ofParameter<nodePort> input;
    ofParameter<int> serverIndex;
    ofParameter<int> numChannels;
    ofParameter<bool> isRecording;
    ofParameter<bool> saveButton;
    ofParameter<string> lastSavedPath;
    
    // Output parameters
    ofParameter<vector<int>> bufferIndex;
    
    // Buffer and synth management
    ofxSCBuffer* recordBuffer;
    ofxSCSynth* recordSynth;
    vector<serverManager*> servers;
    bool isBufferAllocated;
    
    const int SAMPLE_RATE = 44100;
    const int RECORD_SECONDS = 30;
};

#endif /* scRecordBuffer_h */
