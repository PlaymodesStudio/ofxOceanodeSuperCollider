//
//  serverManager.cpp
//  ofxOceanodeSupercollider
//
//  Created by Eduard Frigola on 31/8/23.
//

#include "serverManager.h"
#include "ofxOceanodeSuperCollider.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSuperCollider.h"
#include "scNode.h"
#include "scStart.h"

serverManager::serverManager(std::vector<std::string> _wavs){
    synth = nullptr;
    wavs = _wavs;
    volume = 1;
    mute = false;
    delay = 0;
    audioDevice = 0;
    dumpOsc = false;
};

serverManager::~serverManager(){
    for(auto node : nodesList) node->free(server);
    nodesList.clear();
    for(auto &b : busses) b.free();
    busses.clear();
    if(synth != nullptr){
        synth->free();
        delete synth;
    }
}

void serverManager::setup(){
    server = new ofxSCServer(preferences.bindAddress, preferences.udpPort);
    if(preferences.local){
        sc = new scStart(preferences);
    }
    boot();
}

void serverManager::draw(){
    if(ImGui::Button("Boot Server")){
        boot();
    }
    
    ImGui::SameLine();
    if(ImGui::Button("Kill Server")){
        if(preferences.local){
            kill();
        }
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Local", &preferences.local)){
        if(!preferences.local){
            sc->killServer();
            delete sc;
        }else{
            sc = new scStart(preferences);
        }
    }
    
    if(ImGui::Button("Load Defs")){
        loadDefs();
    }
    
    ImGui::Separator();
    
    if(ImGui::SliderFloat("Volume", &volume, 0, 1)){
        setVolume(volume);
    }
    
    ImGui::SameLine();
    
    if(ImGui::Checkbox("Mute", &mute)){
        if(mute) setVolume(0);
        else setVolume(volume);
    }
    
    if(ImGui::SliderInt("Delay", &delay, 0, 5000)){
        setDelay(delay);
    }
    
    ImGui::Separator();
    
    ImGui::InputInt("Udp Port", &preferences.udpPort);
    
    
    int intaddress[4] = {0, 0, 0, 0};
    vector<string> splitAddress = ofSplitString(preferences.bindAddress, ".");
    for(int i = 0; i < 4; i++){
        intaddress[i] = ofToInt(splitAddress[i]);
    }
    if(ImGui::InputInt4("Bind Address", &intaddress[0])){
        string newAddress = ofToString(intaddress[0]) + "."
                            + ofToString(intaddress[1]) + "."
                            + ofToString(intaddress[2]) + "."
                            + ofToString(intaddress[3]) + ".";
        preferences.bindAddress = newAddress;
    }
    
    ImGui::InputInt("Audio Busses", &preferences.numAudioBusChannels);
    ImGui::InputInt("Control Busses", &preferences.numControlBusChannels);
    ImGui::InputInt("Input Channels", &preferences.numInputBusChannels);
    ImGui::InputInt("Output Channels", &preferences.numOutputBusChannels);
    ImGui::InputInt("Block Size", &preferences.blockSize);
    ImGui::InputInt("Buffer Size", &preferences.hardwareBufferSize);
    ImGui::InputInt("Sampling Rate", &preferences.hardwareSampleRate);
    ImGui::InputInt("Num Buffers", &preferences.numBuffers);
    ImGui::InputInt("Max Nodes", &preferences.maxNodes);
    ImGui::InputInt("Max Synthdefs", &preferences.maxSynthDefs);
    ImGui::InputInt("Mem Size", &preferences.memSize);
    ImGui::InputInt("Num Wire Bufs", &preferences.numWireBufs);
    ImGui::InputInt("Num R Gens", &preferences.numRGens);
    ImGui::InputInt("Max Logins", &preferences.maxLogins);
    ImGui::InputFloat("Safety Clip Th", &preferences.safetyClipThreshold);
    
    auto vector_getter = [](void* vec, int idx, const char** out_text)
    {
        auto& vector = *static_cast<std::vector<std::string>*>(vec);
        if (idx < 0 || idx >= static_cast<int>(vector.size())) { return false; }
        *out_text = vector.at(idx).c_str();
        return true;
    };
    
    if(ImGui::Combo("Audio Device", &audioDevice, vector_getter, static_cast<void*>(&audioDeviceNames), audioDeviceNames.size())){
        if(audioDevice == 0){
            preferences.deviceName = "nil";
        }else{
            preferences.deviceName = audioDeviceNames[audioDevice];
        }
    }
    
    //Device name;
    bool verb = preferences.verbosity;
    if(ImGui::Checkbox("Verbosity", &verb)){
        if(verb) preferences.verbosity = 1;
        else preferences.verbosity = 0;
    }

    bool localUgens = (preferences.ugensPlugins != "");
    if(ImGui::Checkbox("LocalUgens", &localUgens)){
        if(localUgens)
            preferences.ugensPlugins = ofToDataPath("Supercollider/Ugens", true);
        else
            preferences.ugensPlugins = "";
    }
    
    if(ImGui::Checkbox("Dump Osc", &dumpOsc)){
        ofxOscMessage m;
        m.setAddress("/dumpOSC");
        if(dumpOsc) m.addIntArg(1);
        else m.addIntArg(0);
        server->sendMsg(m);
    }
}

void serverManager::boot(){
    if(preferences.local){
        sc->start();
        sleep(5);
    }
    ofxOscMessage m2;
    m2.setAddress("/g_new");
    m2.addIntArg(1);
    m2.addIntArg(0);
    m2.addIntArg(0);
    server->sendMsg(m2);
    
    ofxOscMessage m;
    m.setAddress("/d_loadDir");
    m.addStringArg(ofToDataPath("Supercollider/Synthdefs", true));
    m.addIntArg(0);
    server->sendMsg(m);
    
    sleep(1);
    synth = new ofxSCSynth("output", server);
    synth->addToTail();
    setVolume(volume);
    setDelay(delay);
    
    for(auto &w : wavs){
        buffers[w] = new ofxSCBuffer(0, 0, server);
        buffers[w]->readChannel(ofToDataPath("Supercollider/Samples/" + w, true), {0});
    }
}

void serverManager::kill(){
    if(preferences.local){
        ofxOscMessage m;
        m.setAddress("/quit");
        server->sendMsg(m);
        sleep(1);
        sc->killServer();
    }
}

void serverManager::loadDefs(){
    ofxOscMessage m;
    m.setAddress("/d_loadDir");
    m.addStringArg(ofToDataPath("Supercollider/Synthdefs", true));
    m.addIntArg(0);
    server->sendMsg(m);
}

void serverManager::setVolume(float _volume){
    volume = _volume;
    synth->set("levels", volume);
}

void serverManager::setDelay(int _delay){
    delay = _delay;
    synth->set("delay", delay);
}

void serverManager::setOutputChannel(int channel){
    synth->set("out", channel);
}

void serverManager::recomputeGraph(scNode* firstNode){
    if(firstNode == nullptr){
        for(auto node : nodesList) node->free(server);
        nodesList.clear();
        for(auto &b : busses) b.free();
        busses.clear();
    }else{
        server->setWaitToSend(true);
        std::vector<scNode*> newNodesList;
        std::map<scNode*, std::pair<int, std::vector<int>>> nodeChilds;
        if(firstNode != nullptr)
            while(firstNode->appendOrderedNodes(newNodesList, nodeChilds));
        
        //TODO: Only delete non existing nodes
        for(auto node : nodesList){
//            if(std::find(newNodesList.begin(), newNodesList.end(), node) == newNodesList.end()){
                node->free(server);
//            }else{
//                newNodesList.erase(std::remove(newNodesList.begin(), newNodesList.end(), node), newNodesList.end());
//            }
        }
        
        nodesList = newNodesList;
        
        std::map<scNode*, std::vector<scNode*>> connections;

        for (auto it = nodesList.rbegin(); it != nodesList.rend(); ++it) {
            (*it)->getConnections(connections);
            (*it)->createSynth(server);
        }
        
        for(auto &b : busses) b.free();
        busses.clear();
        outputBussesRefToNode.clear();
        inputBussesRefToNode.clear();
        
        busses.emplace_back(RATE_AUDIO, MAX_NODE_CHANNELS, server); //From server to first node
        int busindex = busses.back().index;
        firstNode->setOutputBus(server, busindex);
        outputBussesRefToNode[firstNode] = busindex;
        synth->set("in", busindex);
        for(auto &c : connections){
            busses.emplace_back(RATE_AUDIO, MAX_NODE_CHANNELS, server);
            busindex = busses.back().index;
            c.first->setOutputBus(server, busindex);
            outputBussesRefToNode[c.first] = busindex;
            for(auto &dest : c.second){
                dest->setInputBus(server, c.first, busindex);
                inputBussesRefToNode[dest].push_back(busindex);
            }
        }
        server->sendStoredBundle();
        server->setWaitToSend(false);
    }
    graphComputed.notify();
}

int serverManager::getOutputBusForNode(scNode* node){
    if(outputBussesRefToNode.count(node) == 1){
        return outputBussesRefToNode[node];
    }
    else{
        return -1;
    }
}
