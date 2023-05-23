//
//  scStart.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 24/01/2022.
//

#ifndef scStart_h
#define scStart_h

#include "ofThread.h"

// this needs to be threaded otherwise it blocks the main thread
class scStart : public ofThread{
  
public:
    scStart(){}
      
    void setup(string scPath_){
        loadConfig();
        scPath = scPath_;
        if(autoStart){
            startThread();
        }
    }
    
    void start(){
        startThread();
    }
      
    void threadedFunction(){
        
        string termcmd = scPath;
        termcmd += " -u " + ofToString(udpPort);
        termcmd += " -B " + bindAddress;
        termcmd += " -c " + ofToString(numControlBusChannels);
        termcmd += " -a " + ofToString(numAudioBusChannels);
        termcmd += " -i " + ofToString(numInputBusChannels);
        termcmd += " -o " + ofToString(numOutputBusChannels);
        termcmd += " -z " + ofToString(blockSize);
        termcmd += " -Z " + ofToString(hardwareBufferSize);
        termcmd += " -S " + ofToString(hardwareSampleRate);
        termcmd += " -b " + ofToString(numBuffers);
        termcmd += " -n " + ofToString(maxNodes);
        termcmd += " -d " + ofToString(maxSynthDefs);
        termcmd += " -m " + ofToString(memSize);
        termcmd += " -w " + ofToString(numWireBufs);
        termcmd += " -r " + ofToString(numRGens);
        termcmd += " -l " + ofToString(maxLogins);
        termcmd += " -s " + ofToString(safetyClipThreshold);
        termcmd += " -H " + deviceName;
        if(ugensPlugins != "") termcmd += " -U " + ugensPlugins;
 
//        string termcmd = scPath + " -u 57110 -z 64 -Z 512 -w 2048 -a 1024 -n 16384 -m 65536 -i 2 -o 2 -R 0 -l 1";
          
//        cout << ofSystem(termcmd);
        FILE * ret = nullptr;
    #ifdef TARGET_WIN32
        ret = _popen(termcmd.c_str(),"r");
    #else
        ret = popen(termcmd.c_str(),"r");
    #endif

        string strret;
        int c;

        if (ret == nullptr){
            ofLogError("ofUtils") << "ofSystem(): error opening return file for command \"" << termcmd  << "\"";
        }else{
            c = fgetc (ret);
            string line;
            while (c != EOF) {
    //            std::cout << (char)c;
                strret += c;
                c = fgetc (ret);
                if(c != '\n'){
                    line += c;
                }else{
                    ofLog() << line;
                    line = "";
                }
            }
    #ifdef TARGET_WIN32
            _pclose (ret);
    #else
            pclose (ret);
    #endif
        }
    }
      
      
    ~scStart(){
        waitForThread(true);
    }
    
    void killServer(){
        waitForThread(true);
    }
    
    void saveConfig(){
        ofJson json;
        json["udpPort"] = udpPort;
        json["bindAddress"] = bindAddress;
        json["numControlBusChannels"] = numControlBusChannels;
        json["numAudioBusChannels"] = numAudioBusChannels;
        json["numInputBusChannels"] = numInputBusChannels;
        json["numOutputBusChannels"] = numOutputBusChannels;
        json["blockSize"] = blockSize;
        json["hardwareBufferSize"] = hardwareBufferSize;
        json["hardwareSampleRate"] = hardwareSampleRate;
        json["numBuffers"] = numBuffers;
        json["maxNodes"] = maxNodes;
        json["maxSynthDefs"] = maxSynthDefs;
        json["memSize"] = memSize;
        json["numWireBufs"] = numWireBufs;
        json["numRGens"] = numRGens;
        json["maxLogins"] = maxLogins;
        json["safetyClipThreshold"] = safetyClipThreshold;
        json["deviceName"] = deviceName;
        json["verbosity"] = verbosity;
        json["ugensPlugins"] = ugensPlugins;
        json["autoStart"] = autoStart;
        
        ofSavePrettyJson(ofToDataPath("Supercollider_config.json"), json);
    }
    
    void loadConfig(){
        ofJson json = ofLoadJson(ofToDataPath("Supercollider_config.json"));
        if(!json.empty()){
            udpPort = json["udpPort"];
            bindAddress = json["bindAddress"];
            numControlBusChannels = json["numControlBusChannels"];
            numAudioBusChannels = json["numAudioBusChannels"];
            numInputBusChannels = json["numInputBusChannels"];
            numOutputBusChannels = json["numOutputBusChannels"];
            blockSize = json["blockSize"];
            hardwareBufferSize = json["hardwareBufferSize"];
            hardwareSampleRate = json["hardwareSampleRate"];
            numBuffers = json["numBuffers"];
            maxNodes = json["maxNodes"];
            maxSynthDefs = json["maxSynthDefs"];
            memSize = json["memSize"];
            numWireBufs = json["numWireBufs"];
            numRGens = json["numRGens"];
            maxLogins = json["maxLogins"];
            safetyClipThreshold = json["safetyClipThreshold"];
            deviceName = json["deviceName"];
            verbosity = json["verbosity"];
            ugensPlugins = json["ugensPlugins"];
            autoStart = json["autoStart"];
        }
    }
      
    int udpPort = 57110; //u (0-65535)
    string bindAddress = "127.0.0.1"; //-B (set to 0.0.0.0 to listen to all)
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
    string ugensPlugins = ""; //-U (list of paths separated by :)
    
    
    bool autoStart;
    
private:
    string scPath = "";

};

#endif /* scStart_h */
