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
#include <atomic>
#include <mutex>

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
        reboot = true;
        if(!isThreadRunning()) startThread();
    }

    void setPreferences(scPreferences _prefs){
        std::lock_guard<std::mutex> lock(preferencesMutex);
        prefs = _prefs;
    }

    void restartServer(scPreferences _prefs){
        {
            std::lock_guard<std::mutex> lock(preferencesMutex);
            prefs = _prefs;
        }
        reboot = true;
        if(isThreadRunning()){
            ofSystem("killall scsynth");
        }else{
            startThread();
        }
    }
      
    void threadedFunction(){
        while(reboot){
            uint64_t launchTime = ofGetElapsedTimeMillis();
            scPreferences launchPrefs;
            {
                std::lock_guard<std::mutex> lock(preferencesMutex);
                launchPrefs = prefs;
            }
            string termcmd = "\"" + scPath + "\"";
            termcmd += " -u " + ofToString(launchPrefs.udpPort);
            termcmd += " -B " + launchPrefs.bindAddress;
            termcmd += " -c " + ofToString(launchPrefs.numControlBusChannels);
            termcmd += " -a " + ofToString(launchPrefs.numAudioBusChannels);
            termcmd += " -i " + ofToString(launchPrefs.numInputBusChannels);
            termcmd += " -o " + ofToString(launchPrefs.numOutputBusChannels);
            termcmd += " -z " + ofToString(launchPrefs.blockSize);
            termcmd += " -Z " + ofToString(launchPrefs.hardwareBufferSize);
            termcmd += " -S " + ofToString(launchPrefs.hardwareSampleRate);
            termcmd += " -b " + ofToString(launchPrefs.numBuffers);
            termcmd += " -n " + ofToString(launchPrefs.maxNodes);
            termcmd += " -d " + ofToString(launchPrefs.maxSynthDefs);
            termcmd += " -m " + ofToString(launchPrefs.memSize);
            termcmd += " -w " + ofToString(launchPrefs.numWireBufs);
            termcmd += " -r " + ofToString(launchPrefs.numRGens);
            termcmd += " -l " + ofToString(launchPrefs.maxLogins);
            termcmd += " -s " + ofToString(launchPrefs.safetyClipThreshold);
            const bool hasOutputDevice = launchPrefs.deviceName != "nil" && !launchPrefs.deviceName.empty();
            const bool hasInputDevice = launchPrefs.inputDeviceName != "nil" && !launchPrefs.inputDeviceName.empty();
            if(!hasOutputDevice && !hasInputDevice){
                termcmd += " -H nil";
            }else{
                // scsynth's two-argument form: -H <inputDevice> <outputDevice>.
                // An empty string means "use the system default" for that side.
                termcmd += " -H " + shellQuote(hasInputDevice ? launchPrefs.inputDeviceName : "")
                         + " " + shellQuote(hasOutputDevice ? launchPrefs.deviceName : "");
            }
            termcmd += " -D 0 "; //Deactivate synthdefs
            std::string pluginsPath = scPath.substr(0, scPath.size()-7);
            if(launchPrefs.ugensPlugins != "") termcmd += " -U " + shellQuote(ofToDataPath(launchPrefs.ugensPlugins, true) + ":" + pluginsPath);
            
            ofLogNotice("scStart") << "Launching scsynth: " << termcmd;
            
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
                ret = nullptr;
            }
            if(reboot && ofGetElapsedTimeMillis() - launchTime < 1000){
                ofSleepMillis(1000);
            }
        }
    }
      
      
    ~scStart(){
        killServer();
    }
    
    void killServer(){
        reboot = false;
        ofSystem("killall scsynth");
        waitForThread(false);
    }
    
private:
    static std::string shellQuote(const std::string& value){
        std::string quoted = "'";
        for(char c : value){
            if(c == '\''){
                quoted += "'\\''";
            }else{
                quoted += c;
            }
        }
        quoted += "'";
        return quoted;
    }

    string scPath = "";
    scPreferences prefs;
    std::mutex preferencesMutex;
    std::atomic<bool> reboot{true};
    
    FILE * ret = nullptr;
};

#endif /* scStart_h */
