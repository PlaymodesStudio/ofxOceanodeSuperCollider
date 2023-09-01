//
//  ofxOceanodeSuperColliderController.cpp
//
//  Created by Eduard Frigola Bagué on 23/12/2022.
//

#include "ofxOceanodeSuperColliderController.h"
#include "scStart.h"
#include "ofxSCServer.h"
#include "imgui.h"
#include "serverManager.h"

ofxOceanodeSuperColliderController::ofxOceanodeSuperColliderController() : ofxOceanodeBaseController("SuperCollider"){
    volume = 0;
    mute = false;
    delay = 0;
    reloadAudioDevices();
}

void ofxOceanodeSuperColliderController::createServers(vector<string> wavs){
    ofDirectory dir;
    dir.open(ofToDataPath("Supercollider/Config/"));
    if(dir.exists()){
        dir.sort();
        for(auto f : dir.getFiles()){
            outputServers.push_back(new serverManager(wavs));
            loadConfig(f.getAbsolutePath(), outputServers.back()->preferences);
            outputServers.back()->setAudioDevices(audioDeviceNames);
        }
    }else{
        dir.createDirectory("Supercollider/Config/");
    }
    //If no config found, create just one server with default settings
    if(outputServers.size() == 0){
        outputServers.push_back(new serverManager());
        outputServers.back()->setAudioDevices(audioDeviceNames);
    }
}

void ofxOceanodeSuperColliderController::setup(){
    for(auto s : outputServers) s->setup();
}

void ofxOceanodeSuperColliderController::draw(){
    if(ImGui::Button("Boot Servers")){
        for(auto s : outputServers) s->boot();
    }
    
    ImGui::SameLine();
    if(ImGui::Button("Kill Server")){
        for(auto s : outputServers) s->kill();
    }
    
    ImGui::SameLine();
    
    if(ImGui::Button("Load Defs")){
        for(auto s : outputServers) s->loadDefs();
    }
    
    ImGui::Separator();
    
    if(ImGui::SliderFloat("Master Volume", &volume, 0, 1)){
        for(auto &n : outputServers){
            n->setVolume(volume);
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Master Mute", &mute)){
        for(auto &n : outputServers){
            if(mute) n->setVolume(0);
            else n->setVolume(volume);
        }
    }
    
    if(ImGui::SliderInt("Master Delay", &delay, 0, 5000)){
        for(auto &n : outputServers){
            n->setDelay(delay);
        }
    }
    
    ImGui::Separator();
    
    for(int i = 0; i < outputServers.size(); i++){
        if(ImGui::TreeNode(("Server " + ofToString(i)).c_str())){
            outputServers[i]->draw();
            ImGui::TreePop();
        }
    }
    
    if(ImGui::Button("Save Settings")){
        for(int i = 0; i < outputServers.size(); i++){
            saveConfig("Supercollider/Config/ServerPreferences_" + ofToString(i) + ".json", outputServers[i]->preferences);
        }
    }
}

void ofxOceanodeSuperColliderController::killServers(){
    for(auto s : outputServers) s->kill();
}


void ofxOceanodeSuperColliderController::reloadAudioDevices(){
    auto devices = ofSoundStreamListDevices();
    
    audioDeviceNames = {"Default"};
    for(auto &d : devices) audioDeviceNames.push_back(d.name);
}


void ofxOceanodeSuperColliderController::saveConfig(std::string filepath, scPreferences prefs){
    ofJson json;
    json["local"] = prefs.local;
    json["udpPort"] = prefs.udpPort;
    json["bindAddress"] = prefs.bindAddress;
    json["numControlBusChannels"] = prefs.numControlBusChannels;
    json["numAudioBusChannels"] = prefs.numAudioBusChannels;
    json["numInputBusChannels"] = prefs.numInputBusChannels;
    json["numOutputBusChannels"] = prefs.numOutputBusChannels;
    json["blockSize"] = prefs.blockSize;
    json["hardwareBufferSize"] = prefs.hardwareBufferSize;
    json["hardwareSampleRate"] = prefs.hardwareSampleRate;
    json["numBuffers"] = prefs.numBuffers;
    json["maxNodes"] = prefs.maxNodes;
    json["maxSynthDefs"] = prefs.maxSynthDefs;
    json["memSize"] = prefs.memSize;
    json["numWireBufs"] = prefs.numWireBufs;
    json["numRGens"] = prefs.numRGens;
    json["maxLogins"] = prefs.maxLogins;
    json["safetyClipThreshold"] = prefs.safetyClipThreshold;
    json["deviceName"] = prefs.deviceName;
    json["verbosity"] = prefs.verbosity;
    json["ugensPlugins"] = prefs.ugensPlugins;
    
    ofSavePrettyJson(filepath, json);
}

void ofxOceanodeSuperColliderController::loadConfig(std::string filepath, scPreferences &prefs){
    ofJson json = ofLoadJson(filepath);
    if(!json.empty()){
        prefs.local = json["local"];
        prefs.udpPort = json["udpPort"];
        prefs.bindAddress = json["bindAddress"];
        prefs.numControlBusChannels = json["numControlBusChannels"];
        prefs.numAudioBusChannels = json["numAudioBusChannels"];
        prefs.numInputBusChannels = json["numInputBusChannels"];
        prefs.numOutputBusChannels = json["numOutputBusChannels"];
        prefs.blockSize = json["blockSize"];
        prefs.hardwareBufferSize = json["hardwareBufferSize"];
        prefs.hardwareSampleRate = json["hardwareSampleRate"];
        prefs.numBuffers = json["numBuffers"];
        prefs.maxNodes = json["maxNodes"];
        prefs.maxSynthDefs = json["maxSynthDefs"];
        prefs.memSize = json["memSize"];
        prefs.numWireBufs = json["numWireBufs"];
        prefs.numRGens = json["numRGens"];
        prefs.maxLogins = json["maxLogins"];
        prefs.safetyClipThreshold = json["safetyClipThreshold"];
        prefs.deviceName = json["deviceName"];
        prefs.verbosity = json["verbosity"];
        prefs.ugensPlugins = json["ugensPlugins"];
    }
}
