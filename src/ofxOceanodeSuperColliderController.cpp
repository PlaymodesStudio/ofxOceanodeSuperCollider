//
//  ofxOceanodeSuperColliderController.cpp
//
//  Created by Eduard Frigola Bagué on 23/12/2022.
//

#include "ofxOceanodeSuperColliderController.h"
#include "scStart.h"
#include "ofxSCServer.h"
#include "imgui.h"
#include "scOut.h"

ofxOceanodeSuperColliderController::ofxOceanodeSuperColliderController() : ofxOceanodeBaseController("SuperCollider"){
    audioDevice = 0;
    volume = 0;
    mute = false;
    delay = 0;
    reloadAudioDevices();
}

void ofxOceanodeSuperColliderController::setScEngine(scStart* _scEngine){
    sc = _scEngine;
}

void ofxOceanodeSuperColliderController::setScServer(ofxSCServer* _scServer){
    scServer = _scServer;
}

void ofxOceanodeSuperColliderController::draw(){
    if(ImGui::Button("Boot Server")){
        sc->start();
        sleep(5);
        ofxOscMessage m2;
        m2.setAddress("/g_new");
        m2.addIntArg(1);
        m2.addIntArg(0);
        m2.addIntArg(0);
        scServer->sendMsg(m2);
        
        ofxOscMessage m;
        m.setAddress("/d_loadDir");
        m.addStringArg(ofToDataPath("Supercollider/Synthdefs", true));
        m.addIntArg(0);
        scServer->sendMsg(m);
    }
    
    ImGui::SameLine();
    if(ImGui::Button("Kill Server")){
        ofxOscMessage m;
        m.setAddress("/quit");
        scServer->sendMsg(m);
        sc->killServer();
    }
    
    ImGui::SameLine();
    
    ImGui::Checkbox("AutoStart", &sc->autoStart);
    
    if(ImGui::Button("Load Defs")){
        ofxOscMessage m;
        m.setAddress("/d_loadDir");
        m.addStringArg(ofToDataPath("Supercollider/Synthdefs", true));
        m.addIntArg(0);
        scServer->sendMsg(m);
    }
    
    ImGui::Separator();
    
    if(ImGui::SliderFloat("Volume", &volume, 0, 1)){
        for(auto &n : outputNodes){
            n->setVolume(volume);
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Mute", &mute)){
        for(auto &n : outputNodes){
            if(mute) n->setVolume(0);
            else n->setVolume(volume);
        }
    }
    
    if(ImGui::SliderInt("Delay", &delay, 0, 5000)){
        for(auto &n : outputNodes){
            n->setDelay(delay);
        }
    }
    
    ImGui::Separator();
    
    ImGui::InputInt("Udp Port", &sc->udpPort);
    
    
    int intaddress[4] = {0, 0, 0, 0};
    vector<string> splitAddress = ofSplitString(sc->bindAddress, ".");
    for(int i = 0; i < 4; i++){
        intaddress[i] = ofToInt(splitAddress[i]);
    }
    if(ImGui::InputInt4("Bind Address", &intaddress[0])){
        string newAddress = ofToString(intaddress[0]) + "."
                            + ofToString(intaddress[1]) + "."
                            + ofToString(intaddress[2]) + "."
                            + ofToString(intaddress[3]) + ".";
        sc->bindAddress = newAddress;
    }
    
    ImGui::InputInt("Audio Busses", &sc->numAudioBusChannels);
    ImGui::InputInt("Control Busses", &sc->numControlBusChannels);
    ImGui::InputInt("Input Channels", &sc->numInputBusChannels);
    ImGui::InputInt("Output Channels", &sc->numOutputBusChannels);
    ImGui::InputInt("Block Size", &sc->blockSize);
    ImGui::InputInt("Buffer Size", &sc->hardwareBufferSize);
    ImGui::InputInt("Sampling Rate", &sc->hardwareSampleRate);
    ImGui::InputInt("Num Buffers", &sc->numBuffers);
    ImGui::InputInt("Max Nodes", &sc->maxNodes);
    ImGui::InputInt("Max Synthdefs", &sc->maxSynthDefs);
    ImGui::InputInt("Mem Size", &sc->memSize);
    ImGui::InputInt("Num Wire Bufs", &sc->numWireBufs);
    ImGui::InputInt("Num R Gens", &sc->numRGens);
    ImGui::InputInt("Max Logins", &sc->maxLogins);
    ImGui::InputFloat("Safety Clip Th", &sc->safetyClipThreshold);
    
    auto vector_getter = [](void* vec, int idx, const char** out_text)
    {
        auto& vector = *static_cast<std::vector<std::string>*>(vec);
        if (idx < 0 || idx >= static_cast<int>(vector.size())) { return false; }
        *out_text = vector.at(idx).c_str();
        return true;
    };
    
    if(ImGui::Combo("Audio Device", &audioDevice, vector_getter, static_cast<void*>(&audioDeviceNames), audioDeviceNames.size())){
        if(audioDevice == 0){
            sc->deviceName = "nil";
        }else{
            sc->deviceName = audioDeviceNames[audioDevice];
        }
    }
    
    //Device name;
    bool verb = sc->verbosity;
    if(ImGui::Checkbox("Verbosity", &verb)){
        if(verb) sc->verbosity = 1;
        else sc->verbosity = 0;
    }

    bool localUgens = (sc->ugensPlugins != "");
    if(ImGui::Checkbox("LocalUgens", &localUgens)){
        if(localUgens)
            sc->ugensPlugins = ofToDataPath("Supercollider/Ugens", true);
        else
            sc->ugensPlugins = "";
    }
    
    if(ImGui::Checkbox("Dump Osc", &dumpOsc)){
        ofxOscMessage m;
        m.setAddress("/dumpOSC");
        if(dumpOsc) m.addIntArg(1);
        else m.addIntArg(0);
        scServer->sendMsg(m);
    }
    
    if(ImGui::Button("Save Settings")){
        sc->saveConfig();
    }
}


void ofxOceanodeSuperColliderController::reloadAudioDevices(){
    auto devices = ofSoundStreamListDevices();
    
    audioDeviceNames = {"Default"};
    for(auto &d : devices) audioDeviceNames.push_back(d.name);
}

void ofxOceanodeSuperColliderController::addOutput(scOut* node){
    outputNodes.push_back(node);
    if(mute) node->setVolume(0);
    else node->setVolume(volume);
    node->setDelay(delay);
}

void ofxOceanodeSuperColliderController::removeOutput(scOut* node){
    outputNodes.erase(std::remove(outputNodes.begin(), outputNodes.end(), node), outputNodes.end());

}
