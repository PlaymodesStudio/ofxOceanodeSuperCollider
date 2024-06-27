//
//  scStart.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 24/01/2022.
//

#ifndef scStart_h
#define scStart_h

#include "ofThread.h"
#include "serverManager.h"

// this needs to be threaded otherwise it blocks the main thread
class scStart : public ofThread{
  
public:
    scStart(scPreferences _prefs){
        prefs = _prefs;
        
        if(ofDirectory::doesDirectoryExist(ofToDataPath("Supercollider/Scsynth/bin/"))){
            scPath = ofToDataPath("Supercollider/Scsynth/bin/scsynth", true);
        }else if(ofDirectory::doesDirectoryExist("/Applications/SuperCollider.app/Contents/Resources/")){
            scPath = "/Applications/SuperCollider.app/Contents/Resources/scsynth";
        }else{
            ofLog() << "No ScSynth found on the system";
        }
    }
    
    void start(){
        startThread();
    }
      
    void threadedFunction(){
        string termcmd = scPath;
        termcmd += " -u " + ofToString(prefs.udpPort);
        termcmd += " -B " + prefs.bindAddress;
        termcmd += " -c " + ofToString(prefs.numControlBusChannels);
        termcmd += " -a " + ofToString(prefs.numAudioBusChannels);
        termcmd += " -i " + ofToString(prefs.numInputBusChannels);
        termcmd += " -o " + ofToString(prefs.numOutputBusChannels);
        termcmd += " -z " + ofToString(prefs.blockSize);
        termcmd += " -Z " + ofToString(prefs.hardwareBufferSize);
        termcmd += " -S " + ofToString(prefs.hardwareSampleRate);
        termcmd += " -b " + ofToString(prefs.numBuffers);
        termcmd += " -n " + ofToString(prefs.maxNodes);
        termcmd += " -d " + ofToString(prefs.maxSynthDefs);
        termcmd += " -m " + ofToString(prefs.memSize);
        termcmd += " -w " + ofToString(prefs.numWireBufs);
        termcmd += " -r " + ofToString(prefs.numRGens);
        termcmd += " -l " + ofToString(prefs.maxLogins);
        termcmd += " -s " + ofToString(prefs.safetyClipThreshold);
        termcmd += " -H " + prefs.deviceName;
        if(prefs.ugensPlugins != "") termcmd += " -U " + ofToString("\"") + ofToDataPath(prefs.ugensPlugins, true) + ofToString(":") + ofToDataPath("Supercollider/scsynth/aarch/bin", true) + ofToString("\"");
 
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
        ofSystem("killall scsynth");
    }
    
private:
    string scPath = "";
    scPreferences prefs;
};

#endif /* scStart_h */
