//
//  serverManager.h
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 31/8/23.
//

#ifndef serverManager_h
#define serverManager_h

#include <stdio.h>
#include <vector>
#include <map>
#include <string>
#include <ofConstants.h>
#include <ofEvent.h>

class ofxSCServer;
class ofxSCSynth;
class ofxSCBuffer;
class ofxSCBus;
class scNode;
class scStart;
class scOutput;

struct scPreferences{
    bool local = true;
    int udpPort = 57110; //u (0-65535)
    std::string bindAddress = "127.0.0.1"; //-B (set to 0.0.0.0 to listen to all)
    int numControlBusChannels = 16384; //c
    int numAudioBusChannels = 4096; //a
    int numInputBusChannels = 2; //i
    int numOutputBusChannels = 16; //o
    int blockSize = 64; //z
    int hardwareBufferSize = 64; //Z
    int hardwareSampleRate = 44100; //S
    int numBuffers = 1024; //b
    int maxNodes = 1024; //n
    int maxSynthDefs = 1024; //d
    int memSize = 2048576; //m
    int numWireBufs = 8192; //w
    int numRGens = 64; //r
    int maxLogins = 1; //l
    float safetyClipThreshold = 1.26; //s
    std::string deviceName = "nil"; //H
    int verbosity = 0;
    std::string ugensPlugins = ""; //-U (list of paths separated by :)
};

class serverManager{
public:
    serverManager(std::vector<std::string> wavs = {});
    ~serverManager();
    
    void setup();
    void draw();
    
    void boot();
    void kill();
    void loadDefs();
    
    void setVolume(float volume);
    void setDelay(int delay);
    
    void setStereoMix(bool stereomix);
    void setStereoMixSize(int stereomixSize);
    
    void setOutputChannel(int channel);
    void recomputeGraph();
    
    void addOutput(scOutput* output);
    void removeOutput(scOutput* output);
    
    void setAudioDevices(std::vector<std::string> audioDevices){audioDeviceNames = audioDevices;}
    
    ofxSCServer* getServer(){return server;}
    int getOutputBusForNode(scNode* node);
    
    void recomputeGraphOnce(){
        numRecomputeGraphOnce++;
        if(numRecomputeGraphOnce == outputs.size()){
            recomputeGraph();
            numRecomputeGraphOnce = 0;
        }
    }
    
    void resetRecomputeGraphOnce(){
        numRecomputeGraphOnce = 0;
    }
    
    scPreferences preferences;
    ofEvent<void> graphComputed;
private:

    std::vector<scNode*> connectedNodes; //List of all nodes
    
    ofEventListeners listeners;
    
    ofxSCSynth *synth;
    ofxSCServer *server;
    
    std::vector<scOutput*> outputs;
    std::vector<scNode*> nodesList;
    std::map<scNode*, std::map<int, std::vector<ofxSCSynth*>>> synthMap;
    
    std::vector<ofxSCBus> busses;
    std::map<scNode*, int> outputBussesRefToNode;
    std::map<scNode*, std::vector<int>> inputBussesRefToNode;
    
    std::map<std::string, ofxSCBuffer*> buffers;
    std::vector<std::string> wavs;
    
    float volume;
    bool mute;
    int delay;
    bool stereomix;
    int stereomixSize;
    int audioDevice;
    bool dumpOsc;
    std::vector<std::string> audioDeviceNames;
    scStart* sc;
    
    int numRecomputeGraphOnce;
};

#endif /* serverManager_h */
