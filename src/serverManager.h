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

class ofxSCServer;
class ofxSCSynth;
class ofxSCBuffer;
class ofxSCBus;
class scNode;
class scStart;

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
    
    void setOutputChannel(int channel);
    void recomputeGraph(scNode* firstNode);
    
    void setAudioDevices(std::vector<std::string> audioDevices){audioDeviceNames = audioDevices;}
    
    ofxSCServer* getServer(){return server;}
    
    scPreferences preferences;
private:

    std::vector<scNode*> connectedNodes; //List of all nodes
    
    ofxSCSynth *synth;
    ofxSCServer *server;
    
    std::vector<scNode*> nodesList;
    std::map<scNode*, std::map<int, std::vector<ofxSCSynth*>>> synthMap;
    
    std::vector<ofxSCBus*> busses;
    
    std::map<std::string, ofxSCBuffer*> buffers;
    std::vector<std::string> wavs;
    
    float volume;
    bool mute;
    int delay;
    int audioDevice;
    bool dumpOsc;
    std::vector<std::string> audioDeviceNames;
    scStart* sc;
};

#endif /* serverManager_h */
